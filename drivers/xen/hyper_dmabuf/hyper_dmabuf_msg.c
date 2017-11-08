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
#include <linux/dma-buf.h>
#include <xen/grant_table.h>
#include <linux/workqueue.h>
#include "hyper_dmabuf_msg.h"
#include "hyper_dmabuf_drv.h"
#include "hyper_dmabuf_remote_sync.h"
#include "hyper_dmabuf_list.h"

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
		 * operands0~3 : hyper_dmabuf_id
		 * operands4 : number of pages to be shared
		 * operands5 : offset of data in the first page
		 * operands6 : length of data in the last page
		 * operands7 : top-level reference number for shared pages
		 * operands8~11 : Driver-specific private data (e.g. graphic buffer's meta info)
		 */
		for (i=0; i < 11; i++)
			req->operands[i] = operands[i];
		break;

	case HYPER_DMABUF_NOTIFY_UNEXPORT:
		/* destroy sg_list for hyper_dmabuf_id on remote side */
		/* command : DMABUF_DESTROY,
		 * operands0~3 : hyper_dmabuf_id_t hid
		 */

		for (i=0; i < 4; i++)
			req->operands[i] = operands[i];
		break;

	case HYPER_DMABUF_EXPORT_FD:
	case HYPER_DMABUF_EXPORT_FD_FAILED:
		/* dmabuf fd is being created on imported side or importing failed */
		/* command : HYPER_DMABUF_EXPORT_FD or HYPER_DMABUF_EXPORT_FD_FAILED,
		 * operands0~3 : hyper_dmabuf_id
		 */

		for (i=0; i < 4; i++)
			req->operands[i] = operands[i];
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
		 * operands0~3 : hyper_dmabuf_id
		 * operands4 : map(=1)/unmap(=2)/attach(=3)/detach(=4)
		 */
		for (i = 0; i < 5; i++)
			req->operands[i] = operands[i];
		break;

	default:
		/* no command found */
		return;
	}
}

