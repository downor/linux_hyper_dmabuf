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
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/dma-buf.h>
#include <xen/grant_table.h>
#include <asm/xen/page.h>
#include "hyper_dmabuf_drv.h"
#include "hyper_dmabuf_struct.h"
#include "hyper_dmabuf_ops.h"
#include "hyper_dmabuf_sgl_proc.h"
#include "hyper_dmabuf_id.h"
#include "hyper_dmabuf_msg.h"
#include "hyper_dmabuf_list.h"

#define WAIT_AFTER_SYNC_REQ 0
#define REFS_PER_PAGE (PAGE_SIZE/sizeof(grant_ref_t))

extern struct hyper_dmabuf_private hyper_dmabuf_private;

inline int hyper_dmabuf_sync_request(hyper_dmabuf_id_t hid, int dmabuf_ops)
{
	struct hyper_dmabuf_req *req;
	struct hyper_dmabuf_backend_ops *ops = hyper_dmabuf_private.backend_ops;
	int operands[5];
	int i;
	int ret;

	operands[0] = hid.id;

	for (i=0; i<3; i++)
		operands[i+1] = hid.rng_key[i];

	operands[4] = dmabuf_ops;

	req = kcalloc(1, sizeof(*req), GFP_KERNEL);

	if (!req) {
		dev_err(hyper_dmabuf_private.device,
			"No memory left to be allocated\n");
		return -ENOMEM;
	}

	hyper_dmabuf_create_request(req, HYPER_DMABUF_OPS_TO_SOURCE, &operands[0]);

	/* send request and wait for a response */
	ret = ops->send_req(HYPER_DMABUF_DOM_ID(hid), req, WAIT_AFTER_SYNC_REQ);

	kfree(req);

	return ret;
}

static int hyper_dmabuf_ops_attach(struct dma_buf* dmabuf, struct device* dev,
			struct dma_buf_attachment *attach)
{
	struct hyper_dmabuf_imported_sgt_info *sgt_info;
	int ret;

	if (!attach->dmabuf->priv)
		return -EINVAL;

	sgt_info = (struct hyper_dmabuf_imported_sgt_info *)attach->dmabuf->priv;

	ret = hyper_dmabuf_sync_request(sgt_info->hid,
					HYPER_DMABUF_OPS_ATTACH);

	if (ret < 0) {
		dev_err(hyper_dmabuf_private.device,
			"hyper_dmabuf::%s Error:send dmabuf sync request failed\n", __func__);
		return ret;
	}

	return 0;
}

static void hyper_dmabuf_ops_detach(struct dma_buf* dmabuf, struct dma_buf_attachment *attach)
{
	struct hyper_dmabuf_imported_sgt_info *sgt_info;
	int ret;

	if (!attach->dmabuf->priv)
		return;

	sgt_info = (struct hyper_dmabuf_imported_sgt_info *)attach->dmabuf->priv;

	ret = hyper_dmabuf_sync_request(sgt_info->hid,
					HYPER_DMABUF_OPS_DETACH);

	if (ret < 0) {
		dev_err(hyper_dmabuf_private.device,
			"hyper_dmabuf::%s Error:send dmabuf sync request failed\n", __func__);
	}
}

static struct sg_table* hyper_dmabuf_ops_map(struct dma_buf_attachment *attachment,
						enum dma_data_direction dir)
{
	struct sg_table *st;
	struct hyper_dmabuf_imported_sgt_info *sgt_info;
	struct hyper_dmabuf_pages_info *page_info;
	int ret;

	if (!attachment->dmabuf->priv)
		return NULL;

	sgt_info = (struct hyper_dmabuf_imported_sgt_info *)attachment->dmabuf->priv;

	/* extract pages from sgt */
	page_info = hyper_dmabuf_ext_pgs(sgt_info->sgt);

	if (!page_info) {
		return NULL;
	}

