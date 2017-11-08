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
#include <linux/miscdevice.h>
#include <asm/uaccess.h>
#include <linux/dma-buf.h>
#include <linux/delay.h>
#include <linux/list.h>
#include "hyper_dmabuf_drv.h"
#include "hyper_dmabuf_id.h"
#include "hyper_dmabuf_struct.h"
#include "hyper_dmabuf_ioctl.h"
#include "hyper_dmabuf_list.h"
#include "hyper_dmabuf_msg.h"
#include "hyper_dmabuf_imp.h"
#include "hyper_dmabuf_query.h"

extern struct hyper_dmabuf_private hyper_dmabuf_private;

static int hyper_dmabuf_tx_ch_setup(struct file *filp, void *data)
{
	struct ioctl_hyper_dmabuf_tx_ch_setup *tx_ch_attr;
	struct hyper_dmabuf_backend_ops *ops = hyper_dmabuf_private.backend_ops;
	int ret = 0;

	if (!data) {
		dev_err(hyper_dmabuf_private.device, "user data is NULL\n");
		return -EINVAL;
	}
	tx_ch_attr = (struct ioctl_hyper_dmabuf_tx_ch_setup *)data;

	ret = ops->init_tx_ch(tx_ch_attr->remote_domain);

	return ret;
}

static int hyper_dmabuf_rx_ch_setup(struct file *filp, void *data)
{
	struct ioctl_hyper_dmabuf_rx_ch_setup *rx_ch_attr;
	struct hyper_dmabuf_backend_ops *ops = hyper_dmabuf_private.backend_ops;
	int ret = 0;

	if (!data) {
		dev_err(hyper_dmabuf_private.device, "user data is NULL\n");
		return -EINVAL;
	}

	rx_ch_attr = (struct ioctl_hyper_dmabuf_rx_ch_setup *)data;

	ret = ops->init_rx_ch(rx_ch_attr->source_domain);

	return ret;
}

