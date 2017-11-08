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

#include <linux/init.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/poll.h>
#include <linux/dma-buf.h>
#include "hyper_dmabuf_drv.h"
#include "hyper_dmabuf_conf.h"
#include "hyper_dmabuf_ioctl.h"
#include "hyper_dmabuf_msg.h"
#include "hyper_dmabuf_list.h"
#include "hyper_dmabuf_id.h"
#include "hyper_dmabuf_event.h"

#ifdef CONFIG_HYPER_DMABUF_XEN
#include "xen/hyper_dmabuf_xen_drv.h"
extern struct hyper_dmabuf_backend_ops xen_backend_ops;
#endif

MODULE_LICENSE("GPL and additional rights");
MODULE_AUTHOR("Intel Corporation");

struct hyper_dmabuf_private hyper_dmabuf_private;

long hyper_dmabuf_ioctl(struct file *filp,
			unsigned int cmd, unsigned long param);

void hyper_dmabuf_emergency_release(struct hyper_dmabuf_sgt_info* sgt_info,
				    void *attr);

int hyper_dmabuf_open(struct inode *inode, struct file *filp)
{
	int ret = 0;

	/* Do not allow exclusive open */
	if (filp->f_flags & O_EXCL)
		return -EBUSY;

	/*
	 * Initialize backend if needed,
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

int hyper_dmabuf_release(struct inode *inode, struct file *filp)
{
	hyper_dmabuf_foreach_exported(hyper_dmabuf_emergency_release, filp);

	/* clean up event queue */
	hyper_dmabuf_events_release();

	return 0;
}

unsigned int hyper_dmabuf_event_poll(struct file *filp, struct poll_table_struct *wait)
{
	unsigned int mask = 0;

	poll_wait(filp, &hyper_dmabuf_private.event_wait, wait);

	if (!list_empty(&hyper_dmabuf_private.event_list))
		mask |= POLLIN | POLLRDNORM;

	return mask;
}

ssize_t hyper_dmabuf_event_read(struct file *filp, char __user *buffer,
		size_t count, loff_t *offset)
{
	int ret;

	/* only root can read events */
	if (!capable(CAP_DAC_OVERRIDE))
		return -EFAULT;

	/* make sure user buffer can be written */
	if (!access_ok(VERIFY_WRITE, buffer, count))
		return -EFAULT;

	ret = mutex_lock_interruptible(&hyper_dmabuf_private.event_read_lock);
	if (ret)
		return ret;

	while (1) {
		struct hyper_dmabuf_event *e = NULL;

		spin_lock_irq(&hyper_dmabuf_private.event_lock);
		if (!list_empty(&hyper_dmabuf_private.event_list)) {
			e = list_first_entry(&hyper_dmabuf_private.event_list,
					struct hyper_dmabuf_event, link);
			list_del(&e->link);
		}
		spin_unlock_irq(&hyper_dmabuf_private.event_lock);

		if (!e) {
			if (ret)
				break;
			if (filp->f_flags & O_NONBLOCK) {
				ret = -EAGAIN;
				break;
			}

			mutex_unlock(&hyper_dmabuf_private.event_read_lock);
			ret = wait_event_interruptible(hyper_dmabuf_private.event_wait,
						       !list_empty(&hyper_dmabuf_private.event_list));

			if (ret >= 0)
				ret = mutex_lock_interruptible(&hyper_dmabuf_private.event_read_lock);

			if (ret)
				return ret;
		} else {
			unsigned length = (sizeof(struct hyper_dmabuf_event_hdr) + e->event_data.hdr.size);

			if (length > count - ret) {
put_back_event:
				spin_lock_irq(&hyper_dmabuf_private.event_lock);
				list_add(&e->link, &hyper_dmabuf_private.event_list);
				spin_unlock_irq(&hyper_dmabuf_private.event_lock);
				break;
			}

			if (copy_to_user(buffer + ret, &e->event_data.hdr,
					 sizeof(struct hyper_dmabuf_event_hdr))) {
				if (ret == 0)
					ret = -EFAULT;

				goto put_back_event;
			}

			ret += sizeof(struct hyper_dmabuf_event_hdr);

			if (copy_to_user(buffer + ret, e->event_data.data, e->event_data.hdr.size)) {
				/* error while copying void *data */

				struct hyper_dmabuf_event_hdr dummy_hdr = {0};
				ret -= sizeof(struct hyper_dmabuf_event_hdr);

				/* nullifying hdr of the event in user buffer */
				copy_to_user(buffer + ret, &dummy_hdr,
					     sizeof(dummy_hdr));

				ret = -EFAULT;

				goto put_back_event;
			}

			ret += e->event_data.hdr.size;
			kfree(e);
		}
	}

	mutex_unlock(&hyper_dmabuf_private.event_read_lock);

	return 0;
}

