#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <xen/grant_table.h>
#include <xen/events.h>
#include <xen/xenbus.h>
#include <asm/xen/page.h>
#include "hyper_dmabuf_xen_comm.h"
#include "hyper_dmabuf_xen_comm_list.h"
#include "../hyper_dmabuf_imp.h"
#include "../hyper_dmabuf_list.h"
#include "../hyper_dmabuf_msg.h"

static int export_req_id = 0;

struct hyper_dmabuf_ring_rq req_pending = {0};

/* Creates entry in xen store that will keep details of all exporter rings created by this domain */
int32_t hyper_dmabuf_setup_data_dir()
{
	char buf[255];

	sprintf(buf, "/local/domain/%d/data/hyper_dmabuf", hyper_dmabuf_get_domid());
	return xenbus_mkdir(XBT_NIL, buf, "");
}


/* Removes entry from xenstore with exporter ring details.
 * Other domains that has connected to any of exporter rings created by this domain,
 * will be notified about removal of this entry and will treat that as signal to
 * cleanup importer rings created for this domain
 */
int32_t hyper_dmabuf_destroy_data_dir()
{
	char buf[255];

	sprintf(buf, "/local/domain/%d/data/hyper_dmabuf", hyper_dmabuf_get_domid());
	return xenbus_rm(XBT_NIL, buf, "");
}

/*
 * Adds xenstore entries with details of exporter ring created for given remote domain.
 * It requires special daemon running in dom0 to make sure that given remote domain will
 * have right permissions to access that data.
 */
static int32_t hyper_dmabuf_expose_ring_details(uint32_t domid, uint32_t rdomid, uint32_t grefid, uint32_t port)
{
	char buf[255];
	int ret;

	sprintf(buf, "/local/domain/%d/data/hyper_dmabuf/%d", domid, rdomid);
	ret = xenbus_printf(XBT_NIL, buf, "grefid", "%d", grefid);

	if (ret) {
		printk("Failed to write xenbus entry %s: %d\n", buf, ret);
		return ret;
	}

	ret = xenbus_printf(XBT_NIL, buf, "port", "%d", port);

	if (ret) {
		printk("Failed to write xenbus entry %s: %d\n", buf, ret);
		return ret;
	}

	return 0;
}

/*
 * Queries details of ring exposed by remote domain.
 */
static int32_t hyper_dmabuf_get_ring_details(uint32_t domid, uint32_t rdomid, uint32_t *grefid, uint32_t *port)
{
	char buf[255];
	int ret;

	sprintf(buf, "/local/domain/%d/data/hyper_dmabuf/%d", rdomid, domid);
	ret = xenbus_scanf(XBT_NIL, buf, "grefid", "%d", grefid);

	if (ret <= 0) {
		printk("Failed to read xenbus entry %s: %d\n", buf, ret);
		return ret;
	}

	ret = xenbus_scanf(XBT_NIL, buf, "port", "%d", port);

	if (ret <= 0) {
		printk("Failed to read xenbus entry %s: %d\n", buf, ret);
		return ret;
	}

	return (ret <= 0 ? 1 : 0);
}

int32_t hyper_dmabuf_get_domid(void)
{
	struct xenbus_transaction xbt;
	int32_t domid;

        xenbus_transaction_start(&xbt);

        if (!xenbus_scanf(xbt, "domid","", "%d", &domid)) {
		domid = -1;
        }
        xenbus_transaction_end(xbt, 0);

	return domid;
}

int hyper_dmabuf_next_req_id_export(void)
{
        export_req_id++;
        return export_req_id;
}

/* For now cache latast rings as global variables TODO: keep them in list*/
static irqreturn_t hyper_dmabuf_front_ring_isr(int irq, void *info);
static irqreturn_t hyper_dmabuf_back_ring_isr(int irq, void *info);

/*
 * Callback function that will be called on any change of xenbus path being watched.
 * Used for detecting creation/destruction of remote domain exporter ring.
 * When remote domain's exporter ring will be detected, importer ring on this domain will be created.
 * When remote domain's exporter ring destruction will be detected it will celanup this domain importer ring.
 * Destruction can be caused by unloading module by remote domain or it's crash/force shutdown.
 */
