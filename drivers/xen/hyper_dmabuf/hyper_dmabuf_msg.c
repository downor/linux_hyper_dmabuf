#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/dma-buf.h>
#include <xen/grant_table.h>
#include <linux/workqueue.h>
#include "hyper_dmabuf_msg.h"
#include "hyper_dmabuf_drv.h"
#include "hyper_dmabuf_imp.h"
#include "hyper_dmabuf_remote_sync.h"
#include "hyper_dmabuf_list.h"

#define FORCED_UNEXPORTING 0

extern struct hyper_dmabuf_private hyper_dmabuf_private;

struct cmd_process {
	struct work_struct work;
	struct hyper_dmabuf_req *rq;
	int domid;
};

void hyper_dmabuf_create_request(struct hyper_dmabuf_req *req,
				 enum hyper_dmabuf_command command, int *operands)
{
	int i;

	req->status = HYPER_DMABUF_REQ_NOT_RESPONDED;
	req->command = command;

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
			req->operands[i] = operands[i];
		break;

	case HYPER_DMABUF_NOTIFY_UNEXPORT:
		/* destroy sg_list for hyper_dmabuf_id on remote side */
		/* command : DMABUF_DESTROY,
		 * operands0 : hyper_dmabuf_id
		 */
		req->operands[0] = operands[0];
		break;

	case HYPER_DMABUF_FIRST_EXPORT:
		/* dmabuf fd is being created on imported side for first time */
		/* command : HYPER_DMABUF_FIRST_EXPORT,
		 * operands0 : hyper_dmabuf_id
		 */
		req->operands[0] = operands[0];
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
			req->operands[i] = operands[i];
		break;

	default:
		/* no command found */
		return;
	}
}

void cmd_process_work(struct work_struct *work)
{
	struct hyper_dmabuf_sgt_info *sgt_info;
	struct hyper_dmabuf_imported_sgt_info *imported_sgt_info;
	struct cmd_process *proc = container_of(work, struct cmd_process, work);
	struct hyper_dmabuf_req *req;
	int domid;
	int i;

	req = proc->rq;
	domid = proc->domid;

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
		imported_sgt_info = kcalloc(1, sizeof(*imported_sgt_info), GFP_KERNEL);
		imported_sgt_info->hyper_dmabuf_id = req->operands[0];
		imported_sgt_info->frst_ofst = req->operands[2];
		imported_sgt_info->last_len = req->operands[3];
		imported_sgt_info->nents = req->operands[1];
		imported_sgt_info->ref_handle = req->operands[4];

		printk("DMABUF was exported\n");
		printk("\thyper_dmabuf_id %d\n", req->operands[0]);
		printk("\tnents %d\n", req->operands[1]);
		printk("\tfirst offset %d\n", req->operands[2]);
		printk("\tlast len %d\n", req->operands[3]);
		printk("\tgrefid %d\n", req->operands[4]);

		for (i=0; i<4; i++)
			imported_sgt_info->private[i] = req->operands[5+i];

		imported_sgt_info->valid = 1;
		hyper_dmabuf_register_imported(imported_sgt_info);
		break;

	case HYPER_DMABUF_FIRST_EXPORT:
		/* find a corresponding SGT for the id */
		sgt_info = hyper_dmabuf_find_exported(req->operands[0]);

		if (!sgt_info) {
			printk("critical err: requested sgt_info can't be found %d\n", req->operands[0]);
			break;
		}

		sgt_info->importer_exported++;
		break;

	case HYPER_DMABUF_OPS_TO_REMOTE:
		/* notifying dmabuf map/unmap to importer (probably not needed) */
		/* for dmabuf synchronization */
		break;

	default:
		/* shouldn't get here */
		/* no matched command, nothing to do.. just return error */
		break;
	}

	kfree(req);
	kfree(proc);
}

int hyper_dmabuf_msg_parse(int domid, struct hyper_dmabuf_req *req)
{
	struct cmd_process *proc;
	struct hyper_dmabuf_req *temp_req;
	struct hyper_dmabuf_imported_sgt_info *sgt_info;
	int ret;

	if (!req) {
		printk("request is NULL\n");
		return -EINVAL;
	}

	if ((req->command < HYPER_DMABUF_EXPORT) ||
		(req->command > HYPER_DMABUF_OPS_TO_SOURCE)) {
		printk("invalid command\n");
		return -EINVAL;
	}

	req->status = HYPER_DMABUF_REQ_PROCESSED;

	/* HYPER_DMABUF_DESTROY requires immediate
	 * follow up so can't be processed in workqueue
	 */
	if (req->command == HYPER_DMABUF_NOTIFY_UNEXPORT) {
		/* destroy sg_list for hyper_dmabuf_id on remote side */
		/* command : HYPER_DMABUF_NOTIFY_UNEXPORT,
		 * operands0 : hyper_dmabuf_id
		 */

		sgt_info = hyper_dmabuf_find_imported(req->operands[0]);

		if (sgt_info) {
			/* if anything is still using dma_buf */
			if (sgt_info->dma_buf &&
			    dmabuf_refcount(sgt_info->dma_buf) > 0) {
				/*
				 * Buffer is still in  use, just mark that it should
				 * not be allowed to export its fd anymore.
				 */
				sgt_info->valid = 0;
			} else {
				/* No one is using buffer, remove it from imported list */
				hyper_dmabuf_remove_imported(req->operands[0]);
				kfree(sgt_info);
			}
		} else {
			req->status = HYPER_DMABUF_REQ_ERROR;
		}

		return req->command;
	}

	/* dma buf remote synchronization */
	if (req->command == HYPER_DMABUF_OPS_TO_SOURCE) {
		/* notifying dmabuf map/unmap to exporter, map will make the driver to do shadow mapping
		 * or unmapping for synchronization with original exporter (e.g. i915) */

		/* command : DMABUF_OPS_TO_SOURCE.
		 * operands0 : hyper_dmabuf_id
		 * operands1 : enum hyper_dmabuf_ops {....}
		 */
		ret = hyper_dmabuf_remote_sync(req->operands[0], req->operands[1]);
		if (ret)
			req->status = HYPER_DMABUF_REQ_ERROR;
		else
			req->status = HYPER_DMABUF_REQ_PROCESSED;

		return req->command;
	}

	temp_req = kmalloc(sizeof(*temp_req), GFP_KERNEL);

	memcpy(temp_req, req, sizeof(*temp_req));

	proc = kcalloc(1, sizeof(struct cmd_process), GFP_KERNEL);

	proc->rq = temp_req;
	proc->domid = domid;

	INIT_WORK(&(proc->work), cmd_process_work);

	queue_work(hyper_dmabuf_private.work_queue, &(proc->work));

	return req->command;
}
