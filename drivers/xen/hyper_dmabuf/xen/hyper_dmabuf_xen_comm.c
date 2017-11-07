/*
 * Copyright © 2017 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Dongwon Kim <dongwon.kim@intel.com>
 *    Mateusz Polrola <mateuszx.potrola@intel.com>
 *
 */

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
#include "../hyper_dmabuf_drv.h"

static int export_req_id = 0;

struct hyper_dmabuf_req req_pending = {0};

extern struct hyper_dmabuf_private hyper_dmabuf_private;

/* Creates entry in xen store that will keep details of all
 * exporter rings created by this domain
 */
static int xen_comm_setup_data_dir(void)
{
	char buf[255];

	sprintf(buf, "/local/domain/%d/data/hyper_dmabuf", hyper_dmabuf_xen_get_domid());
	return xenbus_mkdir(XBT_NIL, buf, "");
}

/* Removes entry from xenstore with exporter ring details.
 * Other domains that has connected to any of exporter rings
 * created by this domain, will be notified about removal of
 * this entry and will treat that as signal to cleanup importer
 * rings created for this domain
 */
static int xen_comm_destroy_data_dir(void)
{
	char buf[255];

	sprintf(buf, "/local/domain/%d/data/hyper_dmabuf", hyper_dmabuf_xen_get_domid());
	return xenbus_rm(XBT_NIL, buf, "");
}

/* Adds xenstore entries with details of exporter ring created
 * for given remote domain. It requires special daemon running
 * in dom0 to make sure that given remote domain will have right
 * permissions to access that data.
 */
static int xen_comm_expose_ring_details(int domid, int rdomid,
					int gref, int port)
{
	char buf[255];
	int ret;

	sprintf(buf, "/local/domain/%d/data/hyper_dmabuf/%d", domid, rdomid);
	ret = xenbus_printf(XBT_NIL, buf, "grefid", "%d", gref);

	if (ret) {
		dev_err(hyper_dmabuf_private.device,
			"Failed to write xenbus entry %s: %d\n", buf, ret);
		return ret;
	}

	ret = xenbus_printf(XBT_NIL, buf, "port", "%d", port);

	if (ret) {
		dev_err(hyper_dmabuf_private.device,
			"Failed to write xenbus entry %s: %d\n", buf, ret);
		return ret;
	}

	return 0;
}

/*
 * Queries details of ring exposed by remote domain.
 */
static int xen_comm_get_ring_details(int domid, int rdomid, int *grefid, int *port)
{
	char buf[255];
	int ret;

	sprintf(buf, "/local/domain/%d/data/hyper_dmabuf/%d", rdomid, domid);
	ret = xenbus_scanf(XBT_NIL, buf, "grefid", "%d", grefid);

	if (ret <= 0) {
		dev_err(hyper_dmabuf_private.device,
			"Failed to read xenbus entry %s: %d\n", buf, ret);
		return ret;
	}

	ret = xenbus_scanf(XBT_NIL, buf, "port", "%d", port);

	if (ret <= 0) {
		dev_err(hyper_dmabuf_private.device,
			"Failed to read xenbus entry %s: %d\n", buf, ret);
		return ret;
	}

	return (ret <= 0 ? 1 : 0);
}

int hyper_dmabuf_xen_get_domid(void)
{
	struct xenbus_transaction xbt;
	int domid;

        xenbus_transaction_start(&xbt);

        if (!xenbus_scanf(xbt, "domid","", "%d", &domid)) {
		domid = -1;
        }
        xenbus_transaction_end(xbt, 0);

	return domid;
}

static int xen_comm_next_req_id(void)
{
        export_req_id++;
        return export_req_id;
}

/* For now cache latast rings as global variables TODO: keep them in list*/
static irqreturn_t front_ring_isr(int irq, void *info);
static irqreturn_t back_ring_isr(int irq, void *info);

