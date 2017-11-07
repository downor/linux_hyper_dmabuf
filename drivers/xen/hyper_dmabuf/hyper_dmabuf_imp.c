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
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/dma-buf.h>
#include <xen/grant_table.h>
#include <asm/xen/page.h>
#include "hyper_dmabuf_struct.h"
#include "hyper_dmabuf_imp.h"
#include "hyper_dmabuf_id.h"
#include "hyper_dmabuf_msg.h"
#include "hyper_dmabuf_drv.h"
#include "hyper_dmabuf_list.h"

extern struct hyper_dmabuf_private hyper_dmabuf_private;

#define REFS_PER_PAGE (PAGE_SIZE/sizeof(grant_ref_t))

int dmabuf_refcount(struct dma_buf *dma_buf)
{
	if ((dma_buf != NULL) && (dma_buf->file != NULL))
		return file_count(dma_buf->file);

	return -1;
}

/* return total number of pages referenced by a sgt
 * for pre-calculation of # of pages behind a given sgt
 */
static int hyper_dmabuf_get_num_pgs(struct sg_table *sgt)
{
	struct scatterlist *sgl;
	int length, i;
	/* at least one page */
	int num_pages = 1;

	sgl = sgt->sgl;

	length = sgl->length - PAGE_SIZE + sgl->offset;
	num_pages += ((length + PAGE_SIZE - 1)/PAGE_SIZE); /* round-up */

	for (i = 1; i < sgt->nents; i++) {
		sgl = sg_next(sgl);
		num_pages += ((sgl->length + PAGE_SIZE - 1) / PAGE_SIZE); /* round-up */
	}

	return num_pages;
}

/* extract pages directly from struct sg_table */
struct hyper_dmabuf_pages_info *hyper_dmabuf_ext_pgs(struct sg_table *sgt)
{
	struct hyper_dmabuf_pages_info *pinfo;
	int i, j, k;
	int length;
	struct scatterlist *sgl;

	pinfo = kmalloc(sizeof(*pinfo), GFP_KERNEL);
	if (pinfo == NULL)
		return NULL;

	pinfo->pages = kmalloc(sizeof(struct page *)*hyper_dmabuf_get_num_pgs(sgt), GFP_KERNEL);
	if (pinfo->pages == NULL)
		return NULL;

	sgl = sgt->sgl;

	pinfo->nents = 1;
	pinfo->frst_ofst = sgl->offset;
	pinfo->pages[0] = sg_page(sgl);
	length = sgl->length - PAGE_SIZE + sgl->offset;
	i = 1;

	while (length > 0) {
		pinfo->pages[i] = nth_page(sg_page(sgl), i);
		length -= PAGE_SIZE;
		pinfo->nents++;
		i++;
	}

	for (j = 1; j < sgt->nents; j++) {
		sgl = sg_next(sgl);
		pinfo->pages[i++] = sg_page(sgl);
		length = sgl->length - PAGE_SIZE;
		pinfo->nents++;
		k = 1;

		while (length > 0) {
			pinfo->pages[i++] = nth_page(sg_page(sgl), k++);
			length -= PAGE_SIZE;
			pinfo->nents++;
		}
	}

	/*
	 * lenght at that point will be 0 or negative,
	 * so to calculate last page size just add it to PAGE_SIZE
	 */
	pinfo->last_len = PAGE_SIZE + length;

	return pinfo;
}

/* create sg_table with given pages and other parameters */
struct sg_table* hyper_dmabuf_create_sgt(struct page **pages,
					 int frst_ofst, int last_len, int nents)
{
	struct sg_table *sgt;
	struct scatterlist *sgl;
	int i, ret;

	sgt = kmalloc(sizeof(struct sg_table), GFP_KERNEL);
	if (sgt == NULL) {
		return NULL;
	}

	ret = sg_alloc_table(sgt, nents, GFP_KERNEL);
	if (ret) {
		if (sgt) {
			sg_free_table(sgt);
			kfree(sgt);
		}

		return NULL;
	}

	sgl = sgt->sgl;

	sg_set_page(sgl, pages[0], PAGE_SIZE-frst_ofst, frst_ofst);

	for (i=1; i<nents-1; i++) {
		sgl = sg_next(sgl);
		sg_set_page(sgl, pages[i], PAGE_SIZE, 0);
	}

	if (nents > 1) /* more than one page */ {
		sgl = sg_next(sgl);
		sg_set_page(sgl, pages[i], last_len, 0);
	}

	return sgt;
}