	/* create a new sg_table with extracted pages */
	st = hyper_dmabuf_create_sgt(page_info->pages, page_info->frst_ofst,
				page_info->last_len, page_info->nents);
	if (!st)
		goto err_free_sg;

        if (!dma_map_sg(attachment->dev, st->sgl, st->nents, dir))
                goto err_free_sg;

	ret = hyper_dmabuf_sync_request(sgt_info->hid,
					HYPER_DMABUF_OPS_MAP);

	kfree(page_info->pages);
	kfree(page_info);

	if (ret < 0) {
		dev_err(hyper_dmabuf_private.device,
			"hyper_dmabuf::%s Error:send dmabuf sync request failed\n", __func__);
	}

	return st;

err_free_sg:
	if (st) {
		sg_free_table(st);
		kfree(st);
	}

	kfree(page_info->pages);
	kfree(page_info);

	return NULL;
}

static void hyper_dmabuf_ops_unmap(struct dma_buf_attachment *attachment,
				   struct sg_table *sg,
				   enum dma_data_direction dir)
{
	struct hyper_dmabuf_imported_sgt_info *sgt_info;
	int ret;

	if (!attachment->dmabuf->priv)
		return;

	sgt_info = (struct hyper_dmabuf_imported_sgt_info *)attachment->dmabuf->priv;

	dma_unmap_sg(attachment->dev, sg->sgl, sg->nents, dir);

	sg_free_table(sg);
	kfree(sg);

	ret = hyper_dmabuf_sync_request(sgt_info->hid,
					HYPER_DMABUF_OPS_UNMAP);

	if (ret < 0) {
		dev_err(hyper_dmabuf_private.device,
			"hyper_dmabuf::%s Error:send dmabuf sync request failed\n", __func__);
	}
}

static void hyper_dmabuf_ops_release(struct dma_buf *dma_buf)
{
	struct hyper_dmabuf_imported_sgt_info *sgt_info;
	struct hyper_dmabuf_backend_ops *ops = hyper_dmabuf_private.backend_ops;
	int ret;
	int final_release;

	if (!dma_buf->priv)
		return;

	sgt_info = (struct hyper_dmabuf_imported_sgt_info *)dma_buf->priv;

	if (!dmabuf_refcount(sgt_info->dma_buf)) {
		sgt_info->dma_buf = NULL;
	}

	sgt_info->num_importers--;

	if (sgt_info->num_importers == 0) {
		ops->unmap_shared_pages(&sgt_info->refs_info, sgt_info->nents);

		if (sgt_info->sgt) {
			sg_free_table(sgt_info->sgt);
			kfree(sgt_info->sgt);
			sgt_info->sgt = NULL;
		}
	}

	final_release = sgt_info && !sgt_info->valid &&
		        !sgt_info->num_importers;

	ret = hyper_dmabuf_sync_request(sgt_info->hid,
					HYPER_DMABUF_OPS_RELEASE);
	if (ret < 0) {
		dev_warn(hyper_dmabuf_private.device,
			 "hyper_dmabuf::%s Error:send dmabuf sync request failed\n", __func__);
	}

	/*
	 * Check if buffer is still valid and if not remove it from imported list.
	 * That has to be done after sending sync request
	 */
	if (final_release) {
		hyper_dmabuf_remove_imported(sgt_info->hid);
		kfree(sgt_info);
	}
}

static int hyper_dmabuf_ops_begin_cpu_access(struct dma_buf *dmabuf, enum dma_data_direction dir)
{
	struct hyper_dmabuf_imported_sgt_info *sgt_info;
	int ret;

	if (!dmabuf->priv)
		return -EINVAL;

	sgt_info = (struct hyper_dmabuf_imported_sgt_info *)dmabuf->priv;

	ret = hyper_dmabuf_sync_request(sgt_info->hid,
					HYPER_DMABUF_OPS_BEGIN_CPU_ACCESS);
	if (ret < 0) {
		dev_err(hyper_dmabuf_private.device,
			"hyper_dmabuf::%s Error:send dmabuf sync request failed\n", __func__);
	}

	return ret;
}