static void remote_domain_exporter_watch_cb(struct xenbus_watch *watch,
				   const char *path, const char *token)
{
	int rdom,ret;
	uint32_t grefid, port;
	struct hyper_dmabuf_ring_info_import *ring_info;

	/* Check which domain has changed its exporter rings */
	ret = sscanf(watch->node, "/local/domain/%d/", &rdom);
	if (ret <= 0) {
		return;
	}

	/* Check if we have importer ring for given remote domain alrady created */
	ring_info = hyper_dmabuf_find_importer_ring(rdom);

	/*
	 * Try to query remote domain exporter ring details - if that will fail and we have
	 * importer ring that means remote domains has cleanup its exporter ring, so our
	 * importer ring is no longer useful.
	 * If querying details will succeed and we don't have importer ring, it means that
	 * remote domain has setup it for us and we should connect to it.
	 */
	ret = hyper_dmabuf_get_ring_details(hyper_dmabuf_get_domid(), rdom, &grefid, &port);

	if (ring_info && ret != 0) {
		printk("Remote exporter closed, cleaninup importer\n");
		hyper_dmabuf_importer_ringbuf_cleanup(rdom);
	} else if (!ring_info && ret == 0) {
		printk("Registering importer\n");
		hyper_dmabuf_importer_ringbuf_init(rdom);
	}
}

/* exporter needs to generated info for page sharing */
int hyper_dmabuf_exporter_ringbuf_init(int rdomain)
{
	struct hyper_dmabuf_ring_info_export *ring_info;
	struct hyper_dmabuf_sring *sring;
	struct evtchn_alloc_unbound alloc_unbound;
	struct evtchn_close close;

	void *shared_ring;
	int ret;

	ring_info = (struct hyper_dmabuf_ring_info_export*)
				kmalloc(sizeof(*ring_info), GFP_KERNEL);

	/* from exporter to importer */
	shared_ring = (void *)__get_free_pages(GFP_KERNEL, 1);
	if (shared_ring == 0) {
		return -EINVAL;
	}

	sring = (struct hyper_dmabuf_sring *) shared_ring;

	SHARED_RING_INIT(sring);

	FRONT_RING_INIT(&(ring_info->ring_front), sring, PAGE_SIZE);

	ring_info->gref_ring = gnttab_grant_foreign_access(rdomain,
							virt_to_mfn(shared_ring), 0);
	if (ring_info->gref_ring < 0) {
		return -EINVAL; /* fail to get gref */
	}

	alloc_unbound.dom = DOMID_SELF;
	alloc_unbound.remote_dom = rdomain;
	ret = HYPERVISOR_event_channel_op(EVTCHNOP_alloc_unbound,
					&alloc_unbound);
	if (ret != 0) {
		printk("Cannot allocate event channel\n");
		return -EINVAL;
	}

	/* setting up interrupt */
	ret = bind_evtchn_to_irqhandler(alloc_unbound.port,
					hyper_dmabuf_front_ring_isr, 0,
					NULL, (void*) ring_info);

	if (ret < 0) {
		printk("Failed to setup event channel\n");
		close.port = alloc_unbound.port;
		HYPERVISOR_event_channel_op(EVTCHNOP_close, &close);
		gnttab_end_foreign_access(ring_info->gref_ring, 0,
					virt_to_mfn(shared_ring));
		return -EINVAL;
	}

	ring_info->rdomain = rdomain;
	ring_info->irq = ret;
	ring_info->port = alloc_unbound.port;

	printk("%s: allocated eventchannel gref %d  port: %d  irq: %d\n",
		__func__,
		ring_info->gref_ring,
		ring_info->port,
		ring_info->irq);

	ret = hyper_dmabuf_register_exporter_ring(ring_info);

	ret = hyper_dmabuf_expose_ring_details(hyper_dmabuf_get_domid(), rdomain,
                                               ring_info->gref_ring, ring_info->port);

	/*
	 * Register watch for remote domain exporter ring.
	 * When remote domain will setup its exporter ring, we will automatically connect our importer ring to it.
	 */
	ring_info->watch.callback = remote_domain_exporter_watch_cb;
	ring_info->watch.node = (const char*) kmalloc(sizeof(char) * 255, GFP_KERNEL);
	sprintf((char*)ring_info->watch.node, "/local/domain/%d/data/hyper_dmabuf/%d/port", rdomain, hyper_dmabuf_get_domid());
	register_xenbus_watch(&ring_info->watch);

	return ret;
}

/* cleans up exporter ring created for given remote domain */
void hyper_dmabuf_exporter_ringbuf_cleanup(int rdomain)
{
	struct hyper_dmabuf_ring_info_export *ring_info;

	/* check if we at all have exporter ring for given rdomain */
	ring_info = hyper_dmabuf_find_exporter_ring(rdomain);

	if (!ring_info) {
		return;
	}

	hyper_dmabuf_remove_exporter_ring(rdomain);

	unregister_xenbus_watch(&ring_info->watch);
	kfree(ring_info->watch.node);

	/* No need to close communication channel, will be done by this function */
	unbind_from_irqhandler(ring_info->irq,	(void*) ring_info);

	/* No need to free sring page, will be freed by this function when other side will end its access */
	gnttab_end_foreign_access(ring_info->gref_ring, 0,
				  (unsigned long) ring_info->ring_front.sring);

	kfree(ring_info);
}

