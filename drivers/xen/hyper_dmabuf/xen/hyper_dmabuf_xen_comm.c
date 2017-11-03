#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
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
static irqreturn_t hyper_dmabuf_front_ring_isr(int irq, void *dev_id);
static irqreturn_t hyper_dmabuf_back_ring_isr(int irq, void *dev_id);

/* exporter needs to generated info for page sharing */
int hyper_dmabuf_exporter_ringbuf_init(int rdomain, grant_ref_t *refid, int *port)
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

	/* store refid and port numbers for userspace's use */
	*refid = ring_info->gref_ring;
	*port = ring_info->port;

	printk("%s: allocated eventchannel gref %d  port: %d  irq: %d\n",
		__func__,
		ring_info->gref_ring,
		ring_info->port,
		ring_info->irq);

	/* register ring info */
	ret = hyper_dmabuf_register_exporter_ring(ring_info);

	return ret;
}

/* importer needs to know about shared page and port numbers for ring buffer and event channel */
int hyper_dmabuf_importer_ringbuf_init(int sdomain, grant_ref_t gref, int port)
{
	struct hyper_dmabuf_ring_info_import *ring_info;
	struct hyper_dmabuf_sring *sring;

	struct page *shared_ring;

	struct gnttab_map_grant_ref *ops;
	struct gnttab_unmap_grant_ref *unmap_ops;
	int ret;

	ring_info = (struct hyper_dmabuf_ring_info_import *)
			kmalloc(sizeof(*ring_info), GFP_KERNEL);

	ring_info->sdomain = sdomain;
	ring_info->evtchn = port;

	ops = (struct gnttab_map_grant_ref*)kmalloc(sizeof(*ops), GFP_KERNEL);
	unmap_ops = (struct gnttab_unmap_grant_ref*)kmalloc(sizeof(*unmap_ops), GFP_KERNEL);

	if (gnttab_alloc_pages(1, &shared_ring)) {
		return -EINVAL;
	}

	gnttab_set_map_op(&ops[0], (unsigned long)pfn_to_kaddr(page_to_pfn(shared_ring)),
			GNTMAP_host_map, gref, sdomain);

	ret = gnttab_map_refs(ops, NULL, &shared_ring, 1);
	if (ret < 0) {
		printk("Cannot map ring\n");
		return -EINVAL;
	}

	if (ops[0].status) {
		printk("Ring mapping failed\n");
		return -EINVAL;
	}

	sring = (struct hyper_dmabuf_sring*) pfn_to_kaddr(page_to_pfn(shared_ring));

	BACK_RING_INIT(&ring_info->ring_back, sring, PAGE_SIZE);

	ret = bind_interdomain_evtchn_to_irqhandler(sdomain, port,
						hyper_dmabuf_back_ring_isr, 0,
						NULL, (void*)ring_info);
	if (ret < 0) {
		return -EINVAL;
	}

	ring_info->irq = ret;

	printk("%s: bound to eventchannel port: %d  irq: %d\n", __func__,
		port,
		ring_info->irq);

	ret = hyper_dmabuf_register_importer_ring(ring_info);

	return ret;
}

int hyper_dmabuf_send_request(int domain, struct hyper_dmabuf_ring_rq *req)
{
	struct hyper_dmabuf_front_ring *ring;
	struct hyper_dmabuf_ring_rq *new_req;
	struct hyper_dmabuf_ring_info_export *ring_info;
	int notify;

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

	memcpy(new_req, req, sizeof(*new_req));

	ring->req_prod_pvt++;

	RING_PUSH_REQUESTS_AND_CHECK_NOTIFY(ring, notify);
	if (notify) {
		notify_remote_via_irq(ring_info->irq);
	}

	return 0;
}

/* ISR for request from exporter (as an importer) */
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

/* ISR for responses from importer */
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
			unsigned long id;

			resp = RING_GET_RESPONSE(ring, i);
			id = resp->response_id;

			if (resp->status == HYPER_DMABUF_REQ_NEEDS_FOLLOW_UP) {
				/* parsing response */
				ret = hyper_dmabuf_msg_parse(ring_info->rdomain,
							(struct hyper_dmabuf_ring_rq *)resp);

				if (ret < 0) {
					printk("getting error while parsing response\n");
				}
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