static int hyper_dmabuf_export_remote(struct file *filp, void *data)
{
	struct ioctl_hyper_dmabuf_export_remote *export_remote_attr;
	struct hyper_dmabuf_backend_ops *ops = hyper_dmabuf_private.backend_ops;
	struct dma_buf *dma_buf;
	struct dma_buf_attachment *attachment;
	struct sg_table *sgt;
	struct hyper_dmabuf_pages_info *page_info;
	struct hyper_dmabuf_sgt_info *sgt_info;
	struct hyper_dmabuf_req *req;
	int operands[MAX_NUMBER_OF_OPERANDS];
	hyper_dmabuf_id_t hid;
	int i;
	int ret = 0;

	if (!data) {
		dev_err(hyper_dmabuf_private.device, "user data is NULL\n");
		return -EINVAL;
	}

	export_remote_attr = (struct ioctl_hyper_dmabuf_export_remote *)data;

	dma_buf = dma_buf_get(export_remote_attr->dmabuf_fd);

	if (IS_ERR(dma_buf)) {
		dev_err(hyper_dmabuf_private.device,  "Cannot get dma buf\n");
		return PTR_ERR(dma_buf);
	}

	/* we check if this specific attachment was already exported
	 * to the same domain and if yes and it's valid sgt_info,
	 * it returns hyper_dmabuf_id of pre-exported sgt_info
	 */
	hid = hyper_dmabuf_find_hid_exported(dma_buf, export_remote_attr->remote_domain);
	if (hid.id != -1) {
		sgt_info = hyper_dmabuf_find_exported(hid);
		if (sgt_info != NULL) {
			if (sgt_info->valid) {
				/*
				 * Check if unexport is already scheduled for that buffer,
				 * if so try to cancel it. If that will fail, buffer needs
				 * to be reexport once again.
				 */
				if (sgt_info->unexport_scheduled) {
					if (!cancel_delayed_work_sync(&sgt_info->unexport_work)) {
						dma_buf_put(dma_buf);
						goto reexport;
					}
					sgt_info->unexport_scheduled = 0;
				}
				dma_buf_put(dma_buf);
				export_remote_attr->hid = hid;
				return 0;
			}
		}
	}

reexport:
	attachment = dma_buf_attach(dma_buf, hyper_dmabuf_private.device);
	if (IS_ERR(attachment)) {
		dev_err(hyper_dmabuf_private.device, "Cannot get attachment\n");
		return PTR_ERR(attachment);
	}

	sgt = dma_buf_map_attachment(attachment, DMA_BIDIRECTIONAL);

	if (IS_ERR(sgt)) {
		dev_err(hyper_dmabuf_private.device, "Cannot map attachment\n");
		return PTR_ERR(sgt);
	}

	sgt_info = kcalloc(1, sizeof(*sgt_info), GFP_KERNEL);

	if(!sgt_info) {
		dev_err(hyper_dmabuf_private.device, "no more space left\n");
		return -ENOMEM;
	}

	sgt_info->hid = hyper_dmabuf_get_hid();

	/* no more exported dmabuf allowed */
	if(sgt_info->hid.id == -1) {
		dev_err(hyper_dmabuf_private.device,
			"exceeds allowed number of dmabuf to be exported\n");
		/* TODO: Cleanup sgt */
		return -ENOMEM;
	}

	/* TODO: We might need to consider using port number on event channel? */
	sgt_info->hyper_dmabuf_rdomain = export_remote_attr->remote_domain;
	sgt_info->dma_buf = dma_buf;
	sgt_info->valid = 1;

	sgt_info->active_sgts = kmalloc(sizeof(struct sgt_list), GFP_KERNEL);
	if (!sgt_info->active_sgts) {
		dev_err(hyper_dmabuf_private.device, "no more space left\n");
		ret = -ENOMEM;
		goto fail_map_active_sgts;
	}

	sgt_info->active_attached = kmalloc(sizeof(struct attachment_list), GFP_KERNEL);
	if (!sgt_info->active_attached) {
		dev_err(hyper_dmabuf_private.device, "no more space left\n");
		ret = -ENOMEM;
		goto fail_map_active_attached;
	}

	sgt_info->va_kmapped = kmalloc(sizeof(struct kmap_vaddr_list), GFP_KERNEL);
	if (!sgt_info->va_kmapped) {
		dev_err(hyper_dmabuf_private.device, "no more space left\n");
		ret = -ENOMEM;
		goto fail_map_va_kmapped;
	}

	sgt_info->va_vmapped = kmalloc(sizeof(struct vmap_vaddr_list), GFP_KERNEL);
	if (!sgt_info->va_vmapped) {
		dev_err(hyper_dmabuf_private.device, "no more space left\n");
		ret = -ENOMEM;
		goto fail_map_va_vmapped;
	}

	sgt_info->active_sgts->sgt = sgt;
	sgt_info->active_attached->attach = attachment;
	sgt_info->va_kmapped->vaddr = NULL;
	sgt_info->va_vmapped->vaddr = NULL;

	/* initialize list of sgt, attachment and vaddr for dmabuf sync
	 * via shadow dma-buf
	 */
	INIT_LIST_HEAD(&sgt_info->active_sgts->list);
	INIT_LIST_HEAD(&sgt_info->active_attached->list);
	INIT_LIST_HEAD(&sgt_info->va_kmapped->list);
	INIT_LIST_HEAD(&sgt_info->va_vmapped->list);

	page_info = hyper_dmabuf_ext_pgs(sgt);
	if (!page_info) {
		dev_err(hyper_dmabuf_private.device, "failed to construct page_info\n");
		goto fail_export;
	}

	sgt_info->nents = page_info->nents;

	/* now register it to export list */
	hyper_dmabuf_register_exported(sgt_info);

	page_info->hyper_dmabuf_rdomain = sgt_info->hyper_dmabuf_rdomain;
	page_info->hid = sgt_info->hid; /* may not be needed */

	export_remote_attr->hid = sgt_info->hid;

	/* now create request for importer via ring */
	operands[0] = page_info->hid.id;

	for (i=0; i<3; i++)
		operands[i+1] = page_info->hid.rng_key[i];

	operands[4] = page_info->nents;
	operands[5] = page_info->frst_ofst;
	operands[6] = page_info->last_len;
	operands[7] = ops->share_pages (page_info->pages, export_remote_attr->remote_domain,
					page_info->nents, &sgt_info->refs_info);
	if (operands[7] < 0) {
		dev_err(hyper_dmabuf_private.device, "pages sharing failed\n");
		goto fail_map_req;
	}

	/* driver/application specific private info, max 4x4 bytes */
	operands[8] = export_remote_attr->private[0];
	operands[9] = export_remote_attr->private[1];
	operands[10] = export_remote_attr->private[2];
	operands[11] = export_remote_attr->private[3];

	req = kcalloc(1, sizeof(*req), GFP_KERNEL);

	if(!req) {
		dev_err(hyper_dmabuf_private.device, "no more space left\n");
		goto fail_map_req;
	}

	/* composing a message to the importer */
	hyper_dmabuf_create_request(req, HYPER_DMABUF_EXPORT, &operands[0]);

	ret = ops->send_req(export_remote_attr->remote_domain, req, false);

	if(ret) {
		dev_err(hyper_dmabuf_private.device, "error while communicating\n");
		goto fail_send_request;
	}

	/* free msg */
	kfree(req);
	/* free page_info */
	kfree(page_info->pages);
	kfree(page_info);

	sgt_info->filp = filp;

	return ret;

fail_send_request:
	kfree(req);

fail_map_req:
	hyper_dmabuf_remove_exported(sgt_info->hid);

fail_export:
	dma_buf_unmap_attachment(sgt_info->active_attached->attach,
				 sgt_info->active_sgts->sgt,
				 DMA_BIDIRECTIONAL);
	dma_buf_detach(sgt_info->dma_buf, sgt_info->active_attached->attach);
	dma_buf_put(sgt_info->dma_buf);

	kfree(sgt_info->va_vmapped);
fail_map_va_vmapped:
	kfree(sgt_info->va_kmapped);
fail_map_va_kmapped:
	kfree(sgt_info->active_sgts);
fail_map_active_sgts:
	kfree(sgt_info->active_attached);
fail_map_active_attached:

	return ret;
}