int hyper_dmabuf_cleanup_sgt_info(struct hyper_dmabuf_sgt_info *sgt_info, int force)
{
	struct sgt_list *sgtl;
	struct attachment_list *attachl;
	struct kmap_vaddr_list *va_kmapl;
	struct vmap_vaddr_list *va_vmapl;
	struct hyper_dmabuf_backend_ops *ops = hyper_dmabuf_private.backend_ops;

	if (!sgt_info) {
		dev_err(hyper_dmabuf_private.device, "invalid hyper_dmabuf_id\n");
		return -EINVAL;
	}

	/* if force != 1, sgt_info can be released only if
	 * there's no activity on exported dma-buf on importer
	 * side.
	 */
	if (!force &&
	    (!list_empty(&sgt_info->va_kmapped->list) ||
	    !list_empty(&sgt_info->va_vmapped->list) ||
	    !list_empty(&sgt_info->active_sgts->list) ||
	    !list_empty(&sgt_info->active_attached->list))) {
		dev_warn(hyper_dmabuf_private.device, "dma-buf is used by importer\n");
		return -EPERM;
	}

	/* force == 1 is not recommended */
	while (!list_empty(&sgt_info->va_kmapped->list)) {
		va_kmapl = list_first_entry(&sgt_info->va_kmapped->list,
					    struct kmap_vaddr_list, list);

		dma_buf_kunmap(sgt_info->dma_buf, 1, va_kmapl->vaddr);
		list_del(&va_kmapl->list);
		kfree(va_kmapl);
	}

	while (!list_empty(&sgt_info->va_vmapped->list)) {
		va_vmapl = list_first_entry(&sgt_info->va_vmapped->list,
					    struct vmap_vaddr_list, list);

		dma_buf_vunmap(sgt_info->dma_buf, va_vmapl->vaddr);
		list_del(&va_vmapl->list);
		kfree(va_vmapl);
	}

	while (!list_empty(&sgt_info->active_sgts->list)) {
		attachl = list_first_entry(&sgt_info->active_attached->list,
					   struct attachment_list, list);

		sgtl = list_first_entry(&sgt_info->active_sgts->list,
					struct sgt_list, list);

		dma_buf_unmap_attachment(attachl->attach, sgtl->sgt,
					 DMA_BIDIRECTIONAL);
		list_del(&sgtl->list);
		kfree(sgtl);
	}

	while (!list_empty(&sgt_info->active_sgts->list)) {
		attachl = list_first_entry(&sgt_info->active_attached->list,
					   struct attachment_list, list);

		dma_buf_detach(sgt_info->dma_buf, attachl->attach);
		list_del(&attachl->list);
		kfree(attachl);
	}

	/* Start cleanup of buffer in reverse order to exporting */
	ops->unshare_pages(&sgt_info->refs_info, sgt_info->nents);

	/* unmap dma-buf */
	dma_buf_unmap_attachment(sgt_info->active_attached->attach,
				 sgt_info->active_sgts->sgt,
				 DMA_BIDIRECTIONAL);

	/* detatch dma-buf */
	dma_buf_detach(sgt_info->dma_buf, sgt_info->active_attached->attach);

	/* close connection to dma-buf completely */
	dma_buf_put(sgt_info->dma_buf);
	sgt_info->dma_buf = NULL;

	kfree(sgt_info->active_sgts);
	kfree(sgt_info->active_attached);
	kfree(sgt_info->va_kmapped);
	kfree(sgt_info->va_vmapped);

	return 0;
}

#define WAIT_AFTER_SYNC_REQ 1

inline int hyper_dmabuf_sync_request(int id, int dmabuf_ops)
{
	struct hyper_dmabuf_req *req;
	struct hyper_dmabuf_backend_ops *ops = hyper_dmabuf_private.backend_ops;
	int operands[2];
	int ret;

	operands[0] = id;
	operands[1] = dmabuf_ops;

	req = kcalloc(1, sizeof(*req), GFP_KERNEL);

	hyper_dmabuf_create_request(req, HYPER_DMABUF_OPS_TO_SOURCE, &operands[0]);

	/* send request and wait for a response */
	ret = ops->send_req(HYPER_DMABUF_DOM_ID(id), req, WAIT_AFTER_SYNC_REQ);

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

	ret = hyper_dmabuf_sync_request(sgt_info->hyper_dmabuf_id,
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

	ret = hyper_dmabuf_sync_request(sgt_info->hyper_dmabuf_id,
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
	if (st == NULL)
		goto err_free_sg;

        if (!dma_map_sg(attachment->dev, st->sgl, st->nents, dir)) {
                goto err_free_sg;
        }

	ret = hyper_dmabuf_sync_request(sgt_info->hyper_dmabuf_id,
					HYPER_DMABUF_OPS_MAP);

	kfree(page_info->pages);
	kfree(page_info);

	if (ret < 0) {
		dev_err(hyper_dmabuf_private.device,
			"hyper_dmabuf::%s Error:send dmabuf sync request failed\n", __func__);
	}

	return st;

err_free_sg:
	sg_free_table(st);
	kfree(st);
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

	ret = hyper_dmabuf_sync_request(sgt_info->hyper_dmabuf_id,
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

	if (final_release) {
		ret = hyper_dmabuf_sync_request(sgt_info->hyper_dmabuf_id,
						HYPER_DMABUF_OPS_RELEASE_FINAL);
	} else {
		ret = hyper_dmabuf_sync_request(sgt_info->hyper_dmabuf_id,
						HYPER_DMABUF_OPS_RELEASE);
	}

	if (ret < 0) {
		dev_err(hyper_dmabuf_private.device,
			"hyper_dmabuf::%s Error:send dmabuf sync request failed\n", __func__);
	}

	/*
	 * Check if buffer is still valid and if not remove it from imported list.
	 * That has to be done after sending sync request
	 */
	if (final_release) {
		hyper_dmabuf_remove_imported(sgt_info->hyper_dmabuf_id);
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

	ret = hyper_dmabuf_sync_request(sgt_info->hyper_dmabuf_id,
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

	ret = hyper_dmabuf_sync_request(sgt_info->hyper_dmabuf_id,
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

	ret = hyper_dmabuf_sync_request(sgt_info->hyper_dmabuf_id,
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

	ret = hyper_dmabuf_sync_request(sgt_info->hyper_dmabuf_id,
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

	ret = hyper_dmabuf_sync_request(sgt_info->hyper_dmabuf_id,
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

	ret = hyper_dmabuf_sync_request(sgt_info->hyper_dmabuf_id,
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

	ret = hyper_dmabuf_sync_request(sgt_info->hyper_dmabuf_id,
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

	ret = hyper_dmabuf_sync_request(sgt_info->hyper_dmabuf_id,
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

	ret = hyper_dmabuf_sync_request(sgt_info->hyper_dmabuf_id,
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
