/*
 * Copyright © 2018 Intel Corporation
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
 * SPDX-License-Identifier: (MIT OR GPL-2.0)
 *
 * Authors:
 *    Dongwon Kim <dongwon.kim@intel.com>
 *    Mateusz Polrola <mateuszx.potrola@intel.com>
 *
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/dma-buf.h>
#include "hyper_dmabuf_drv.h"
#include "hyper_dmabuf_struct.h"
#include "hyper_dmabuf_ops.h"
#include "hyper_dmabuf_sgl_proc.h"
#include "hyper_dmabuf_id.h"
#include "hyper_dmabuf_msg.h"
#include "hyper_dmabuf_list.h"

#define WAIT_AFTER_SYNC_REQ 0
#define REFS_PER_PAGE (PAGE_SIZE/sizeof(grant_ref_t))

static int dmabuf_refcount(struct dma_buf *dma_buf)
{
	if ((dma_buf != NULL) && (dma_buf->file != NULL))
		return file_count(dma_buf->file);

	return -EINVAL;
}

static int hyper_dmabuf_ops_attach(struct dma_buf *dmabuf,
				   struct device *dev,
				   struct dma_buf_attachment *attach)
{
	return 0;
}

static void hyper_dmabuf_ops_detach(struct dma_buf *dmabuf,
				    struct dma_buf_attachment *attach)
{
}

static struct sg_table *hyper_dmabuf_ops_map(
				struct dma_buf_attachment *attachment,
				enum dma_data_direction dir)
{
	struct sg_table *st;
	struct imported_sgt_info *imported;
	struct pages_info *pg_info;

	if (!attachment->dmabuf->priv)
		return NULL;

	imported = (struct imported_sgt_info *)attachment->dmabuf->priv;

	/* extract pages from sgt */
	pg_info = hyper_dmabuf_ext_pgs(imported->sgt);

	if (!pg_info)
		return NULL;

	/* create a new sg_table with extracted pages */
	st = hyper_dmabuf_create_sgt(pg_info->pgs, pg_info->frst_ofst,
				     pg_info->last_len, pg_info->nents);
	if (!st)
		goto err_free_sg;

	if (!dma_map_sg(attachment->dev, st->sgl, st->nents, dir))
		goto err_free_sg;

	kfree(pg_info->pgs);
	kfree(pg_info);

	return st;

err_free_sg:
	if (st) {
		sg_free_table(st);
		kfree(st);
	}

	kfree(pg_info->pgs);
	kfree(pg_info);

	return NULL;
}

static void hyper_dmabuf_ops_unmap(struct dma_buf_attachment *attachment,
				   struct sg_table *sg,
				   enum dma_data_direction dir)
{
	struct imported_sgt_info *imported;

	if (!attachment->dmabuf->priv)
		return;

	imported = (struct imported_sgt_info *)attachment->dmabuf->priv;

	dma_unmap_sg(attachment->dev, sg->sgl, sg->nents, dir);

	sg_free_table(sg);
	kfree(sg);
}

static void hyper_dmabuf_ops_release(struct dma_buf *dma_buf)
{
	struct imported_sgt_info *imported;
	struct hyper_dmabuf_bknd_ops *bknd_ops = hy_drv_priv->bknd_ops;
	int finish;

	if (!dma_buf->priv)
		return;

	imported = (struct imported_sgt_info *)dma_buf->priv;

	if (!dmabuf_refcount(imported->dma_buf))
		imported->dma_buf = NULL;

	imported->importers--;

	if (imported->importers == 0) {
		bknd_ops->unmap_shared_pages(&imported->refs_info,
					     imported->nents);

		if (imported->sgt) {
			sg_free_table(imported->sgt);
			kfree(imported->sgt);
			imported->sgt = NULL;
		}
	}

	finish = imported && !imported->valid &&
		 !imported->importers;

	/*
	 * Check if buffer is still valid and if not remove it
	 * from imported list. That has to be done after sending
	 * sync request
	 */
	if (finish) {
		hyper_dmabuf_remove_imported(imported->hid);
		kfree(imported);
	}
}

static int hyper_dmabuf_ops_begin_cpu_access(struct dma_buf *dmabuf,
					     enum dma_data_direction dir)
{
	return 0;
}

static int hyper_dmabuf_ops_end_cpu_access(struct dma_buf *dmabuf,
					   enum dma_data_direction dir)
{
	return 0;
}

static void *hyper_dmabuf_ops_kmap_atomic(struct dma_buf *dmabuf,
					  unsigned long pgnum)
{
	/* TODO: NULL for now. Need to return the addr of mapped region */
	return NULL;
}

static void hyper_dmabuf_ops_kunmap_atomic(struct dma_buf *dmabuf,
					   unsigned long pgnum, void *vaddr)
{
}

static void *hyper_dmabuf_ops_kmap(struct dma_buf *dmabuf, unsigned long pgnum)
{
	/* for now NULL.. need to return the address of mapped region */
	return NULL;
}

static void hyper_dmabuf_ops_kunmap(struct dma_buf *dmabuf, unsigned long pgnum,
				    void *vaddr)
{
}

static int hyper_dmabuf_ops_mmap(struct dma_buf *dmabuf,
				 struct vm_area_struct *vma)
{
	return 0;
}

static void *hyper_dmabuf_ops_vmap(struct dma_buf *dmabuf)
{
	return NULL;
}

static void hyper_dmabuf_ops_vunmap(struct dma_buf *dmabuf, void *vaddr)
{
}

static const struct dma_buf_ops hyper_dmabuf_ops = {
	.attach = hyper_dmabuf_ops_attach,
	.detach = hyper_dmabuf_ops_detach,
	.map_dma_buf = hyper_dmabuf_ops_map,
	.unmap_dma_buf = hyper_dmabuf_ops_unmap,
	.release = hyper_dmabuf_ops_release,
	.begin_cpu_access = (void *)hyper_dmabuf_ops_begin_cpu_access,
	.end_cpu_access = (void *)hyper_dmabuf_ops_end_cpu_access,
	.map_atomic = hyper_dmabuf_ops_kmap_atomic,
	.unmap_atomic = hyper_dmabuf_ops_kunmap_atomic,
	.map = hyper_dmabuf_ops_kmap,
	.unmap = hyper_dmabuf_ops_kunmap,
	.mmap = hyper_dmabuf_ops_mmap,
	.vmap = hyper_dmabuf_ops_vmap,
	.vunmap = hyper_dmabuf_ops_vunmap,
};

/* exporting dmabuf as fd */
int hyper_dmabuf_export_fd(struct imported_sgt_info *imported, int flags)
{
	int fd = -1;

	/* call hyper_dmabuf_export_dmabuf and create
	 * and bind a handle for it then release
	 */
	hyper_dmabuf_export_dma_buf(imported);

	if (imported->dma_buf)
		fd = dma_buf_fd(imported->dma_buf, flags);

	return fd;
}

void hyper_dmabuf_export_dma_buf(struct imported_sgt_info *imported)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);

	exp_info.ops = &hyper_dmabuf_ops;

	/* multiple of PAGE_SIZE, not considering offset */
	exp_info.size = imported->sgt->nents * PAGE_SIZE;
	exp_info.flags = /* not sure about flag */ 0;
	exp_info.priv = imported;

	imported->dma_buf = dma_buf_export(&exp_info);
}