static int hyper_dmabuf_export_fd_ioctl(struct file *filp, void *data)
{
	struct ioctl_hyper_dmabuf_export_fd *export_fd_attr;
	struct hyper_dmabuf_backend_ops *ops = hyper_dmabuf_private.backend_ops;
	struct hyper_dmabuf_imported_sgt_info *sgt_info;
	struct hyper_dmabuf_req *req;
	struct page **data_pages;
	int operands[4];
	int i;
	int ret = 0;

	dev_dbg(hyper_dmabuf_private.device, "%s entry\n", __func__);

	if (!data) {
		dev_err(hyper_dmabuf_private.device, "user data is NULL\n");
		return -EINVAL;
	}

	export_fd_attr = (struct ioctl_hyper_dmabuf_export_fd *)data;

	/* look for dmabuf for the id */
	sgt_info = hyper_dmabuf_find_imported(export_fd_attr->hid);

	/* can't find sgt from the table */
	if (!sgt_info) {
		dev_err(hyper_dmabuf_private.device, "can't find the entry\n");
		return -ENOENT;
	}

	mutex_lock(&hyper_dmabuf_private.lock);

	sgt_info->num_importers++;

	/* send notification for export_fd to exporter */
	operands[0] = sgt_info->hid.id;

	for (i=0; i<3; i++)
		operands[i+1] = sgt_info->hid.rng_key[i];

	dev_dbg(hyper_dmabuf_private.device, "Exporting fd of buffer {id:%d key:%d %d %d}\n",
		sgt_info->hid.id, sgt_info->hid.rng_key[0], sgt_info->hid.rng_key[1],
		sgt_info->hid.rng_key[2]);

	req = kcalloc(1, sizeof(*req), GFP_KERNEL);

	if (!req) {
		dev_err(hyper_dmabuf_private.device,
			"No memory left to be allocated\n");
		return -ENOMEM;
	}

	hyper_dmabuf_create_request(req, HYPER_DMABUF_EXPORT_FD, &operands[0]);

	ret = ops->send_req(HYPER_DMABUF_DOM_ID(sgt_info->hid), req, true);

	if (ret < 0) {
		/* in case of timeout other end eventually will receive request, so we need to undo it */
		hyper_dmabuf_create_request(req, HYPER_DMABUF_EXPORT_FD_FAILED, &operands[0]);
		ops->send_req(operands[0], req, false);
		kfree(req);
		dev_err(hyper_dmabuf_private.device, "Failed to create sgt or notify exporter\n");
		sgt_info->num_importers--;
		mutex_unlock(&hyper_dmabuf_private.lock);
		return ret;
	}

	kfree(req);

	if (ret == HYPER_DMABUF_REQ_ERROR) {
		dev_err(hyper_dmabuf_private.device,
			"Buffer invalid {id:%d key:%d %d %d}, cannot import\n",
			sgt_info->hid.id, sgt_info->hid.rng_key[0], sgt_info->hid.rng_key[1],
			sgt_info->hid.rng_key[2]);

		sgt_info->num_importers--;
		mutex_unlock(&hyper_dmabuf_private.lock);
		return -EINVAL;
	} else {
		dev_dbg(hyper_dmabuf_private.device, "Can import buffer {id:%d key:%d %d %d}\n",
			sgt_info->hid.id, sgt_info->hid.rng_key[0], sgt_info->hid.rng_key[1],
			sgt_info->hid.rng_key[2]);

		ret = 0;
	}

	dev_dbg(hyper_dmabuf_private.device,
		  "%s Found buffer gref %d  off %d last len %d nents %d domain %d\n", __func__,
		  sgt_info->ref_handle, sgt_info->frst_ofst,
		  sgt_info->last_len, sgt_info->nents,
		  HYPER_DMABUF_DOM_ID(sgt_info->hid));

	if (!sgt_info->sgt) {
		dev_dbg(hyper_dmabuf_private.device,
			"%s buffer {id:%d key:%d %d %d} pages not mapped yet\n", __func__,
			sgt_info->hid.id, sgt_info->hid.rng_key[0], sgt_info->hid.rng_key[1],
			sgt_info->hid.rng_key[2]);

		data_pages = ops->map_shared_pages(sgt_info->ref_handle,
						   HYPER_DMABUF_DOM_ID(sgt_info->hid),
						   sgt_info->nents,
						   &sgt_info->refs_info);

		if (!data_pages) {
			dev_err(hyper_dmabuf_private.device,
				"Cannot map pages of buffer {id:%d key:%d %d %d}\n",
				sgt_info->hid.id, sgt_info->hid.rng_key[0], sgt_info->hid.rng_key[1],
				sgt_info->hid.rng_key[2]);

			sgt_info->num_importers--;
			req = kcalloc(1, sizeof(*req), GFP_KERNEL);
			hyper_dmabuf_create_request(req, HYPER_DMABUF_EXPORT_FD_FAILED, &operands[0]);
			ops->send_req(HYPER_DMABUF_DOM_ID(sgt_info->hid), req, false);
			kfree(req);
			mutex_unlock(&hyper_dmabuf_private.lock);
			return -EINVAL;
		}

		sgt_info->sgt = hyper_dmabuf_create_sgt(data_pages, sgt_info->frst_ofst,
							sgt_info->last_len, sgt_info->nents);

	}

	export_fd_attr->fd = hyper_dmabuf_export_fd(sgt_info, export_fd_attr->flags);

	if (export_fd_attr->fd < 0) {
		/* fail to get fd */
		ret = export_fd_attr->fd;
	}

	mutex_unlock(&hyper_dmabuf_private.lock);

	dev_dbg(hyper_dmabuf_private.device, "%s exit\n", __func__);
	return ret;
}

