#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/dma-buf.h>
#include <linux/delay.h>
#include "hyper_dmabuf_struct.h"
#include "hyper_dmabuf_imp.h"
#include "hyper_dmabuf_list.h"
#include "hyper_dmabuf_drv.h"
#include "hyper_dmabuf_query.h"
#include "xen/hyper_dmabuf_xen_comm.h"
#include "xen/hyper_dmabuf_xen_comm_list.h"
#include "hyper_dmabuf_msg.h"

struct hyper_dmabuf_private {
	struct device *device;
} hyper_dmabuf_private;

static uint32_t hyper_dmabuf_id_gen(void) {
	/* TODO: add proper implementation */
	static uint32_t id = 0;
	static int32_t domid = -1;
	if (domid == -1) {
		domid = hyper_dmabuf_get_domid();
	}
	return HYPER_DMABUF_ID_IMPORTER(domid, id++);
}

static int hyper_dmabuf_exporter_ring_setup(void *data)
{
	struct ioctl_hyper_dmabuf_exporter_ring_setup *ring_attr;
	struct hyper_dmabuf_ring_info_export *ring_info;
	int ret = 0;

	if (!data) {
		printk("user data is NULL\n");
		return -1;
	}
	ring_attr = (struct ioctl_hyper_dmabuf_exporter_ring_setup *)data;

	/* check if the ring ch already exists */
	ring_info = hyper_dmabuf_find_exporter_ring(ring_attr->remote_domain);

	if (ring_info) {
		printk("(exporter's) ring ch to domid = %d already exist\ngref = %d, port = %d\n",
			ring_info->rdomain, ring_info->gref_ring, ring_info->port);
		return 0;
	}

	ret = hyper_dmabuf_exporter_ringbuf_init(ring_attr->remote_domain,
						&ring_attr->ring_refid,
						&ring_attr->port);

	return ret;
}

static int hyper_dmabuf_importer_ring_setup(void *data)
{
	struct ioctl_hyper_dmabuf_importer_ring_setup *setup_imp_ring_attr;
	struct hyper_dmabuf_ring_info_import *ring_info;
	int ret = 0;

	if (!data) {
		printk("user data is NULL\n");
		return -1;
	}

	setup_imp_ring_attr = (struct ioctl_hyper_dmabuf_importer_ring_setup *)data;

	/* check if the ring ch already exist */
	ring_info = hyper_dmabuf_find_importer_ring(setup_imp_ring_attr->source_domain);

	if (ring_info) {
		printk("(importer's) ring ch to domid = %d already exist\n", ring_info->sdomain);
		return 0;
	}

	/* user need to provide a port number and ref # for the page used as ring buffer */
	ret = hyper_dmabuf_importer_ringbuf_init(setup_imp_ring_attr->source_domain,
						 setup_imp_ring_attr->ring_refid,
						 setup_imp_ring_attr->port);

	return ret;
}