/* Callback function that will be called on any change of xenbus path
 * being watched. Used for detecting creation/destruction of remote
 * domain exporter ring.
 *
 * When remote domain's exporter ring will be detected, importer ring
 * on this domain will be created.
 *
 * When remote domain's exporter ring destruction will be detected it
 * will celanup this domain importer ring.
 *
 * Destruction can be caused by unloading module by remote domain or
 * it's crash/force shutdown.
 */
static void remote_dom_exporter_watch_cb(struct xenbus_watch *watch,
					 const char *path, const char *token)
{
	int rdom,ret;
	uint32_t grefid, port;
	struct xen_comm_rx_ring_info *ring_info;

	/* Check which domain has changed its exporter rings */
	ret = sscanf(watch->node, "/local/domain/%d/", &rdom);
	if (ret <= 0) {
		return;
	}

	/* Check if we have importer ring for given remote domain alrady created */
	ring_info = xen_comm_find_rx_ring(rdom);

	/* Try to query remote domain exporter ring details - if that will
	 * fail and we have importer ring that means remote domains has cleanup
	 * its exporter ring, so our importer ring is no longer useful.
	 *
	 * If querying details will succeed and we don't have importer ring,
	 * it means that remote domain has setup it for us and we should connect
	 * to it.
	 */
	ret = xen_comm_get_ring_details(hyper_dmabuf_xen_get_domid(), rdom,
					&grefid, &port);

	if (ring_info && ret != 0) {
		dev_info(hyper_dmabuf_private.device,
			 "Remote exporter closed, cleaninup importer\n");
		hyper_dmabuf_xen_cleanup_rx_rbuf(rdom);
	} else if (!ring_info && ret == 0) {
		dev_info(hyper_dmabuf_private.device,
			 "Registering importer\n");
		hyper_dmabuf_xen_init_rx_rbuf(rdom);
	}
}

/* exporter needs to generated info for page sharing */
int hyper_dmabuf_xen_init_tx_rbuf(int domid)
{
	struct xen_comm_tx_ring_info *ring_info;
	struct xen_comm_sring *sring;
	struct evtchn_alloc_unbound alloc_unbound;
	struct evtchn_close close;

	void *shared_ring;
	int ret;

	/* check if there's any existing tx channel in the table */
	ring_info = xen_comm_find_tx_ring(domid);

	if (ring_info) {
		dev_info(hyper_dmabuf_private.device,
			 "tx ring ch to domid = %d already exist\ngref = %d, port = %d\n",
		ring_info->rdomain, ring_info->gref_ring, ring_info->port);
		return 0;
	}

	ring_info = kmalloc(sizeof(*ring_info), GFP_KERNEL);

	/* from exporter to importer */
	shared_ring = (void *)__get_free_pages(GFP_KERNEL, 1);
	if (shared_ring == 0) {
		return -EINVAL;
	}

	sring = (struct xen_comm_sring *) shared_ring;

	SHARED_RING_INIT(sring);

	FRONT_RING_INIT(&(ring_info->ring_front), sring, PAGE_SIZE);

	ring_info->gref_ring = gnttab_grant_foreign_access(domid,
							   virt_to_mfn(shared_ring),
							   0);
	if (ring_info->gref_ring < 0) {
		/* fail to get gref */
		return -EINVAL;
	}

	alloc_unbound.dom = DOMID_SELF;
	alloc_unbound.remote_dom = domid;
	ret = HYPERVISOR_event_channel_op(EVTCHNOP_alloc_unbound,
					&alloc_unbound);
	if (ret != 0) {
		dev_err(hyper_dmabuf_private.device,
			"Cannot allocate event channel\n");
		return -EINVAL;
	}

	/* setting up interrupt */
	ret = bind_evtchn_to_irqhandler(alloc_unbound.port,
					front_ring_isr, 0,
					NULL, (void*) ring_info);

	if (ret < 0) {
		dev_err(hyper_dmabuf_private.device,
			"Failed to setup event channel\n");
		close.port = alloc_unbound.port;
		HYPERVISOR_event_channel_op(EVTCHNOP_close, &close);
		gnttab_end_foreign_access(ring_info->gref_ring, 0,
					virt_to_mfn(shared_ring));
		return -EINVAL;
	}

	ring_info->rdomain = domid;
	ring_info->irq = ret;
	ring_info->port = alloc_unbound.port;

	mutex_init(&ring_info->lock);

	dev_dbg(hyper_dmabuf_private.device,
		"%s: allocated eventchannel gref %d  port: %d  irq: %d\n",
		__func__,
		ring_info->gref_ring,
		ring_info->port,
		ring_info->irq);

	ret = xen_comm_add_tx_ring(ring_info);

	ret = xen_comm_expose_ring_details(hyper_dmabuf_xen_get_domid(), domid,
					   ring_info->gref_ring, ring_info->port);

	/*
	 * Register watch for remote domain exporter ring.
	 * When remote domain will setup its exporter ring,
	 * we will automatically connect our importer ring to it.
	 */
	ring_info->watch.callback = remote_dom_exporter_watch_cb;
	ring_info->watch.node = (const char*) kmalloc(sizeof(char) * 255, GFP_KERNEL);
	sprintf((char*)ring_info->watch.node,
		"/local/domain/%d/data/hyper_dmabuf/%d/port",
		domid, hyper_dmabuf_xen_get_domid());

	register_xenbus_watch(&ring_info->watch);

	return ret;
}

