#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/dma-buf.h>
#include "hyper_dmabuf_imp.h"
//#include "hyper_dmabuf_remote_sync.h"
#include "xen/hyper_dmabuf_xen_comm.h"
#include "hyper_dmabuf_msg.h"
#include "hyper_dmabuf_list.h"

void hyper_dmabuf_create_request(struct hyper_dmabuf_ring_rq *request,
				        enum hyper_dmabuf_command command, int *operands)
{
	int i;

	request->request_id = hyper_dmabuf_next_req_id_export();
	request->status = HYPER_DMABUF_REQ_NOT_RESPONDED;
	request->command = command;

	switch(command) {
	/* as exporter, commands to importer */
	case HYPER_DMABUF_EXPORT:
		/* exporting pages for dmabuf */
		/* command : HYPER_DMABUF_EXPORT,
		 * operands0 : hyper_dmabuf_id
		 * operands1 : number of pages to be shared
		 * operands2 : offset of data in the first page
		 * operands3 : length of data in the last page
		 * operands4 : top-level reference number for shared pages
		 * operands5~8 : Driver-specific private data (e.g. graphic buffer's meta info)
		 */
		for (i=0; i < 8; i++)
			request->operands[i] = operands[i];
		break;

	case HYPER_DMABUF_DESTROY:
		/* destroy sg_list for hyper_dmabuf_id on remote side */
		/* command : DMABUF_DESTROY,
		 * operands0 : hyper_dmabuf_id
		 */
		request->operands[0] = operands[0];
		break;

	case HYPER_DMABUF_OPS_TO_REMOTE:
		/* notifying dmabuf map/unmap to importer (probably not needed) */
		/* for dmabuf synchronization */
		break;

	/* as importer, command to exporter */
	case HYPER_DMABUF_OPS_TO_SOURCE:
		/* notifying dmabuf map/unmap to exporter, map will make the driver to do shadow mapping
		* or unmapping for synchronization with original exporter (e.g. i915) */
		/* command : DMABUF_OPS_TO_SOURCE.
		 * operands0 : hyper_dmabuf_id
		 * operands1 : map(=1)/unmap(=2)/attach(=3)/detach(=4)
		 */
		for (i=0; i<2; i++)
			request->operands[i] = operands[i];
		break;

	/* requesting the other side to setup another ring channel for reverse direction */
	case HYPER_DMABUF_EXPORTER_RING_SETUP:
		/* command : HYPER_DMABUF_EXPORTER_RING_SETUP */
		/* no operands needed */
		break;

	default:
		/* no command found */
		return;
	}
}