static int hyper_dmabuf_ops_end_cpu_access(struct dma_buf *dmabuf, enum dma_data_direction dir)
{
	struct hyper_dmabuf_imported_sgt_info *sgt_info;
	int ret;

	if (!dmabuf->priv)
		return -EINVAL;

	sgt_info = (struct hyper_dmabuf_imported_sgt_info *)dmabuf->priv;

	ret = hyper_dmabuf_sync_request(sgt_info->hid,
					HYPER_DMABUF_OPS_END_CPU_ACCESS);
	if (ret < 0) {
		dev_err(hyper_dmabuf_private.device,
			"hyper_dmabuf::%s Error:send dmabuf sync request failed\n", __func__);
	}

	return 0;
}

static void *hyper_dmabuf_ops_kmap_atomic(struct dma_buf *dmabuf, unsigned long pgnum)
{
	struct hyper_dmabuf_imported_sgt_info *sgt_info;
	int ret;

	if (!dmabuf->priv)
		return NULL;

	sgt_info = (struct hyper_dmabuf_imported_sgt_info *)dmabuf->priv;

	ret = hyper_dmabuf_sync_request(sgt_info->hid,
					HYPER_DMABUF_OPS_KMAP_ATOMIC);
	if (ret < 0) {
		dev_err(hyper_dmabuf_private.device,
			"hyper_dmabuf::%s Error:send dmabuf sync request failed\n", __func__);
	}

	return NULL; /* for now NULL.. need to return the address of mapped region */
}

static void hyper_dmabuf_ops_kunmap_atomic(struct dma_buf *dmabuf, unsigned long pgnum, void *vaddr)
{
	struct hyper_dmabuf_imported_sgt_info *sgt_info;
	int ret;

	if (!dmabuf->priv)
		return;

	sgt_info = (struct hyper_dmabuf_imported_sgt_info *)dmabuf->priv;

	ret = hyper_dmabuf_sync_request(sgt_info->hid,
					HYPER_DMABUF_OPS_KUNMAP_ATOMIC);
	if (ret < 0) {
		dev_err(hyper_dmabuf_private.device,
			"hyper_dmabuf::%s Error:send dmabuf sync request failed\n", __func__);
	}
}

static void *hyper_dmabuf_ops_kmap(struct dma_buf *dmabuf, unsigned long pgnum)
{
	struct hyper_dmabuf_imported_sgt_info *sgt_info;
	int ret;

	if (!dmabuf->priv)
		return NULL;

	sgt_info = (struct hyper_dmabuf_imported_sgt_info *)dmabuf->priv;

	ret = hyper_dmabuf_sync_request(sgt_info->hid,
					HYPER_DMABUF_OPS_KMAP);
	if (ret < 0) {
		dev_err(hyper_dmabuf_private.device,
			"hyper_dmabuf::%s Error:send dmabuf sync request failed\n", __func__);
	}

	return NULL; /* for now NULL.. need to return the address of mapped region */
}

static void hyper_dmabuf_ops_kunmap(struct dma_buf *dmabuf, unsigned long pgnum, void *vaddr)
{
	struct hyper_dmabuf_imported_sgt_info *sgt_info;
	int ret;

	if (!dmabuf->priv)
		return;

	sgt_info = (struct hyper_dmabuf_imported_sgt_info *)dmabuf->priv;

	ret = hyper_dmabuf_sync_request(sgt_info->hid,
					HYPER_DMABUF_OPS_KUNMAP);
	if (ret < 0) {
		dev_err(hyper_dmabuf_private.device,
			"hyper_dmabuf::%s Error:send dmabuf sync request failed\n", __func__);
	}
}