/* cleans up exporter ring created for given remote domain */
void hyper_dmabuf_xen_cleanup_tx_rbuf(int domid)
{
	struct xen_comm_tx_ring_info *ring_info;
	struct xen_comm_rx_ring_info *rx_ring_info;

	/* check if we at all have exporter ring for given rdomain */
	ring_info = xen_comm_find_tx_ring(domid);

	if (!ring_info) {
		return;
	}

	xen_comm_remove_tx_ring(domid);

	unregister_xenbus_watch(&ring_info->watch);
	kfree(ring_info->watch.node);

	/* No need to close communication channel, will be done by
	 * this function
	 */
	unbind_from_irqhandler(ring_info->irq, (void*) ring_info);

	/* No need to free sring page, will be freed by this function
	 * when other side will end its access
	 */
	gnttab_end_foreign_access(ring_info->gref_ring, 0,
				  (unsigned long) ring_info->ring_front.sring);

	kfree(ring_info);

	rx_ring_info = xen_comm_find_rx_ring(domid);
	if (!rx_ring_info)
		return;

	BACK_RING_INIT(&(rx_ring_info->ring_back), rx_ring_info->ring_back.sring, PAGE_SIZE);
}

/* importer needs to know about shared page and port numbers for
 * ring buffer and event channel
 */