void cmd_process_work(struct work_struct *work)
{
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
		 * operands0~3 : hyper_dmabuf_id
		 * operands4 : number of pages to be shared
		 * operands5 : offset of data in the first page
		 * operands6 : length of data in the last page
		 * operands7 : top-level reference number for shared pages
		 * operands8~11 : Driver-specific private data (e.g. graphic buffer's meta info)
		 */
		imported_sgt_info = kcalloc(1, sizeof(*imported_sgt_info), GFP_KERNEL);

		if (!imported_sgt_info) {
			dev_err(hyper_dmabuf_private.device,
				"No memory left to be allocated\n");
			break;
		}

		imported_sgt_info->hid.id = req->operands[0];

		for (i=0; i<3; i++)
			imported_sgt_info->hid.rng_key[i] = req->operands[i+1];

		imported_sgt_info->nents = req->operands[4];
		imported_sgt_info->frst_ofst = req->operands[5];
		imported_sgt_info->last_len = req->operands[6];
		imported_sgt_info->ref_handle = req->operands[7];

		dev_dbg(hyper_dmabuf_private.device, "DMABUF was exported\n");
		dev_dbg(hyper_dmabuf_private.device, "\thid{id:%d key:%d %d %d}\n",
			req->operands[0], req->operands[1], req->operands[2],
			req->operands[3]);
		dev_dbg(hyper_dmabuf_private.device, "\tnents %d\n", req->operands[4]);
		dev_dbg(hyper_dmabuf_private.device, "\tfirst offset %d\n", req->operands[5]);
		dev_dbg(hyper_dmabuf_private.device, "\tlast len %d\n", req->operands[6]);
		dev_dbg(hyper_dmabuf_private.device, "\tgrefid %d\n", req->operands[7]);

		for (i=0; i<4; i++)
			imported_sgt_info->private[i] = req->operands[8+i];

		imported_sgt_info->valid = 1;
		hyper_dmabuf_register_imported(imported_sgt_info);
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
	struct hyper_dmabuf_sgt_info *exp_sgt_info;
	hyper_dmabuf_id_t hid;
	int ret;

	if (!req) {
		dev_err(hyper_dmabuf_private.device, "request is NULL\n");
		return -EINVAL;
	}

	hid.id = req->operands[0];
	hid.rng_key[0] = req->operands[1];
	hid.rng_key[1] = req->operands[2];
	hid.rng_key[2] = req->operands[3];

	if ((req->command < HYPER_DMABUF_EXPORT) ||
		(req->command > HYPER_DMABUF_OPS_TO_SOURCE)) {
		dev_err(hyper_dmabuf_private.device, "invalid command\n");
		return -EINVAL;
	}

	req->status = HYPER_DMABUF_REQ_PROCESSED;

	/* HYPER_DMABUF_DESTROY requires immediate
	 * follow up so can't be processed in workqueue
	 */
	if (req->command == HYPER_DMABUF_NOTIFY_UNEXPORT) {
		/* destroy sg_list for hyper_dmabuf_id on remote side */
		/* command : HYPER_DMABUF_NOTIFY_UNEXPORT,
		 * operands0~3 : hyper_dmabuf_id
		 */
		dev_dbg(hyper_dmabuf_private.device,
			"%s: processing HYPER_DMABUF_NOTIFY_UNEXPORT\n", __func__);

		sgt_info = hyper_dmabuf_find_imported(hid);

		if (sgt_info) {
			/* if anything is still using dma_buf */
			if (sgt_info->num_importers) {
				/*
				 * Buffer is still in  use, just mark that it should
				 * not be allowed to export its fd anymore.
				 */
				sgt_info->valid = 0;
			} else {
				/* No one is using buffer, remove it from imported list */
				hyper_dmabuf_remove_imported(hid);
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
		 * operands0~3 : hyper_dmabuf_id
		 * operands1 : enum hyper_dmabuf_ops {....}
		 */
		dev_dbg(hyper_dmabuf_private.device,
			"%s: HYPER_DMABUF_OPS_TO_SOURCE\n", __func__);

		ret = hyper_dmabuf_remote_sync(hid, req->operands[4]);

		if (ret)
			req->status = HYPER_DMABUF_REQ_ERROR;
		else
			req->status = HYPER_DMABUF_REQ_PROCESSED;

		return req->command;
	}

	/* synchronous dma_buf_fd export */
	if (req->command == HYPER_DMABUF_EXPORT_FD) {
		/* find a corresponding SGT for the id */
		dev_dbg(hyper_dmabuf_private.device,
			"Processing HYPER_DMABUF_EXPORT_FD for buffer {id:%d key:%d %d %d}\n",
			hid.id, hid.rng_key[0], hid.rng_key[1], hid.rng_key[2]);

		exp_sgt_info = hyper_dmabuf_find_exported(hid);

		if (!exp_sgt_info) {
			dev_err(hyper_dmabuf_private.device,
				"critical err: requested sgt_info can't be found for buffer {id:%d key:%d %d %d}\n",
				hid.id, hid.rng_key[0], hid.rng_key[1], hid.rng_key[2]);

			req->status = HYPER_DMABUF_REQ_ERROR;
		} else if (!exp_sgt_info->valid) {
			dev_dbg(hyper_dmabuf_private.device,
				"Buffer no longer valid - cannot export fd for buffer {id:%d key:%d %d %d}\n",
				hid.id, hid.rng_key[0], hid.rng_key[1], hid.rng_key[2]);

			req->status = HYPER_DMABUF_REQ_ERROR;
		} else {
			dev_dbg(hyper_dmabuf_private.device,
				"Buffer still valid - can export fd for buffer {id:%d key:%d %d %d}\n",
				hid.id, hid.rng_key[0], hid.rng_key[1], hid.rng_key[2]);

			exp_sgt_info->importer_exported++;
			req->status = HYPER_DMABUF_REQ_PROCESSED;
		}
		return req->command;
	}

	if (req->command == HYPER_DMABUF_EXPORT_FD_FAILED) {
		dev_dbg(hyper_dmabuf_private.device,
			"Processing HYPER_DMABUF_EXPORT_FD_FAILED for buffer {id:%d key:%d %d %d}\n",
			hid.id, hid.rng_key[0], hid.rng_key[1], hid.rng_key[2]);

		exp_sgt_info = hyper_dmabuf_find_exported(hid);

		if (!exp_sgt_info) {
			dev_err(hyper_dmabuf_private.device,
				"critical err: requested sgt_info can't be found for buffer {id:%d key:%d %d %d}\n",
				hid.id, hid.rng_key[0], hid.rng_key[1], hid.rng_key[2]);

			req->status = HYPER_DMABUF_REQ_ERROR;
		} else {
			exp_sgt_info->importer_exported--;
			req->status = HYPER_DMABUF_REQ_PROCESSED;
		}
		return req->command;
	}

	dev_dbg(hyper_dmabuf_private.device,
		"%s: putting request to workqueue\n", __func__);
	temp_req = kmalloc(sizeof(*temp_req), GFP_KERNEL);

	if (!temp_req) {
		dev_err(hyper_dmabuf_private.device,
			"No memory left to be allocated\n");
		return -ENOMEM;
	}

	memcpy(temp_req, req, sizeof(*temp_req));

	proc = kcalloc(1, sizeof(struct cmd_process), GFP_KERNEL);

	if (!proc) {
		dev_err(hyper_dmabuf_private.device,
			"No memory left to be allocated\n");
		kfree(temp_req);
		return -ENOMEM;
	}

	proc->rq = temp_req;
	proc->domid = domid;

	INIT_WORK(&(proc->work), cmd_process_work);

	queue_work(hyper_dmabuf_private.work_queue, &(proc->work));

	return req->command;
}