static int hyper_dmabuf_ops_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct hyper_dmabuf_imported_sgt_info *sgt_info;
	int ret;

	if (!dmabuf->priv)
		return -EINVAL;

	sgt_info = (struct hyper_dmabuf_imported_sgt_info *)dmabuf->priv;

	ret = hyper_dmabuf_sync_request(sgt_info->hid,
					HYPER_DMABUF_OPS_MMAP);
	if (ret < 0) {
		dev_err(hyper_dmabuf_private.device,
			"hyper_dmabuf::%s Error:send dmabuf sync request failed\n", __func__);
	}

	return ret;
}

static void *hyper_dmabuf_ops_vmap(struct dma_buf *dmabuf)
{
	struct hyper_dmabuf_imported_sgt_info *sgt_info;
	int ret;

	if (!dmabuf->priv)
		return NULL;

	sgt_info = (struct hyper_dmabuf_imported_sgt_info *)dmabuf->priv;

	ret = hyper_dmabuf_sync_request(sgt_info->hid,
					HYPER_DMABUF_OPS_VMAP);
	if (ret < 0) {
		dev_err(hyper_dmabuf_private.device,
			"hyper_dmabuf::%s Error:send dmabuf sync request failed\n", __func__);
	}

	return NULL;
}

static void hyper_dmabuf_ops_vunmap(struct dma_buf *dmabuf, void *vaddr)
{
	struct hyper_dmabuf_imported_sgt_info *sgt_info;
	int ret;

	if (!dmabuf->priv)
		return;

	sgt_info = (struct hyper_dmabuf_imported_sgt_info *)dmabuf->priv;

	ret = hyper_dmabuf_sync_request(sgt_info->hid,
					HYPER_DMABUF_OPS_VUNMAP);
	if (ret < 0) {
		dev_err(hyper_dmabuf_private.device,
			"hyper_dmabuf::%s Error:send dmabuf sync request failed\n", __func__);
	}
}

static const struct dma_buf_ops hyper_dmabuf_ops = {
		.attach = hyper_dmabuf_ops_attach,
		.detach = hyper_dmabuf_ops_detach,
		.map_dma_buf = hyper_dmabuf_ops_map,
		.unmap_dma_buf = hyper_dmabuf_ops_unmap,
		.release = hyper_dmabuf_ops_release,
		.begin_cpu_access = (void*)hyper_dmabuf_ops_begin_cpu_access,
		.end_cpu_access = (void*)hyper_dmabuf_ops_end_cpu_access,
		.map_atomic = hyper_dmabuf_ops_kmap_atomic,
		.unmap_atomic = hyper_dmabuf_ops_kunmap_atomic,
		.map = hyper_dmabuf_ops_kmap,
		.unmap = hyper_dmabuf_ops_kunmap,
		.mmap = hyper_dmabuf_ops_mmap,
		.vmap = hyper_dmabuf_ops_vmap,
		.vunmap = hyper_dmabuf_ops_vunmap,
};

/* exporting dmabuf as fd */
int hyper_dmabuf_export_fd(struct hyper_dmabuf_imported_sgt_info *dinfo, int flags)
{
	int fd = -1;

	/* call hyper_dmabuf_export_dmabuf and create
	 * and bind a handle for it then release
	 */
	hyper_dmabuf_export_dma_buf(dinfo);

	if (dinfo->dma_buf) {
		fd = dma_buf_fd(dinfo->dma_buf, flags);
	}

	return fd;
}

void hyper_dmabuf_export_dma_buf(struct hyper_dmabuf_imported_sgt_info *dinfo)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);

	exp_info.ops = &hyper_dmabuf_ops;

	/* multiple of PAGE_SIZE, not considering offset */
	exp_info.size = dinfo->sgt->nents * PAGE_SIZE;
	exp_info.flags = /* not sure about flag */0;
	exp_info.priv = dinfo;

	dinfo->dma_buf = dma_buf_export(&exp_info);
}