int hyper_dmabuf_xen_init_rx_rbuf(int domid)
{
	struct xen_comm_rx_ring_info *ring_info;
	struct xen_comm_sring *sring;

	struct page *shared_ring;

	struct gnttab_map_grant_ref *map_ops;

	int ret;
	int rx_gref, rx_port;

	/* check if there's existing rx ring channel */
	ring_info = xen_comm_find_rx_ring(domid);

	if (ring_info) {
		dev_info(hyper_dmabuf_private.device,
			 "rx ring ch from domid = %d already exist\n", ring_info->sdomain);
		return 0;
	}

	ret = xen_comm_get_ring_details(hyper_dmabuf_xen_get_domid(), domid,
					&rx_gref, &rx_port);

	if (ret) {
		dev_err(hyper_dmabuf_private.device,
			"Domain %d has not created exporter ring for current domain\n", domid);
		return ret;
	}

	ring_info = kmalloc(sizeof(*ring_info), GFP_KERNEL);

	ring_info->sdomain = domid;
	ring_info->evtchn = rx_port;

	map_ops = kmalloc(sizeof(*map_ops), GFP_KERNEL);

	if (gnttab_alloc_pages(1, &shared_ring)) {
		return -EINVAL;
	}

	gnttab_set_map_op(&map_ops[0], (unsigned long)pfn_to_kaddr(page_to_pfn(shared_ring)),
			  GNTMAP_host_map, rx_gref, domid);

	gnttab_set_unmap_op(&ring_info->unmap_op, (unsigned long)pfn_to_kaddr(page_to_pfn(shared_ring)),
			    GNTMAP_host_map, -1);

	ret = gnttab_map_refs(map_ops, NULL, &shared_ring, 1);
	if (ret < 0) {
		dev_err(hyper_dmabuf_private.device, "Cannot map ring\n");
		return -EINVAL;
	}

	if (map_ops[0].status) {
		dev_err(hyper_dmabuf_private.device, "Ring mapping failed\n");
		return -EINVAL;
	} else {
		ring_info->unmap_op.handle = map_ops[0].handle;
	}

	kfree(map_ops);

	sring = (struct xen_comm_sring *)pfn_to_kaddr(page_to_pfn(shared_ring));

	BACK_RING_INIT(&ring_info->ring_back, sring, PAGE_SIZE);

	ret = bind_interdomain_evtchn_to_irq(domid, rx_port);

	if (ret < 0) {
		return -EINVAL;
	}

	ring_info->irq = ret;

	dev_dbg(hyper_dmabuf_private.device,
		"%s: bound to eventchannel port: %d  irq: %d\n", __func__,
		rx_port,
		ring_info->irq);

	ret = xen_comm_add_rx_ring(ring_info);

	/* Setup communcation channel in opposite direction */
	if (!xen_comm_find_tx_ring(domid)) {
		ret = hyper_dmabuf_xen_init_tx_rbuf(domid);
	}

	ret = request_irq(ring_info->irq,
			  back_ring_isr, 0,
			  NULL, (void*)ring_info);

	return ret;
}

/* clenas up importer ring create for given source domain */
void hyper_dmabuf_xen_cleanup_rx_rbuf(int domid)
{
	struct xen_comm_rx_ring_info *ring_info;
	struct xen_comm_tx_ring_info *tx_ring_info;
	struct page *shared_ring;

	/* check if we have importer ring created for given sdomain */
	ring_info = xen_comm_find_rx_ring(domid);

	if (!ring_info)
		return;

	xen_comm_remove_rx_ring(domid);

	/* no need to close event channel, will be done by that function */
	unbind_from_irqhandler(ring_info->irq, (void*)ring_info);

	/* unmapping shared ring page */
	shared_ring = virt_to_page(ring_info->ring_back.sring);
	gnttab_unmap_refs(&ring_info->unmap_op, NULL, &shared_ring, 1);
	gnttab_free_pages(1, &shared_ring);

	kfree(ring_info);

	tx_ring_info = xen_comm_find_tx_ring(domid);
	if (!tx_ring_info)
		return;

	SHARED_RING_INIT(tx_ring_info->ring_front.sring);
	FRONT_RING_INIT(&(tx_ring_info->ring_front), tx_ring_info->ring_front.sring, PAGE_SIZE);
}

int hyper_dmabuf_xen_init_comm_env(void)
{
	int ret;

	xen_comm_ring_table_init();
	ret = xen_comm_setup_data_dir();

	return ret;
}

/* cleans up all tx/rx rings */
static void hyper_dmabuf_xen_cleanup_all_rbufs(void)
{
	xen_comm_foreach_tx_ring(hyper_dmabuf_xen_cleanup_tx_rbuf);
	xen_comm_foreach_rx_ring(hyper_dmabuf_xen_cleanup_rx_rbuf);
}

void hyper_dmabuf_xen_destroy_comm(void)
{
	hyper_dmabuf_xen_cleanup_all_rbufs();
	xen_comm_destroy_data_dir();
}