/* unexport dmabuf from the database and send int req to the source domain
 * to unmap it.
 */
static void hyper_dmabuf_delayed_unexport(struct work_struct *work)
{
	struct hyper_dmabuf_req *req;
	int i, ret;
	int operands[4];
	struct hyper_dmabuf_backend_ops *ops = hyper_dmabuf_private.backend_ops;
	struct hyper_dmabuf_sgt_info *sgt_info =
		container_of(work, struct hyper_dmabuf_sgt_info, unexport_work.work);

	if (!sgt_info)
		return;

	dev_dbg(hyper_dmabuf_private.device,
		"Marking buffer {id:%d key:%d %d %d} as invalid\n",
		sgt_info->hid.id, sgt_info->hid.rng_key[0], sgt_info->hid.rng_key[1],
		sgt_info->hid.rng_key[2]);

	/* no longer valid */
	sgt_info->valid = 0;

	req = kcalloc(1, sizeof(*req), GFP_KERNEL);

	if (!req) {
		dev_err(hyper_dmabuf_private.device,
			"No memory left to be allocated\n");
		return;
	}

	operands[0] = sgt_info->hid.id;

	for (i=0; i<3; i++)
		operands[i+1] = sgt_info->hid.rng_key[i];

	hyper_dmabuf_create_request(req, HYPER_DMABUF_NOTIFY_UNEXPORT, &operands[0]);

	/* Now send unexport request to remote domain, marking that buffer should not be used anymore */
	ret = ops->send_req(sgt_info->hyper_dmabuf_rdomain, req, true);
	if (ret < 0) {
		dev_err(hyper_dmabuf_private.device,
			"unexport message for buffer {id:%d key:%d %d %d} failed\n",
			sgt_info->hid.id, sgt_info->hid.rng_key[0], sgt_info->hid.rng_key[1],
			sgt_info->hid.rng_key[2]);
	}

	/* free msg */
	kfree(req);
	sgt_info->unexport_scheduled = 0;

	/*
	 * Immediately clean-up if it has never been exported by importer
	 * (so no SGT is constructed on importer).
	 * clean it up later in remote sync when final release ops
	 * is called (importer does this only when there's no
	 * no consumer of locally exported FDs)
	 */
	if (!sgt_info->importer_exported) {
		dev_dbg(hyper_dmabuf_private.device,
			"claning up buffer {id:%d key:%d %d %d} completly\n",
			sgt_info->hid.id, sgt_info->hid.rng_key[0], sgt_info->hid.rng_key[1],
			sgt_info->hid.rng_key[2]);

		hyper_dmabuf_cleanup_sgt_info(sgt_info, false);
		hyper_dmabuf_remove_exported(sgt_info->hid);
		/* register hyper_dmabuf_id to the list for reuse */
		store_reusable_hid(sgt_info->hid);
		kfree(sgt_info);
	}
}