int hyper_dmabuf_msg_parse(int domid, struct hyper_dmabuf_ring_rq *req)
{
	uint32_t i, ret;
	struct hyper_dmabuf_imported_sgt_info *imported_sgt_info;
	struct hyper_dmabuf_sgt_info *sgt_info;

	/* make sure req is not NULL (may not be needed) */
	if (!req) {
		return -EINVAL;
	}

	req->status = HYPER_DMABUF_REQ_PROCESSED;

	switch (req->command) {
	case HYPER_DMABUF_EXPORT:
		/* exporting pages for dmabuf */
		/* command : HYPER_DMABUF_EXPORT,
		 * operands0 : hyper_dmabuf_id
		 * operands1 : number of pages to be shared
		 * operands2 : offset of data in the first page
		 * operands3 : length of data in the last page
		 * operands4 : top-level reference number for shared pages
		 * operands5~8 : Driver-specific private data (e.g. graphic buffer's meta info)
		 */
		imported_sgt_info = (struct hyper_dmabuf_imported_sgt_info*)kcalloc(1, sizeof(*imported_sgt_info), GFP_KERNEL);
		imported_sgt_info->hyper_dmabuf_id = req->operands[0];
		imported_sgt_info->frst_ofst = req->operands[2];
		imported_sgt_info->last_len = req->operands[3];
		imported_sgt_info->nents = req->operands[1];
		imported_sgt_info->gref = req->operands[4];

		printk("DMABUF was exported\n");
		printk("\thyper_dmabuf_id %d\n", req->operands[0]);
		printk("\tnents %d\n", req->operands[1]);
		printk("\tfirst offset %d\n", req->operands[2]);
		printk("\tlast len %d\n", req->operands[3]);
		printk("\tgrefid %d\n", req->operands[4]);

		for (i=0; i<4; i++)
			imported_sgt_info->private[i] = req->operands[5+i];

		hyper_dmabuf_register_imported(imported_sgt_info);
		break;

	case HYPER_DMABUF_DESTROY:
		/* destroy sg_list for hyper_dmabuf_id on remote side */
		/* command : DMABUF_DESTROY,
		 * operands0 : hyper_dmabuf_id
		 */

		imported_sgt_info =
			hyper_dmabuf_find_imported(req->operands[0]);

		if (imported_sgt_info) {
			hyper_dmabuf_cleanup_imported_pages(imported_sgt_info);

			hyper_dmabuf_remove_imported(req->operands[0]);

			/* TODO: cleanup sgt on importer side etc */
		}

		/* Notify exporter that buffer is freed and it can cleanup it */
		req->status = HYPER_DMABUF_REQ_NEEDS_FOLLOW_UP;
		req->command = HYPER_DMABUF_DESTROY_FINISH;

#if 0 /* function is not implemented yet */

		ret = hyper_dmabuf_destroy_sgt(req->hyper_dmabuf_id);
#endif
		break;

	case HYPER_DMABUF_DESTROY_FINISH:
		/* destroy sg_list for hyper_dmabuf_id on local side */
		/* command : DMABUF_DESTROY_FINISH,
		 * operands0 : hyper_dmabuf_id
		 */

		/* TODO: that should be done on workqueue, when received ack from all importers that buffer is no longer used */
		sgt_info =
			hyper_dmabuf_find_exported(req->operands[0]);

		if (sgt_info) {
			hyper_dmabuf_cleanup_gref_table(sgt_info);

			/* unmap dmabuf */
			dma_buf_unmap_attachment(sgt_info->attachment, sgt_info->sgt, DMA_BIDIRECTIONAL);
			dma_buf_detach(sgt_info->dma_buf, sgt_info->attachment);
			dma_buf_put(sgt_info->dma_buf);

			/* TODO: Rest of cleanup, sgt cleanup etc */
		}

		break;

	case HYPER_DMABUF_OPS_TO_REMOTE:
		/* notifying dmabuf map/unmap to importer (probably not needed) */
		/* for dmabuf synchronization */
		break;

	/* as importer, command to exporter */
	case HYPER_DMABUF_OPS_TO_SOURCE:
		/* notifying dmabuf map/unmap to exporter, map will make the driver to do shadow mapping
		* or unmapping for synchronization with original exporter (e.g. i915) */
		/* command : DMABUF_OPS_TO_SOURCE.
		 * operands0 : hyper_dmabuf_id
		 * operands1 : map(=1)/unmap(=2)/attach(=3)/detach(=4)
		 */
		break;

	/* requesting the other side to setup another ring channel for reverse direction */
	case HYPER_DMABUF_EXPORTER_RING_SETUP:
		/* command: HYPER_DMABUF_EXPORTER_RING_SETUP
		 * no operands needed */
		ret = hyper_dmabuf_exporter_ringbuf_init(domid, &req->operands[0], &req->operands[1]);
		if (ret < 0) {
			req->status = HYPER_DMABUF_REQ_ERROR;
			return -EINVAL;
		}

		req->status = HYPER_DMABUF_REQ_NEEDS_FOLLOW_UP;
		req->command = HYPER_DMABUF_IMPORTER_RING_SETUP;
		break;

	case HYPER_DMABUF_IMPORTER_RING_SETUP:
		/* command: HYPER_DMABUF_IMPORTER_RING_SETUP */
		/* no operands needed */
		ret = hyper_dmabuf_importer_ringbuf_init(domid, req->operands[0], req->operands[1]);
		if (ret < 0)
			return -EINVAL;

		break;

	default:
		/* no matched command, nothing to do.. just return error */
		return -EINVAL;
	}

	return req->command;
}