int hyper_dmabuf_xen_send_req(int domid, struct hyper_dmabuf_req *req, int wait)
{
	struct xen_comm_front_ring *ring;
	struct hyper_dmabuf_req *new_req;
	struct xen_comm_tx_ring_info *ring_info;
	int notify;
	int timeout = 1000;

	/* find a ring info for the channel */
	ring_info = xen_comm_find_tx_ring(domid);
	if (!ring_info) {
		dev_err(hyper_dmabuf_private.device,
			"Can't find ring info for the channel\n");
		return -EINVAL;
	}


	mutex_lock(&ring_info->lock);

	ring = &ring_info->ring_front;

	if (RING_FULL(ring))
		return -EBUSY;

	new_req = RING_GET_REQUEST(ring, ring->req_prod_pvt);
	if (!new_req) {
		mutex_unlock(&ring_info->lock);
		dev_err(hyper_dmabuf_private.device,
			"NULL REQUEST\n");
		return -EIO;
	}

	req->request_id = xen_comm_next_req_id();

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
			mutex_unlock(&ring_info->lock);
			dev_err(hyper_dmabuf_private.device, "request timed-out\n");
			return -EBUSY;
		}

		mutex_unlock(&ring_info->lock);
		return req_pending.status;
	}

	mutex_unlock(&ring_info->lock);

	return 0;
}

/* ISR for handling request */
static irqreturn_t back_ring_isr(int irq, void *info)
{
	RING_IDX rc, rp;
	struct hyper_dmabuf_req req;
	struct hyper_dmabuf_resp resp;

	int notify, more_to_do;
	int ret;

	struct xen_comm_rx_ring_info *ring_info;
	struct xen_comm_back_ring *ring;

	ring_info = (struct xen_comm_rx_ring_info *)info;
	ring = &ring_info->ring_back;

	dev_dbg(hyper_dmabuf_private.device, "%s\n", __func__);

	do {
		rc = ring->req_cons;
		rp = ring->sring->req_prod;
		more_to_do = 0;
		while (rc != rp) {
			if (RING_REQUEST_CONS_OVERFLOW(ring, rc))
				break;

			memcpy(&req, RING_GET_REQUEST(ring, rc), sizeof(req));
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
					notify_remote_via_irq(ring_info->irq);
				}
			}

			RING_FINAL_CHECK_FOR_REQUESTS(ring, more_to_do);
		}
	} while (more_to_do);

	return IRQ_HANDLED;
}

/* ISR for handling responses */
static irqreturn_t front_ring_isr(int irq, void *info)
{
	/* front ring only care about response from back */
	struct hyper_dmabuf_resp *resp;
	RING_IDX i, rp;
	int more_to_do, ret;

	struct xen_comm_tx_ring_info *ring_info;
	struct xen_comm_front_ring *ring;

	ring_info = (struct xen_comm_tx_ring_info *)info;
	ring = &ring_info->ring_front;

	dev_dbg(hyper_dmabuf_private.device, "%s\n", __func__);

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
							(struct hyper_dmabuf_req *)resp);

				if (ret < 0) {
					dev_err(hyper_dmabuf_private.device,
						"getting error while parsing response\n");
				}
			} else if (resp->status == HYPER_DMABUF_REQ_PROCESSED) {
				/* for debugging dma_buf remote synchronization */
				dev_dbg(hyper_dmabuf_private.device,
					"original request = 0x%x\n", resp->command);
				dev_dbg(hyper_dmabuf_private.device,
					"Just got HYPER_DMABUF_REQ_PROCESSED\n");
			} else if (resp->status == HYPER_DMABUF_REQ_ERROR) {
				/* for debugging dma_buf remote synchronization */
				dev_dbg(hyper_dmabuf_private.device,
					"original request = 0x%x\n", resp->command);
				dev_dbg(hyper_dmabuf_private.device,
					"Just got HYPER_DMABUF_REQ_ERROR\n");
			}
		}

		ring->rsp_cons = i;

		if (i != ring->req_prod_pvt) {
			RING_FINAL_CHECK_FOR_RESPONSES(ring, more_to_do);
		} else {
			ring->sring->rsp_event = i+1;
		}
	} while (more_to_do);

	return IRQ_HANDLED;
}
