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
#include "hyper_dmabuf_drv.h"
#include "hyper_dmabuf_struct.h"
#include "hyper_dmabuf_list.h"
#include "hyper_dmabuf_msg.h"
#include "hyper_dmabuf_id.h"
#include "hyper_dmabuf_msg.h"
#include "hyper_dmabuf_imp.h"

extern struct hyper_dmabuf_private hyper_dmabuf_private;

/* Whenever importer does dma operations from remote domain,
 * a notification is sent to the exporter so that exporter
 * issues equivalent dma operation on the original dma buf
 * for indirect synchronization via shadow operations.
 *
 * All ptrs and references (e.g struct sg_table*,
 * struct dma_buf_attachment) created via these operations on
 * exporter's side are kept in stack (implemented as circular
 * linked-lists) separately so that those can be re-referenced
 * later when unmapping operations are invoked to free those.
 *
 * The very first element on the bottom of each stack holds
 * is what is created when initial exporting is issued so it
 * should not be modified or released by this fuction.
 */
int hyper_dmabuf_remote_sync(hyper_dmabuf_id_t hid, int ops)
{
	struct hyper_dmabuf_sgt_info *sgt_info;
	struct sgt_list *sgtl;
	struct attachment_list *attachl;
	struct kmap_vaddr_list *va_kmapl;
	struct vmap_vaddr_list *va_vmapl;
	int ret;

	/* find a coresponding SGT for the id */
	sgt_info = hyper_dmabuf_find_exported(hid);

	if (!sgt_info) {
		dev_err(hyper_dmabuf_private.device,
			"dmabuf remote sync::can't find exported list\n");
		return -ENOENT;
	}

	switch (ops) {
	case HYPER_DMABUF_OPS_ATTACH:
		attachl = kcalloc(1, sizeof(*attachl), GFP_KERNEL);

		if (!attachl) {
			dev_err(hyper_dmabuf_private.device,
				"dmabuf remote sync::error while processing HYPER_DMABUF_OPS_ATTACH\n");
			return -ENOMEM;
		}

		attachl->attach = dma_buf_attach(sgt_info->dma_buf,
						 hyper_dmabuf_private.device);

		if (!attachl->attach) {
			kfree(attachl);
			dev_err(hyper_dmabuf_private.device,
				"dmabuf remote sync::error while processing HYPER_DMABUF_OPS_ATTACH\n");
			return -ENOMEM;
		}

		list_add(&attachl->list, &sgt_info->active_attached->list);
		break;

	case HYPER_DMABUF_OPS_DETACH:
		if (list_empty(&sgt_info->active_attached->list)) {
			dev_err(hyper_dmabuf_private.device,
				"dmabuf remote sync::error while processing HYPER_DMABUF_OPS_DETACH\n");
			dev_err(hyper_dmabuf_private.device,
				"no more dmabuf attachment left to be detached\n");
			return -EFAULT;
		}

		attachl = list_first_entry(&sgt_info->active_attached->list,
					   struct attachment_list, list);

		dma_buf_detach(sgt_info->dma_buf, attachl->attach);
		list_del(&attachl->list);
		kfree(attachl);
		break;

	case HYPER_DMABUF_OPS_MAP:
		if (list_empty(&sgt_info->active_attached->list)) {
			dev_err(hyper_dmabuf_private.device,
				"dmabuf remote sync::error while processing HYPER_DMABUF_OPS_MAP\n");
			dev_err(hyper_dmabuf_private.device,
				"no more dmabuf attachment left to be mapped\n");
			return -EFAULT;
		}

		attachl = list_first_entry(&sgt_info->active_attached->list,
					   struct attachment_list, list);

		sgtl = kcalloc(1, sizeof(*sgtl), GFP_KERNEL);

		if (!sgtl) {
			dev_err(hyper_dmabuf_private.device,
				"dmabuf remote sync::error while processing HYPER_DMABUF_OPS_MAP\n");
			return -ENOMEM;
		}

		sgtl->sgt = dma_buf_map_attachment(attachl->attach, DMA_BIDIRECTIONAL);
		if (!sgtl->sgt) {
			kfree(sgtl);
			dev_err(hyper_dmabuf_private.device,
				"dmabuf remote sync::error while processing HYPER_DMABUF_OPS_MAP\n");
			return -ENOMEM;
		}
		list_add(&sgtl->list, &sgt_info->active_sgts->list);
		break;

	case HYPER_DMABUF_OPS_UNMAP:
		if (list_empty(&sgt_info->active_sgts->list) ||
		    list_empty(&sgt_info->active_attached->list)) {
			dev_err(hyper_dmabuf_private.device,
				"dmabuf remote sync::error while processing HYPER_DMABUF_OPS_UNMAP\n");
			dev_err(hyper_dmabuf_private.device,
				"no more SGT or attachment left to be unmapped\n");
			return -EFAULT;
		}

		attachl = list_first_entry(&sgt_info->active_attached->list,
					   struct attachment_list, list);
		sgtl = list_first_entry(&sgt_info->active_sgts->list,
					struct sgt_list, list);

		dma_buf_unmap_attachment(attachl->attach, sgtl->sgt,
					 DMA_BIDIRECTIONAL);
		list_del(&sgtl->list);
		kfree(sgtl);
		break;

	case HYPER_DMABUF_OPS_RELEASE:
		dev_dbg(hyper_dmabuf_private.device,
			"Buffer {id:%d key:%d %d %d} released, references left: %d\n",
			 sgt_info->hid.id, sgt_info->hid.rng_key[0], sgt_info->hid.rng_key[1],
			 sgt_info->hid.rng_key[2], sgt_info->importer_exported -1);

                sgt_info->importer_exported--;
		/* If there are still importers just break, if no then continue with final cleanup */
		if (sgt_info->importer_exported)
			break;

		/*
		 * Importer just released buffer fd, check if there is any other importer still using it.
		 * If not and buffer was unexported, clean up shared data and remove that buffer.
		 */
		dev_dbg(hyper_dmabuf_private.device,
			"Buffer {id:%d key:%d %d %d} final released\n",
			sgt_info->hid.id, sgt_info->hid.rng_key[0], sgt_info->hid.rng_key[1],
			sgt_info->hid.rng_key[2]);

		if (!sgt_info->valid && !sgt_info->importer_exported &&
		    !sgt_info->unexport_scheduled) {
			hyper_dmabuf_cleanup_sgt_info(sgt_info, false);
			hyper_dmabuf_remove_exported(hid);
			kfree(sgt_info);
			/* store hyper_dmabuf_id in the list for reuse */
			store_reusable_hid(hid);
		}

		break;

	case HYPER_DMABUF_OPS_BEGIN_CPU_ACCESS:
		ret = dma_buf_begin_cpu_access(sgt_info->dma_buf, DMA_BIDIRECTIONAL);
		if (ret) {
			dev_err(hyper_dmabuf_private.device,
				"dmabuf remote sync::error while processing HYPER_DMABUF_OPS_BEGIN_CPU_ACCESS\n");
			return ret;
		}
		break;

	case HYPER_DMABUF_OPS_END_CPU_ACCESS:
		ret = dma_buf_end_cpu_access(sgt_info->dma_buf, DMA_BIDIRECTIONAL);
		if (ret) {
			dev_err(hyper_dmabuf_private.device,
				"dmabuf remote sync::error while processing HYPER_DMABUF_OPS_END_CPU_ACCESS\n");
			return ret;
		}
		break;

	case HYPER_DMABUF_OPS_KMAP_ATOMIC:
	case HYPER_DMABUF_OPS_KMAP:
		va_kmapl = kcalloc(1, sizeof(*va_kmapl), GFP_KERNEL);
		if (!va_kmapl) {
			dev_err(hyper_dmabuf_private.device,
				"dmabuf remote sync::error while processing HYPER_DMABUF_OPS_KMAP(_ATOMIC)\n");
			return -ENOMEM;
		}

		/* dummy kmapping of 1 page */
		if (ops == HYPER_DMABUF_OPS_KMAP_ATOMIC)
			va_kmapl->vaddr = dma_buf_kmap_atomic(sgt_info->dma_buf, 1);
		else
			va_kmapl->vaddr = dma_buf_kmap(sgt_info->dma_buf, 1);

		if (!va_kmapl->vaddr) {
			kfree(va_kmapl);
			dev_err(hyper_dmabuf_private.device,
				"dmabuf remote sync::error while processing HYPER_DMABUF_OPS_KMAP(_ATOMIC)\n");
			return -ENOMEM;
		}
		list_add(&va_kmapl->list, &sgt_info->va_kmapped->list);
		break;

	case HYPER_DMABUF_OPS_KUNMAP_ATOMIC:
	case HYPER_DMABUF_OPS_KUNMAP:
		if (list_empty(&sgt_info->va_kmapped->list)) {
			dev_err(hyper_dmabuf_private.device,
				"dmabuf remote sync::error while processing HYPER_DMABUF_OPS_KUNMAP(_ATOMIC)\n");
			dev_err(hyper_dmabuf_private.device,
				"no more dmabuf VA to be freed\n");
			return -EFAULT;
		}

		va_kmapl = list_first_entry(&sgt_info->va_kmapped->list,
					    struct kmap_vaddr_list, list);
		if (!va_kmapl->vaddr) {
			dev_err(hyper_dmabuf_private.device,
				"dmabuf remote sync::error while processing HYPER_DMABUF_OPS_KUNMAP(_ATOMIC)\n");
			return PTR_ERR(va_kmapl->vaddr);
		}

		/* unmapping 1 page */
		if (ops == HYPER_DMABUF_OPS_KUNMAP_ATOMIC)
			dma_buf_kunmap_atomic(sgt_info->dma_buf, 1, va_kmapl->vaddr);
		else
			dma_buf_kunmap(sgt_info->dma_buf, 1, va_kmapl->vaddr);

		list_del(&va_kmapl->list);
		kfree(va_kmapl);
		break;

	case HYPER_DMABUF_OPS_MMAP:
		/* currently not supported: looking for a way to create
		 * a dummy vma */
		dev_warn(hyper_dmabuf_private.device,
			 "dmabuf remote sync::sychronized mmap is not supported\n");
		break;

	case HYPER_DMABUF_OPS_VMAP:
		va_vmapl = kcalloc(1, sizeof(*va_vmapl), GFP_KERNEL);

		if (!va_vmapl) {
			dev_err(hyper_dmabuf_private.device,
				"dmabuf remote sync::error while processing HYPER_DMABUF_OPS_VMAP\n");
			return -ENOMEM;
		}

		/* dummy vmapping */
		va_vmapl->vaddr = dma_buf_vmap(sgt_info->dma_buf);

		if (!va_vmapl->vaddr) {
			kfree(va_vmapl);
			dev_err(hyper_dmabuf_private.device,
				"dmabuf remote sync::error while processing HYPER_DMABUF_OPS_VMAP\n");
			return -ENOMEM;
		}
		list_add(&va_vmapl->list, &sgt_info->va_vmapped->list);
		break;

	case HYPER_DMABUF_OPS_VUNMAP:
		if (list_empty(&sgt_info->va_vmapped->list)) {
			dev_err(hyper_dmabuf_private.device,
				"dmabuf remote sync::error while processing HYPER_DMABUF_OPS_VUNMAP\n");
			dev_err(hyper_dmabuf_private.device,
				"no more dmabuf VA to be freed\n");
			return -EFAULT;
		}
		va_vmapl = list_first_entry(&sgt_info->va_vmapped->list,
					struct vmap_vaddr_list, list);
		if (!va_vmapl || va_vmapl->vaddr == NULL) {
			dev_err(hyper_dmabuf_private.device,
				"dmabuf remote sync::error while processing HYPER_DMABUF_OPS_VUNMAP\n");
			return -EFAULT;
		}

		dma_buf_vunmap(sgt_info->dma_buf, va_vmapl->vaddr);

		list_del(&va_vmapl->list);
		kfree(va_vmapl);
		break;

	default:
		/* program should not get here */
		break;
	}

	return 0;
}
