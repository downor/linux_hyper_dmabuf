#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/dma-buf.h>
#include <linux/delay.h>
#include <linux/list.h>
#include "hyper_dmabuf_struct.h"
#include "hyper_dmabuf_ioctl.h"
#include "hyper_dmabuf_list.h"
#include "hyper_dmabuf_msg.h"
#include "hyper_dmabuf_drv.h"
#include "hyper_dmabuf_id.h"
#include "hyper_dmabuf_imp.h"
#include "hyper_dmabuf_query.h"

extern struct hyper_dmabuf_private hyper_dmabuf_private;

static int hyper_dmabuf_tx_ch_setup(void *data)
{
	struct ioctl_hyper_dmabuf_tx_ch_setup *tx_ch_attr;
	struct hyper_dmabuf_backend_ops *ops = hyper_dmabuf_private.backend_ops;
	int ret = 0;

	if (!data) {
		dev_err(hyper_dmabuf_private.device, "user data is NULL\n");
		return -1;
	}
	tx_ch_attr = (struct ioctl_hyper_dmabuf_tx_ch_setup *)data;

	ret = ops->init_tx_ch(tx_ch_attr->remote_domain);

	return ret;
}

static int hyper_dmabuf_rx_ch_setup(void *data)
{
	struct ioctl_hyper_dmabuf_rx_ch_setup *rx_ch_attr;
	struct hyper_dmabuf_backend_ops *ops = hyper_dmabuf_private.backend_ops;
	int ret = 0;

	if (!data) {
		dev_err(hyper_dmabuf_private.device, "user data is NULL\n");
		return -1;
	}

	rx_ch_attr = (struct ioctl_hyper_dmabuf_rx_ch_setup *)data;

	ret = ops->init_rx_ch(rx_ch_attr->source_domain);

	return ret;
}

static int hyper_dmabuf_export_remote(void *data)
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
	int ret = 0;

	if (!data) {
		dev_err(hyper_dmabuf_private.device, "user data is NULL\n");
		return -1;
	}

	export_remote_attr = (struct ioctl_hyper_dmabuf_export_remote *)data;

	dma_buf = dma_buf_get(export_remote_attr->dmabuf_fd);

	if (!dma_buf) {
		dev_err(hyper_dmabuf_private.device,  "Cannot get dma buf\n");
		return -1;
	}

	/* we check if this specific attachment was already exported
	 * to the same domain and if yes and it's valid sgt_info,
	 * it returns hyper_dmabuf_id of pre-exported sgt_info
	 */
	ret = hyper_dmabuf_find_id_exported(dma_buf, export_remote_attr->remote_domain);
	sgt_info = hyper_dmabuf_find_exported(ret);
	if (ret != -1 && sgt_info->valid) {
		dma_buf_put(dma_buf);
		export_remote_attr->hyper_dmabuf_id = ret;
		return 0;
	}

	attachment = dma_buf_attach(dma_buf, hyper_dmabuf_private.device);
	if (!attachment) {
		dev_err(hyper_dmabuf_private.device, "Cannot get attachment\n");
		return -1;
	}

	/* Clear ret, as that will cause whole ioctl to return failure
	 * to userspace, which is not true
	 */
	ret = 0;

	sgt = dma_buf_map_attachment(attachment, DMA_BIDIRECTIONAL);

	sgt_info = kmalloc(sizeof(*sgt_info), GFP_KERNEL);

	sgt_info->hyper_dmabuf_id = hyper_dmabuf_get_id();

	/* TODO: We might need to consider using port number on event channel? */
	sgt_info->hyper_dmabuf_rdomain = export_remote_attr->remote_domain;
	sgt_info->dma_buf = dma_buf;
	sgt_info->valid = 1;
	sgt_info->importer_exported = 0;

	sgt_info->active_sgts = kmalloc(sizeof(struct sgt_list), GFP_KERNEL);
	sgt_info->active_attached = kmalloc(sizeof(struct attachment_list), GFP_KERNEL);
	sgt_info->va_kmapped = kmalloc(sizeof(struct kmap_vaddr_list), GFP_KERNEL);
	sgt_info->va_vmapped = kmalloc(sizeof(struct vmap_vaddr_list), GFP_KERNEL);

	sgt_info->active_sgts->sgt = sgt;
	sgt_info->active_attached->attach = attachment;
	sgt_info->va_kmapped->vaddr = NULL; /* first vaddr is NULL */
	sgt_info->va_vmapped->vaddr = NULL; /* first vaddr is NULL */

	/* initialize list of sgt, attachment and vaddr for dmabuf sync
	 * via shadow dma-buf
	 */
	INIT_LIST_HEAD(&sgt_info->active_sgts->list);
	INIT_LIST_HEAD(&sgt_info->active_attached->list);
	INIT_LIST_HEAD(&sgt_info->va_kmapped->list);
	INIT_LIST_HEAD(&sgt_info->va_vmapped->list);

	page_info = hyper_dmabuf_ext_pgs(sgt);
	if (page_info == NULL)
		goto fail_export;

	sgt_info->nents = page_info->nents;

	/* now register it to export list */
	hyper_dmabuf_register_exported(sgt_info);

	page_info->hyper_dmabuf_rdomain = sgt_info->hyper_dmabuf_rdomain;
	page_info->hyper_dmabuf_id = sgt_info->hyper_dmabuf_id; /* may not be needed */

	export_remote_attr->hyper_dmabuf_id = sgt_info->hyper_dmabuf_id;

	/* now create request for importer via ring */
	operands[0] = page_info->hyper_dmabuf_id;
	operands[1] = page_info->nents;
	operands[2] = page_info->frst_ofst;
	operands[3] = page_info->last_len;
	operands[4] = ops->share_pages (page_info->pages, export_remote_attr->remote_domain,
					page_info->nents, &sgt_info->refs_info);

	/* driver/application specific private info, max 32 bytes */
	operands[5] = export_remote_attr->private[0];
	operands[6] = export_remote_attr->private[1];
	operands[7] = export_remote_attr->private[2];
	operands[8] = export_remote_attr->private[3];

	req = kcalloc(1, sizeof(*req), GFP_KERNEL);

	/* composing a message to the importer */
	hyper_dmabuf_create_request(req, HYPER_DMABUF_EXPORT, &operands[0]);

	if(ops->send_req(export_remote_attr->remote_domain, req, false))
		goto fail_send_request;

	/* free msg */
	kfree(req);
	/* free page_info */
	kfree(page_info->pages);
	kfree(page_info);

	return ret;