/* Schedules unexport of dmabuf.
 */
static int hyper_dmabuf_unexport(struct file *filp, void *data)
{
	struct ioctl_hyper_dmabuf_unexport *unexport_attr;
	struct hyper_dmabuf_sgt_info *sgt_info;

	dev_dbg(hyper_dmabuf_private.device, "%s entry\n", __func__);

	if (!data) {
		dev_err(hyper_dmabuf_private.device, "user data is NULL\n");
		return -EINVAL;
	}

	unexport_attr = (struct ioctl_hyper_dmabuf_unexport *)data;

	/* find dmabuf in export list */
	sgt_info = hyper_dmabuf_find_exported(unexport_attr->hid);

	dev_dbg(hyper_dmabuf_private.device,
		"scheduling unexport of buffer {id:%d key:%d %d %d}\n",
		unexport_attr->hid.id, unexport_attr->hid.rng_key[0],
		unexport_attr->hid.rng_key[1], unexport_attr->hid.rng_key[2]);

	/* failed to find corresponding entry in export list */
	if (sgt_info == NULL) {
		unexport_attr->status = -ENOENT;
		return -ENOENT;
	}

	if (sgt_info->unexport_scheduled)
		return 0;

	sgt_info->unexport_scheduled = 1;
	INIT_DELAYED_WORK(&sgt_info->unexport_work, hyper_dmabuf_delayed_unexport);
	schedule_delayed_work(&sgt_info->unexport_work,
			      msecs_to_jiffies(unexport_attr->delay_ms));

	dev_dbg(hyper_dmabuf_private.device, "%s exit\n", __func__);
	return 0;
}