static int hyper_dmabuf_export_remote(void *data)
{
	struct ioctl_hyper_dmabuf_export_remote *export_remote_attr;
	struct dma_buf *dma_buf;
	struct dma_buf_attachment *attachment;
	struct sg_table *sgt;
	struct hyper_dmabuf_pages_info *page_info;
	struct hyper_dmabuf_sgt_info *sgt_info;
	struct hyper_dmabuf_ring_rq *req;
	int operands[9];
	int ret = 0;

	if (!data) {
		printk("user data is NULL\n");
		return -1;
	}

	export_remote_attr = (struct ioctl_hyper_dmabuf_export_remote *)data;

	dma_buf = dma_buf_get(export_remote_attr->dmabuf_fd);

	if (!dma_buf) {
		printk("Cannot get dma buf\n");
		return -1;
	}

	/* we check if this specific attachment was already exported
	 * to the same domain and if yes, it returns hyper_dmabuf_id
	 * of pre-exported sgt */
	ret = hyper_dmabuf_find_id(dma_buf, export_remote_attr->remote_domain);
	if (ret != -1) {
		dma_buf_put(dma_buf);
		export_remote_attr->hyper_dmabuf_id = ret;
		return 0;
	}

	attachment = dma_buf_attach(dma_buf, hyper_dmabuf_private.device);
	if (!attachment) {
		printk("Cannot get attachment\n");
		return -1;
	}

	/* Clear ret, as that will cause whole ioctl to return failure to userspace, which is not true */
	ret = 0;

	sgt = dma_buf_map_attachment(attachment, DMA_BIDIRECTIONAL);

	sgt_info = kmalloc(sizeof(*sgt_info), GFP_KERNEL);

	sgt_info->hyper_dmabuf_id = hyper_dmabuf_id_gen();
	/* TODO: We might need to consider using port number on event channel? */
	sgt_info->hyper_dmabuf_rdomain = export_remote_attr->remote_domain;
	sgt_info->sgt = sgt;
	sgt_info->attachment = attachment;
	sgt_info->dma_buf = dma_buf;

	page_info = hyper_dmabuf_ext_pgs(sgt);
	if (page_info == NULL)
		goto fail_export;

	/* now register it to export list */
	hyper_dmabuf_register_exported(sgt_info);

	page_info->hyper_dmabuf_rdomain = sgt_info->hyper_dmabuf_rdomain;
	page_info->hyper_dmabuf_id = sgt_info->hyper_dmabuf_id; /* may not be needed */

	export_remote_attr->hyper_dmabuf_id = sgt_info->hyper_dmabuf_id;

	/* now create table of grefs for shared pages and */

	/* now create request for importer via ring */
	operands[0] = page_info->hyper_dmabuf_id;
	operands[1] = page_info->nents;
	operands[2] = page_info->frst_ofst;
	operands[3] = page_info->last_len;
	operands[4] = hyper_dmabuf_create_gref_table(page_info->pages, export_remote_attr->remote_domain,
						page_info->nents, &sgt_info->shared_pages_info);
	/* driver/application specific private info, max 32 bytes */
	operands[5] = export_remote_attr->private[0];
	operands[6] = export_remote_attr->private[1];
	operands[7] = export_remote_attr->private[2];
	operands[8] = export_remote_attr->private[3];

	req = kcalloc(1, sizeof(*req), GFP_KERNEL);

	/* composing a message to the importer */
	hyper_dmabuf_create_request(req, HYPER_DMABUF_EXPORT, &operands[0]);
	if(hyper_dmabuf_send_request(export_remote_attr->remote_domain, req))
		goto fail_send_request;

	/* free msg */
	kfree(req);
	/* free page_info */
	kfree(page_info);

	return ret;

fail_send_request:
	kfree(req);
	hyper_dmabuf_remove_exported(sgt_info->hyper_dmabuf_id);

fail_export:
	dma_buf_unmap_attachment(sgt_info->attachment, sgt_info->sgt, DMA_BIDIRECTIONAL);
	dma_buf_detach(sgt_info->dma_buf, sgt_info->attachment);
	dma_buf_put(sgt_info->dma_buf);

	return -EINVAL;
}

static int hyper_dmabuf_export_fd_ioctl(void *data)
{
	struct ioctl_hyper_dmabuf_export_fd *export_fd_attr;
	struct hyper_dmabuf_imported_sgt_info *imported_sgt_info;
	int ret = 0;

	if (!data) {
		printk("user data is NULL\n");
		return -1;
	}

	export_fd_attr = (struct ioctl_hyper_dmabuf_export_fd *)data;

	/* look for dmabuf for the id */
	imported_sgt_info = hyper_dmabuf_find_imported(export_fd_attr->hyper_dmabuf_id);
	if (imported_sgt_info == NULL) /* can't find sgt from the table */
		return -1;

	printk("%s Found buffer gref %d  off %d last len %d nents %d domain %d\n", __func__,
		imported_sgt_info->gref, imported_sgt_info->frst_ofst,
		imported_sgt_info->last_len, imported_sgt_info->nents,
		HYPER_DMABUF_ID_IMPORTER_GET_SDOMAIN_ID(imported_sgt_info->hyper_dmabuf_id));

	imported_sgt_info->sgt = hyper_dmabuf_map_pages(imported_sgt_info->gref,
						imported_sgt_info->frst_ofst,
						imported_sgt_info->last_len,
						imported_sgt_info->nents,
						HYPER_DMABUF_ID_IMPORTER_GET_SDOMAIN_ID(imported_sgt_info->hyper_dmabuf_id),
						&imported_sgt_info->shared_pages_info);

	if (!imported_sgt_info->sgt) {
		return -1;
	}

	export_fd_attr->fd = hyper_dmabuf_export_fd(imported_sgt_info, export_fd_attr->flags);
	if (export_fd_attr < 0) {
		ret = export_fd_attr->fd;
	}

	return ret;
}