/* importer needs to know about shared page and port numbers for ring buffer and event channel */
int hyper_dmabuf_importer_ringbuf_init(int sdomain)
{
	struct hyper_dmabuf_ring_info_import *ring_info;
	struct hyper_dmabuf_sring *sring;

	struct page *shared_ring;

	struct gnttab_map_grant_ref *ops;
	int ret;
	int importer_gref, importer_port;

	ret = hyper_dmabuf_get_ring_details(hyper_dmabuf_get_domid(), sdomain,
					    &importer_gref, &importer_port);

	if (ret) {
		printk("Domain %d has not created exporter ring for current domain\n", sdomain);
		return ret;
	}

	ring_info = (struct hyper_dmabuf_ring_info_import *)
			kmalloc(sizeof(*ring_info), GFP_KERNEL);

	ring_info->sdomain = sdomain;
	ring_info->evtchn = importer_port;

	ops = (struct gnttab_map_grant_ref*)kmalloc(sizeof(*ops), GFP_KERNEL);

	if (gnttab_alloc_pages(1, &shared_ring)) {
		return -EINVAL;
	}

	gnttab_set_map_op(&ops[0], (unsigned long)pfn_to_kaddr(page_to_pfn(shared_ring)),
			GNTMAP_host_map, importer_gref, sdomain);
	gnttab_set_unmap_op(&ring_info->unmap_op, (unsigned long)pfn_to_kaddr(page_to_pfn(shared_ring)),
			GNTMAP_host_map, -1);

	ret = gnttab_map_refs(ops, NULL, &shared_ring, 1);
	if (ret < 0) {
		printk("Cannot map ring\n");
		return -EINVAL;
	}

	if (ops[0].status) {
		printk("Ring mapping failed\n");
		return -EINVAL;
	} else {
		ring_info->unmap_op.handle = ops[0].handle;
	}

	sring = (struct hyper_dmabuf_sring*) pfn_to_kaddr(page_to_pfn(shared_ring));

	BACK_RING_INIT(&ring_info->ring_back, sring, PAGE_SIZE);

	ret = bind_interdomain_evtchn_to_irqhandler(sdomain, importer_port,
						hyper_dmabuf_back_ring_isr, 0,
						NULL, (void*)ring_info);
	if (ret < 0) {
		return -EINVAL;
	}

	ring_info->irq = ret;

	printk("%s: bound to eventchannel port: %d  irq: %d\n", __func__,
		importer_port,
		ring_info->irq);

	ret = hyper_dmabuf_register_importer_ring(ring_info);

	/* Setup communcation channel in opposite direction */
	if (!hyper_dmabuf_find_exporter_ring(sdomain)) {
		ret = hyper_dmabuf_exporter_ringbuf_init(sdomain);
	}

	return ret;
}

/* clenas up importer ring create for given source domain */
void hyper_dmabuf_importer_ringbuf_cleanup(int sdomain)
{
	struct hyper_dmabuf_ring_info_import *ring_info;
	struct page *shared_ring;

	/* check if we have importer ring created for given sdomain */
	ring_info = hyper_dmabuf_find_importer_ring(sdomain);

	if (!ring_info)
		return;

	hyper_dmabuf_remove_importer_ring(sdomain);

	/* no need to close event channel, will be done by that function */
	unbind_from_irqhandler(ring_info->irq,	(void*) ring_info);

	/* unmapping shared ring page */
	shared_ring = virt_to_page(ring_info->ring_back.sring);
	gnttab_unmap_refs(&ring_info->unmap_op, NULL, &shared_ring, 1);
	gnttab_free_pages(1, &shared_ring);

	kfree(ring_info);
}

/* cleans up all exporter/importer rings */
void hyper_dmabuf_cleanup_ringbufs(void)
{
	hyper_dmabuf_foreach_exporter_ring(hyper_dmabuf_exporter_ringbuf_cleanup);
	hyper_dmabuf_foreach_importer_ring(hyper_dmabuf_importer_ringbuf_cleanup);
}