static struct file_operations hyper_dmabuf_driver_fops =
{
	.owner = THIS_MODULE,
	.open = hyper_dmabuf_open,
	.release = hyper_dmabuf_release,
	.read = hyper_dmabuf_event_read,
	.poll = hyper_dmabuf_event_poll,
	.unlocked_ioctl = hyper_dmabuf_ioctl,
};

static struct miscdevice hyper_dmabuf_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "xen/hyper_dmabuf",
	.fops = &hyper_dmabuf_driver_fops,
};

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

void unregister_device(void)
{
	dev_info(hyper_dmabuf_private.device,
		"hyper_dmabuf: unregister_device() is called\n");

	misc_deregister(&hyper_dmabuf_miscdev);
}

static int __init hyper_dmabuf_drv_init(void)
{
	int ret = 0;

	printk( KERN_NOTICE "hyper_dmabuf_starting: Initialization started\n");

	mutex_init(&hyper_dmabuf_private.lock);

	ret = register_device();
	if (ret < 0) {
		return ret;
	}

#ifdef CONFIG_HYPER_DMABUF_XEN
	hyper_dmabuf_private.backend_ops = &xen_backend_ops;
#endif
	/*
	 * Defer backend setup to first open call.
	 * Due to fact that some hypervisors eg. Xen, may have dependencies
	 * to userspace daemons like xenstored, in that case all xenstore
	 * calls done from kernel will block until that deamon will be
	 * started, in case where module is built in that will block entire
	 * kernel initialization.
	 */
	hyper_dmabuf_private.backend_initialized = false;

	dev_info(hyper_dmabuf_private.device,
		 "initializing database for imported/exported dmabufs\n");

	/* device structure initialization */
	/* currently only does work-queue initialization */
	hyper_dmabuf_private.work_queue = create_workqueue("hyper_dmabuf_wqueue");

	ret = hyper_dmabuf_table_init();
	if (ret < 0) {
		dev_err(hyper_dmabuf_private.device,
			"failed to initialize table for exported/imported entries\n");
		return ret;
	}

#ifdef CONFIG_HYPER_DMABUF_SYSFS
	ret = hyper_dmabuf_register_sysfs(hyper_dmabuf_private.device);
	if (ret < 0) {
		dev_err(hyper_dmabuf_private.device,
			"failed to initialize sysfs\n");
		return ret;
	}
#endif

	/* Initialize event queue */
	INIT_LIST_HEAD(&hyper_dmabuf_private.event_list);
	init_waitqueue_head(&hyper_dmabuf_private.event_wait);

	hyper_dmabuf_private.curr_num_event = 0;

	/* interrupt for comm should be registered here: */
	return ret;
}

static void hyper_dmabuf_drv_exit(void)
{
#ifdef CONFIG_HYPER_DMABUF_SYSFS
	hyper_dmabuf_unregister_sysfs(hyper_dmabuf_private.device);
#endif

	/* hash tables for export/import entries and ring_infos */
	hyper_dmabuf_table_destroy();

	hyper_dmabuf_private.backend_ops->destroy_comm();

	/* destroy workqueue */
	if (hyper_dmabuf_private.work_queue)
		destroy_workqueue(hyper_dmabuf_private.work_queue);

	/* destroy id_queue */
	if (hyper_dmabuf_private.id_queue)
		destroy_reusable_list();

	dev_info(hyper_dmabuf_private.device,
		 "hyper_dmabuf driver: Exiting\n");

	unregister_device();
}

module_init(hyper_dmabuf_drv_init);
module_exit(hyper_dmabuf_drv_exit);