fail_send_request:
	kfree(req);
	hyper_dmabuf_remove_exported(sgt_info->hyper_dmabuf_id);

fail_export:
	dma_buf_unmap_attachment(sgt_info->active_attached->attach,
				 sgt_info->active_sgts->sgt,
				 DMA_BIDIRECTIONAL);
	dma_buf_detach(sgt_info->dma_buf, sgt_info->active_attached->attach);
	dma_buf_put(sgt_info->dma_buf);

	kfree(sgt_info->active_attached);
	kfree(sgt_info->active_sgts);
	kfree(sgt_info->va_kmapped);
	kfree(sgt_info->va_vmapped);

	return -EINVAL;
}

static int hyper_dmabuf_export_fd_ioctl(void *data)
{
	struct ioctl_hyper_dmabuf_export_fd *export_fd_attr;
	struct hyper_dmabuf_backend_ops *ops = hyper_dmabuf_private.backend_ops;
	struct hyper_dmabuf_imported_sgt_info *sgt_info;
	struct hyper_dmabuf_req *req;
	struct page **data_pages;
	int operand;
	int ret = 0;

	dev_dbg(hyper_dmabuf_private.device, "%s entry\n", __func__);

	if (!data) {
		dev_err(hyper_dmabuf_private.device, "user data is NULL\n");
		return -EINVAL;
	}

	export_fd_attr = (struct ioctl_hyper_dmabuf_export_fd *)data;

	/* look for dmabuf for the id */
	sgt_info = hyper_dmabuf_find_imported(export_fd_attr->hyper_dmabuf_id);
	if (sgt_info == NULL) /* can't find sgt from the table */
		return -1;

	dev_dbg(hyper_dmabuf_private.device,
		  "%s Found buffer gref %d  off %d last len %d nents %d domain %d\n", __func__,
		  sgt_info->ref_handle, sgt_info->frst_ofst,
		  sgt_info->last_len, sgt_info->nents,
		  HYPER_DMABUF_DOM_ID(sgt_info->hyper_dmabuf_id));

	if (!sgt_info->sgt) {
		dev_dbg(hyper_dmabuf_private.device,
			"%s buffer %d pages not mapped yet\n", __func__,sgt_info->hyper_dmabuf_id);
		data_pages = ops->map_shared_pages(sgt_info->ref_handle,
						   HYPER_DMABUF_DOM_ID(sgt_info->hyper_dmabuf_id),
						   sgt_info->nents,
						   &sgt_info->refs_info);

		sgt_info->sgt = hyper_dmabuf_create_sgt(data_pages, sgt_info->frst_ofst,
							sgt_info->last_len, sgt_info->nents);

	}

	/* send notification for export_fd to exporter */
	operand = sgt_info->hyper_dmabuf_id;

	req = kcalloc(1, sizeof(*req), GFP_KERNEL);
	hyper_dmabuf_create_request(req, HYPER_DMABUF_FIRST_EXPORT, &operand);

	ret = ops->send_req(HYPER_DMABUF_DOM_ID(operand), req, false);

	if (!sgt_info->sgt || ret) {
		kfree(req);
		dev_err(hyper_dmabuf_private.device, "Failed to create sgt or notify exporter\n");
		return -EINVAL;
	}
	kfree(req);

	export_fd_attr->fd = hyper_dmabuf_export_fd(sgt_info, export_fd_attr->flags);

	if (export_fd_attr->fd < 0) {
		/* fail to get fd */
		ret = export_fd_attr->fd;
	} else {
		sgt_info->num_importers++;
	}

	dev_dbg(hyper_dmabuf_private.device, "%s entry\n", __func__);
	return ret;
}