/* removing dmabuf from the database and send int req to the source domain
* to unmap it. */
static int hyper_dmabuf_destroy(void *data)
{
	struct ioctl_hyper_dmabuf_destroy *destroy_attr;
	struct hyper_dmabuf_sgt_info *sgt_info;
	struct hyper_dmabuf_ring_rq *req;
	int ret;

	if (!data) {
		printk("user data is NULL\n");
		return -EINVAL;
	}

	destroy_attr = (struct ioctl_hyper_dmabuf_destroy *)data;

	/* find dmabuf in export list */
	sgt_info = hyper_dmabuf_find_exported(destroy_attr->hyper_dmabuf_id);
	if (sgt_info == NULL) { /* failed to find corresponding entry in export list */
		destroy_attr->status = -EINVAL;
		return -EFAULT;
	}

	req = kcalloc(1, sizeof(*req), GFP_KERNEL);

	hyper_dmabuf_create_request(req, HYPER_DMABUF_DESTROY, &destroy_attr->hyper_dmabuf_id);

	/* now send destroy request to remote domain
	 * currently assuming there's only one importer exist */
	ret = hyper_dmabuf_send_request(sgt_info->hyper_dmabuf_rdomain, req);
	if (ret < 0) {
		kfree(req);
		return -EFAULT;
	}

	/* free msg */
	kfree(req);
	destroy_attr->status = ret;

	/* Rest of cleanup will follow when importer will free it's buffer,
	 * current implementation assumes that there is only one importer
         */

	return ret;
}

static int hyper_dmabuf_query(void *data)
{
	struct ioctl_hyper_dmabuf_query *query_attr;
	struct hyper_dmabuf_sgt_info *sgt_info;
	struct hyper_dmabuf_imported_sgt_info *imported_sgt_info;
	int ret = 0;

	if (!data) {
		printk("user data is NULL\n");
		return -EINVAL;
	}

	query_attr = (struct ioctl_hyper_dmabuf_query *)data;

	sgt_info = hyper_dmabuf_find_exported(query_attr->hyper_dmabuf_id);
	imported_sgt_info = hyper_dmabuf_find_imported(query_attr->hyper_dmabuf_id);

	/* if dmabuf can't be found in both lists, return */
	if (!(sgt_info && imported_sgt_info)) {
		printk("can't find entry anywhere\n");
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
				query_attr->info = (HYPER_DMABUF_ID_IMPORTER_GET_SDOMAIN_ID(imported_sgt_info->hyper_dmabuf_id));
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

static int hyper_dmabuf_remote_exporter_ring_setup(void *data)
{
	struct ioctl_hyper_dmabuf_remote_exporter_ring_setup *remote_exporter_ring_setup;
	struct hyper_dmabuf_ring_rq *req;

	remote_exporter_ring_setup = (struct ioctl_hyper_dmabuf_remote_exporter_ring_setup *)data;

	req = kcalloc(1, sizeof(*req), GFP_KERNEL);
	hyper_dmabuf_create_request(req, HYPER_DMABUF_EXPORTER_RING_SETUP, NULL);

	/* requesting remote domain to set-up exporter's ring */
	if(hyper_dmabuf_send_request(remote_exporter_ring_setup->rdomain, req) < 0) {
		kfree(req);
		return -EINVAL;
	}

	kfree(req);
	return 0;
}

static const struct hyper_dmabuf_ioctl_desc hyper_dmabuf_ioctls[] = {
	HYPER_DMABUF_IOCTL_DEF(IOCTL_HYPER_DMABUF_EXPORTER_RING_SETUP, hyper_dmabuf_exporter_ring_setup, 0),
	HYPER_DMABUF_IOCTL_DEF(IOCTL_HYPER_DMABUF_IMPORTER_RING_SETUP, hyper_dmabuf_importer_ring_setup, 0),
	HYPER_DMABUF_IOCTL_DEF(IOCTL_HYPER_DMABUF_EXPORT_REMOTE, hyper_dmabuf_export_remote, 0),
	HYPER_DMABUF_IOCTL_DEF(IOCTL_HYPER_DMABUF_EXPORT_FD, hyper_dmabuf_export_fd_ioctl, 0),
	HYPER_DMABUF_IOCTL_DEF(IOCTL_HYPER_DMABUF_DESTROY, hyper_dmabuf_destroy, 0),
	HYPER_DMABUF_IOCTL_DEF(IOCTL_HYPER_DMABUF_QUERY, hyper_dmabuf_query, 0),
	HYPER_DMABUF_IOCTL_DEF(IOCTL_HYPER_DMABUF_REMOTE_EXPORTER_RING_SETUP, hyper_dmabuf_remote_exporter_ring_setup, 0),
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
		printk("no function\n");
		return -EINVAL;
	}

	kdata = kmalloc(_IOC_SIZE(cmd), GFP_KERNEL);
	if (!kdata) {
		printk("no memory\n");
		return -ENOMEM;
	}

	if (copy_from_user(kdata, (void __user *)param, _IOC_SIZE(cmd)) != 0) {
		printk("failed to copy from user arguments\n");
		return -EFAULT;
	}

	ret = func(kdata);

	if (copy_to_user((void __user *)param, kdata, _IOC_SIZE(cmd)) != 0) {
		printk("failed to copy to user arguments\n");
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
