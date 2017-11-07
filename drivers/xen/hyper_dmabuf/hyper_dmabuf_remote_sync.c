#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/dma-buf.h>
#include "hyper_dmabuf_struct.h"
#include "hyper_dmabuf_list.h"
#include "hyper_dmabuf_drv.h"
#include "hyper_dmabuf_id.h"
#include "xen/hyper_dmabuf_xen_comm.h"
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
 * are what is created when initial exporting is issued so it
 * should not be modified or released by this fuction.
 */
int hyper_dmabuf_remote_sync(int id, int ops)
{
	struct hyper_dmabuf_sgt_info *sgt_info;
	struct sgt_list *sgtl;
	struct attachment_list *attachl;
	struct kmap_vaddr_list *va_kmapl;
	struct vmap_vaddr_list *va_vmapl;
	int ret;

	/* find a coresponding SGT for the id */
	sgt_info = hyper_dmabuf_find_exported(id);

	if (!sgt_info) {
		printk("dmabuf remote sync::can't find exported list\n");
		return -EINVAL;
	}

	switch (ops) {
	case HYPER_DMABUF_OPS_ATTACH:
		attachl = kcalloc(1, sizeof(*attachl), GFP_KERNEL);

		attachl->attach = dma_buf_attach(sgt_info->dma_buf,
						 hyper_dmabuf_private.device);

		if (!attachl->attach) {
			kfree(attachl);
			printk("dmabuf remote sync::error while processing HYPER_DMABUF_OPS_ATTACH\n");
			return -EINVAL;
		}

		list_add(&attachl->list, &sgt_info->active_attached->list);
		break;

	case HYPER_DMABUF_OPS_DETACH:
		if (list_empty(&sgt_info->active_attached->list)) {
			printk("dmabuf remote sync::error while processing HYPER_DMABUF_OPS_DETACH\n");
			printk("no more dmabuf attachment left to be detached\n");
			return -EINVAL;
		}

		attachl = list_first_entry(&sgt_info->active_attached->list,
					   struct attachment_list, list);

		dma_buf_detach(sgt_info->dma_buf, attachl->attach);
		list_del(&attachl->list);
		kfree(attachl);
		break;

	case HYPER_DMABUF_OPS_MAP:
		if (list_empty(&sgt_info->active_attached->list)) {
			printk("dmabuf remote sync::error while processing HYPER_DMABUF_OPS_MAP\n");
			printk("no more dmabuf attachment left to be detached\n");
			return -EINVAL;
		}

		attachl = list_first_entry(&sgt_info->active_attached->list,
					   struct attachment_list, list);

		sgtl = kcalloc(1, sizeof(*sgtl), GFP_KERNEL);
		sgtl->sgt = dma_buf_map_attachment(attachl->attach, DMA_BIDIRECTIONAL);
		if (!sgtl->sgt) {
			kfree(sgtl);
			printk("dmabuf remote sync::error while processing HYPER_DMABUF_OPS_MAP\n");
			return -EINVAL;
		}
		list_add(&sgtl->list, &sgt_info->active_sgts->list);
		break;

	case HYPER_DMABUF_OPS_UNMAP:
		if (list_empty(&sgt_info->active_sgts->list) ||
		    list_empty(&sgt_info->active_attached->list)) {
			printk("dmabuf remote sync::error while processing HYPER_DMABUF_OPS_UNMAP\n");
			printk("no more SGT or attachment left to be freed\n");
			return -EINVAL;
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

	case HYPER_DMABUF_OPS_RELEASE_FINAL:
		/*
		 * Importer just released buffer fd, check if there is any other importer still using it.
		 * If not and buffer was unexported, clean up shared data and remove that buffer.
		 */
		 if (list_empty(&sgt_info->active_attached->list) &&
		     !sgt_info->valid) {
			hyper_dmabuf_cleanup_sgt_info(sgt_info, false);
			hyper_dmabuf_remove_exported(id);
			kfree(sgt_info);
			/* store hyper_dmabuf_id in the list for reuse */
			store_reusable_id(id);
		}

		break;

	case HYPER_DMABUF_OPS_RELEASE:
		/* place holder */
		break;

	case HYPER_DMABUF_OPS_BEGIN_CPU_ACCESS:
		ret = dma_buf_begin_cpu_access(sgt_info->dma_buf, DMA_BIDIRECTIONAL);
		if (!ret) {
			printk("dmabuf remote sync::error while processing HYPER_DMABUF_OPS_BEGIN_CPU_ACCESS\n");
			ret = -EINVAL;
		}
		break;

	case HYPER_DMABUF_OPS_END_CPU_ACCESS:
		ret = dma_buf_end_cpu_access(sgt_info->dma_buf, DMA_BIDIRECTIONAL);
		if (!ret) {
			printk("dmabuf remote sync::error while processing HYPER_DMABUF_OPS_END_CPU_ACCESS\n");
			ret = -EINVAL;
		}
		break;

	case HYPER_DMABUF_OPS_KMAP_ATOMIC:
	case HYPER_DMABUF_OPS_KMAP:
		va_kmapl = kcalloc(1, sizeof(*va_kmapl), GFP_KERNEL);

		/* dummy kmapping of 1 page */
		if (ops == HYPER_DMABUF_OPS_KMAP_ATOMIC)
			va_kmapl->vaddr = dma_buf_kmap_atomic(sgt_info->dma_buf, 1);
		else
			va_kmapl->vaddr = dma_buf_kmap(sgt_info->dma_buf, 1);

		if (!va_kmapl->vaddr) {
			kfree(va_kmapl);
			printk("dmabuf remote sync::error while processing HYPER_DMABUF_OPS_KMAP(_ATOMIC)\n");
			return -EINVAL;
		}
		list_add(&va_kmapl->list, &sgt_info->va_kmapped->list);
		break;

	case HYPER_DMABUF_OPS_KUNMAP_ATOMIC:
	case HYPER_DMABUF_OPS_KUNMAP:
		if (list_empty(&sgt_info->va_kmapped->list)) {
			printk("dmabuf remote sync::error while processing HYPER_DMABUF_OPS_KUNMAP(_ATOMIC)\n");
			printk("no more dmabuf VA to be freed\n");
			return -EINVAL;
		}

		va_kmapl = list_first_entry(&sgt_info->va_kmapped->list,
					    struct kmap_vaddr_list, list);
		if (va_kmapl->vaddr == NULL) {
			printk("dmabuf remote sync::error while processing HYPER_DMABUF_OPS_KUNMAP(_ATOMIC)\n");
			return -EINVAL;
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
		printk("dmabuf remote sync::sychronized mmap is not supported\n");
		break;

	case HYPER_DMABUF_OPS_VMAP:
		va_vmapl = kcalloc(1, sizeof(*va_vmapl), GFP_KERNEL);

		/* dummy vmapping */
		va_vmapl->vaddr = dma_buf_vmap(sgt_info->dma_buf);

		if (!va_vmapl->vaddr) {
			kfree(va_vmapl);
			printk("dmabuf remote sync::error while processing HYPER_DMABUF_OPS_VMAP\n");
			return -EINVAL;
		}
		list_add(&va_vmapl->list, &sgt_info->va_vmapped->list);
		break;

	case HYPER_DMABUF_OPS_VUNMAP:
		if (list_empty(&sgt_info->va_vmapped->list)) {
			printk("dmabuf remote sync::error while processing HYPER_DMABUF_OPS_VUNMAP\n");
			printk("no more dmabuf VA to be freed\n");
			return -EINVAL;
		}
		va_vmapl = list_first_entry(&sgt_info->va_vmapped->list,
					struct vmap_vaddr_list, list);
		if (!va_vmapl || va_vmapl->vaddr == NULL) {
			printk("dmabuf remote sync::error while processing HYPER_DMABUF_OPS_VUNMAP\n");
			return -EINVAL;
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