int hyper_dmabuf_send_request(int domain, struct hyper_dmabuf_ring_rq *req, int wait)
{
	struct hyper_dmabuf_front_ring *ring;
	struct hyper_dmabuf_ring_rq *new_req;
	struct hyper_dmabuf_ring_info_export *ring_info;
	int notify;
	int timeout = 1000;

	/* find a ring info for the channel */
	ring_info = hyper_dmabuf_find_exporter_ring(domain);
	if (!ring_info) {
		printk("Can't find ring info for the channel\n");
		return -EINVAL;
	}

	ring = &ring_info->ring_front;

	if (RING_FULL(ring))
		return -EBUSY;

	new_req = RING_GET_REQUEST(ring, ring->req_prod_pvt);
	if (!new_req) {
		printk("NULL REQUEST\n");
		return -EIO;
	}

	/* update req_pending with current request */
	memcpy(&req_pending, req, sizeof(req_pending));

	/* pass current request to the ring */
	memcpy(new_req, req, sizeof(*new_req));

	ring->req_prod_pvt++;

	RING_PUSH_REQUESTS_AND_CHECK_NOTIFY(ring, notify);
	if (notify) {
		notify_remote_via_irq(ring_info->irq);
	}

	if (wait) {
		while (timeout--) {
			if (req_pending.status !=
			    HYPER_DMABUF_REQ_NOT_RESPONDED)
				break;
			usleep_range(100, 120);
		}

		if (timeout < 0) {
			printk("request timed-out\n");
			return -EBUSY;
		}
	}

	return 0;
}

/* ISR for handling request */
static irqreturn_t hyper_dmabuf_back_ring_isr(int irq, void *info)
{
	RING_IDX rc, rp;
	struct hyper_dmabuf_ring_rq req;
	struct hyper_dmabuf_ring_rp resp;

	int notify, more_to_do;
	int ret;

	struct hyper_dmabuf_ring_info_import *ring_info;
	struct hyper_dmabuf_back_ring *ring;

	ring_info = (struct hyper_dmabuf_ring_info_import *)info;
	ring = &ring_info->ring_back;

	do {
		rc = ring->req_cons;
		rp = ring->sring->req_prod;

		while (rc != rp) {
			if (RING_REQUEST_CONS_OVERFLOW(ring, rc))
				break;

			memcpy(&req, RING_GET_REQUEST(ring, rc), sizeof(req));
			printk("Got request\n");
			ring->req_cons = ++rc;

			ret = hyper_dmabuf_msg_parse(ring_info->sdomain, &req);

			if (ret > 0) {
				/* preparing a response for the request and send it to
				 * the requester
				 */
				memcpy(&resp, &req, sizeof(resp));
				memcpy(RING_GET_RESPONSE(ring, ring->rsp_prod_pvt), &resp,
							sizeof(resp));
				ring->rsp_prod_pvt++;

				RING_PUSH_RESPONSES_AND_CHECK_NOTIFY(ring, notify);

				if (notify) {
					printk("Notyfing\n");
					notify_remote_via_irq(ring_info->irq);
				}
			}

			RING_FINAL_CHECK_FOR_REQUESTS(ring, more_to_do);
			printk("Final check for requests %d\n", more_to_do);
		}
	} while (more_to_do);

	return IRQ_HANDLED;
}

/* ISR for handling responses */
static irqreturn_t hyper_dmabuf_front_ring_isr(int irq, void *info)
{
	/* front ring only care about response from back */
	struct hyper_dmabuf_ring_rp *resp;
	RING_IDX i, rp;
	int more_to_do, ret;

	struct hyper_dmabuf_ring_info_export *ring_info;
	struct hyper_dmabuf_front_ring *ring;

	ring_info = (struct hyper_dmabuf_ring_info_export *)info;
	ring = &ring_info->ring_front;

	do {
		more_to_do = 0;
		rp = ring->sring->rsp_prod;
		for (i = ring->rsp_cons; i != rp; i++) {
			resp = RING_GET_RESPONSE(ring, i);

			/* update pending request's status with what is
			 * in the response
			 */
			if (req_pending.request_id == resp->response_id)
				req_pending.status = resp->status;

			if (resp->status == HYPER_DMABUF_REQ_NEEDS_FOLLOW_UP) {
				/* parsing response */
				ret = hyper_dmabuf_msg_parse(ring_info->rdomain,
							(struct hyper_dmabuf_ring_rq *)resp);

				if (ret < 0) {
					printk("getting error while parsing response\n");
				}
			} else if (resp->status == HYPER_DMABUF_REQ_PROCESSED) {
				/* for debugging dma_buf remote synchronization */
				printk("original request = 0x%x\n", resp->command);
				printk("Just got HYPER_DMABUF_REQ_PROCESSED\n");
			} else if (resp->status == HYPER_DMABUF_REQ_ERROR) {
				/* for debugging dma_buf remote synchronization */
				printk("original request = 0x%x\n", resp->command);
				printk("Just got HYPER_DMABUF_REQ_ERROR\n");
			}
		}

		ring->rsp_cons = i;

		if (i != ring->req_prod_pvt) {
			RING_FINAL_CHECK_FOR_RESPONSES(ring, more_to_do);
			printk("more to do %d\n", more_to_do);
		} else {
			ring->sring->rsp_event = i+1;
		}
	} while (more_to_do);

	return IRQ_HANDLED;
}