/* unexport dmabuf from the database and send int req to the source domain
 * to unmap it.
 */
static int hyper_dmabuf_unexport(void *data)
{
	struct ioctl_hyper_dmabuf_unexport *unexport_attr;
	struct hyper_dmabuf_backend_ops *ops = hyper_dmabuf_private.backend_ops;
	struct hyper_dmabuf_sgt_info *sgt_info;
	struct hyper_dmabuf_req *req;
	int ret;

	dev_dbg(hyper_dmabuf_private.device, "%s entry\n", __func__);

	if (!data) {
		dev_err(hyper_dmabuf_private.device, "user data is NULL\n");
		return -EINVAL;
	}

	unexport_attr = (struct ioctl_hyper_dmabuf_unexport *)data;

	/* find dmabuf in export list */
	sgt_info = hyper_dmabuf_find_exported(unexport_attr->hyper_dmabuf_id);

	/* failed to find corresponding entry in export list */
	if (sgt_info == NULL) {
		unexport_attr->status = -EINVAL;
		return -EFAULT;
	}

	req = kcalloc(1, sizeof(*req), GFP_KERNEL);

	hyper_dmabuf_create_request(req, HYPER_DMABUF_NOTIFY_UNEXPORT, &unexport_attr->hyper_dmabuf_id);

	/* Now send unexport request to remote domain, marking that buffer should not be used anymore */
	ret = ops->send_req(sgt_info->hyper_dmabuf_rdomain, req, true);
	if (ret < 0) {
		kfree(req);
		return -EFAULT;
	}

	/* free msg */
	kfree(req);

	dev_dbg(hyper_dmabuf_private.device,
		"Marking buffer %d as invalid\n", unexport_attr->hyper_dmabuf_id);
	/* no longer valid */
	sgt_info->valid = 0;

	/*
	 * Immediately clean-up if it has never been exported by importer
	 * (so no SGT is constructed on importer).
	 * clean it up later in remote sync when final release ops
	 * is called (importer does this only when there's no
	 * no consumer of locally exported FDs)
	 */
	if (!sgt_info->importer_exported) {
		dev_dbg(hyper_dmabuf_private.device,
			"claning up buffer %d completly\n", unexport_attr->hyper_dmabuf_id);
		hyper_dmabuf_cleanup_sgt_info(sgt_info, false);
		hyper_dmabuf_remove_exported(unexport_attr->hyper_dmabuf_id);
		kfree(sgt_info);
		/* register hyper_dmabuf_id to the list for reuse */
		store_reusable_id(unexport_attr->hyper_dmabuf_id);
	}

	dev_dbg(hyper_dmabuf_private.device, "%s entry\n", __func__);
	return ret;
}

static int hyper_dmabuf_query(void *data)
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

	sgt_info = hyper_dmabuf_find_exported(query_attr->hyper_dmabuf_id);
	imported_sgt_info = hyper_dmabuf_find_imported(query_attr->hyper_dmabuf_id);

	/* if dmabuf can't be found in both lists, return */
	if (!(sgt_info && imported_sgt_info)) {
		dev_err(hyper_dmabuf_private.device, "can't find entry anywhere\n");
		return -EINVAL;
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
				query_attr->info = (HYPER_DMABUF_DOM_ID(imported_sgt_info->hyper_dmabuf_id));
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
	int ret = -EINVAL;
	hyper_dmabuf_ioctl_t func;
	char *kdata;

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

	ret = func(kdata);

	if (copy_to_user((void __user *)param, kdata, _IOC_SIZE(cmd)) != 0) {
		dev_err(hyper_dmabuf_private.device, "failed to copy to user arguments\n");
		return -EFAULT;
	}

	kfree(kdata);

	return ret;
}

struct device_info {
	int curr_domain;
};

/*===============================================================================================*/
static struct file_operations hyper_dmabuf_driver_fops =
{
   .owner = THIS_MODULE,
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
	int result = 0;

	result = misc_register(&hyper_dmabuf_miscdev);

	if (result != 0) {
		printk(KERN_WARNING "hyper_dmabuf: driver can't be registered\n");
		return result;
	}

	hyper_dmabuf_private.device = hyper_dmabuf_miscdev.this_device;

	/* TODO: Check if there is a different way to initialize dma mask nicely */
	dma_coerce_mask_and_coherent(hyper_dmabuf_private.device, 0xFFFFFFFF);

	/* TODO find a way to provide parameters for below function or move that to ioctl */
/*	err = bind_interdomain_evtchn_to_irqhandler(rdomain, evtchn,
				src_sink_isr, PORT_NUM, "remote_domain", &info);
	if (err < 0) {
		printk("hyper_dmabuf: can't register interrupt handlers\n");
		return -EFAULT;
	}

	info.irq = err;
*/
	return result;
}

/*-----------------------------------------------------------------------------------------------*/
void unregister_device(void)
{
	printk( KERN_NOTICE "hyper_dmabuf: unregister_device() is called" );
	misc_deregister(&hyper_dmabuf_miscdev);
}