static int hyper_dmabuf_query(struct file *filp, void *data)
{
	struct ioctl_hyper_dmabuf_query *query_attr;
	struct hyper_dmabuf_sgt_info *sgt_info;
	struct hyper_dmabuf_imported_sgt_info *imported_sgt_info;
	int ret = 0;

	if (!data) {
		dev_err(hyper_dmabuf_private.device, "user data is NULL\n");
		return -EINVAL;
	}

	query_attr = (struct ioctl_hyper_dmabuf_query *)data;

	sgt_info = hyper_dmabuf_find_exported(query_attr->hid);
	imported_sgt_info = hyper_dmabuf_find_imported(query_attr->hid);

	/* if dmabuf can't be found in both lists, return */
	if (!(sgt_info && imported_sgt_info)) {
		dev_err(hyper_dmabuf_private.device, "can't find entry anywhere\n");
		return -ENOENT;
	}

	/* not considering the case where a dmabuf is found on both queues
	 * in one domain */
	switch (query_attr->item)
	{
		case DMABUF_QUERY_TYPE_LIST:
			if (sgt_info) {
				query_attr->info = EXPORTED;
			} else {
				query_attr->info = IMPORTED;
			}
			break;

		/* exporting domain of this specific dmabuf*/
		case DMABUF_QUERY_EXPORTER:
			if (sgt_info) {
				query_attr->info = 0xFFFFFFFF; /* myself */
			} else {
				query_attr->info = HYPER_DMABUF_DOM_ID(imported_sgt_info->hid);
			}
			break;

		/* importing domain of this specific dmabuf */
		case DMABUF_QUERY_IMPORTER:
			if (sgt_info) {
				query_attr->info = sgt_info->hyper_dmabuf_rdomain;
			} else {
#if 0 /* TODO: a global variable, current_domain does not exist yet*/
				query_attr->info = current_domain;
#endif
			}
			break;

		/* size of dmabuf in byte */
		case DMABUF_QUERY_SIZE:
			if (sgt_info) {
#if 0 /* TODO: hyper_dmabuf_buf_size is not implemented yet */
				query_attr->info = hyper_dmabuf_buf_size(sgt_info->sgt);
#endif
			} else {
				query_attr->info = imported_sgt_info->nents * 4096 -
						   imported_sgt_info->frst_ofst - 4096 +
						   imported_sgt_info->last_len;
			}
			break;
	}

	return ret;
}

static const struct hyper_dmabuf_ioctl_desc hyper_dmabuf_ioctls[] = {
	HYPER_DMABUF_IOCTL_DEF(IOCTL_HYPER_DMABUF_TX_CH_SETUP, hyper_dmabuf_tx_ch_setup, 0),
	HYPER_DMABUF_IOCTL_DEF(IOCTL_HYPER_DMABUF_RX_CH_SETUP, hyper_dmabuf_rx_ch_setup, 0),
	HYPER_DMABUF_IOCTL_DEF(IOCTL_HYPER_DMABUF_EXPORT_REMOTE, hyper_dmabuf_export_remote, 0),
	HYPER_DMABUF_IOCTL_DEF(IOCTL_HYPER_DMABUF_EXPORT_FD, hyper_dmabuf_export_fd_ioctl, 0),
	HYPER_DMABUF_IOCTL_DEF(IOCTL_HYPER_DMABUF_UNEXPORT, hyper_dmabuf_unexport, 0),
	HYPER_DMABUF_IOCTL_DEF(IOCTL_HYPER_DMABUF_QUERY, hyper_dmabuf_query, 0),
};

static long hyper_dmabuf_ioctl(struct file *filp,
			unsigned int cmd, unsigned long param)
{
	const struct hyper_dmabuf_ioctl_desc *ioctl = NULL;
	unsigned int nr = _IOC_NR(cmd);
	int ret;
	hyper_dmabuf_ioctl_t func;
	char *kdata;

	if (nr > ARRAY_SIZE(hyper_dmabuf_ioctls)) {
		dev_err(hyper_dmabuf_private.device, "invalid ioctl\n");
		return -EINVAL;
	}

	ioctl = &hyper_dmabuf_ioctls[nr];

	func = ioctl->func;

	if (unlikely(!func)) {
		dev_err(hyper_dmabuf_private.device, "no function\n");
		return -EINVAL;
	}

	kdata = kmalloc(_IOC_SIZE(cmd), GFP_KERNEL);
	if (!kdata) {
		dev_err(hyper_dmabuf_private.device, "no memory\n");
		return -ENOMEM;
	}

	if (copy_from_user(kdata, (void __user *)param, _IOC_SIZE(cmd)) != 0) {
		dev_err(hyper_dmabuf_private.device, "failed to copy from user arguments\n");
		return -EFAULT;
	}

	ret = func(filp, kdata);

	if (copy_to_user((void __user *)param, kdata, _IOC_SIZE(cmd)) != 0) {
		dev_err(hyper_dmabuf_private.device, "failed to copy to user arguments\n");
		return -EFAULT;
	}

	kfree(kdata);

	return ret;
}

int hyper_dmabuf_open(struct inode *inode, struct file *filp)
{
	int ret = 0;

	/* Do not allow exclusive open */
	if (filp->f_flags & O_EXCL)
		return -EBUSY;

	/*
	 * Initialize backend if neededm,
	 * use mutex to prevent race conditions when
	 * two userspace apps will open device at the same time
	 */
	mutex_lock(&hyper_dmabuf_private.lock);

	if (!hyper_dmabuf_private.backend_initialized) {
		hyper_dmabuf_private.domid = hyper_dmabuf_private.backend_ops->get_vm_id();

		ret = hyper_dmabuf_private.backend_ops->init_comm_env();
	        if (ret < 0) {
			dev_err(hyper_dmabuf_private.device,
				"failed to initiailize hypervisor-specific comm env\n");
		} else {
			hyper_dmabuf_private.backend_initialized = true;
		}
	}

	mutex_unlock(&hyper_dmabuf_private.lock);

	return ret;
}

static void hyper_dmabuf_emergency_release(struct hyper_dmabuf_sgt_info* sgt_info,
					   void *attr)
{
	struct ioctl_hyper_dmabuf_unexport unexport_attr;
	struct file *filp = (struct file*) attr;

	if (!filp || !sgt_info)
		return;

	if (sgt_info->filp == filp) {
		dev_dbg(hyper_dmabuf_private.device,
			"Executing emergency release of buffer {id:%d key:%d %d %d}\n",
			 sgt_info->hid.id, sgt_info->hid.rng_key[0],
			 sgt_info->hid.rng_key[1], sgt_info->hid.rng_key[2]);

		unexport_attr.hid = sgt_info->hid;
		unexport_attr.delay_ms = 0;

		hyper_dmabuf_unexport(filp, &unexport_attr);
	}
}

int hyper_dmabuf_release(struct inode *inode, struct file *filp)
{
	hyper_dmabuf_foreach_exported(hyper_dmabuf_emergency_release, filp);

	return 0;
}

/*===============================================================================================*/
static struct file_operations hyper_dmabuf_driver_fops =
{
   .owner = THIS_MODULE,
   .open = hyper_dmabuf_open,
   .release = hyper_dmabuf_release,
   .unlocked_ioctl = hyper_dmabuf_ioctl,
};

static struct miscdevice hyper_dmabuf_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "xen/hyper_dmabuf",
	.fops = &hyper_dmabuf_driver_fops,
};

static const char device_name[] = "hyper_dmabuf";

/*===============================================================================================*/
int register_device(void)
{
	int ret = 0;

	ret = misc_register(&hyper_dmabuf_miscdev);

	if (ret) {
		printk(KERN_ERR "hyper_dmabuf: driver can't be registered\n");
		return ret;
	}

	hyper_dmabuf_private.device = hyper_dmabuf_miscdev.this_device;

	/* TODO: Check if there is a different way to initialize dma mask nicely */
	dma_coerce_mask_and_coherent(hyper_dmabuf_private.device, DMA_BIT_MASK(64));

	return ret;
}

/*-----------------------------------------------------------------------------------------------*/
void unregister_device(void)
{
	dev_info(hyper_dmabuf_private.device,
		 "hyper_dmabuf: unregister_device() is called\n");

	misc_deregister(&hyper_dmabuf_miscdev);
}
