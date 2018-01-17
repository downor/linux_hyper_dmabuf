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
 * SPDX-License-Identifier: (MIT OR GPL-2.0)
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
#include "hyper_dmabuf_ioctl.h"
#include "hyper_dmabuf_list.h"
#include "hyper_dmabuf_id.h"

MODULE_LICENSE("GPL and additional rights");
MODULE_AUTHOR("Intel Corporation");

struct hyper_dmabuf_private *hy_drv_priv;

static void force_free(struct exported_sgt_info *exported,
		       void *attr)
{
	struct ioctl_hyper_dmabuf_unexport unexport_attr;
	struct file *filp = (struct file *)attr;

	if (!filp || !exported)
		return;

	if (exported->filp == filp) {
		dev_dbg(hy_drv_priv->dev,
			"Forcefully releasing buffer {id:%d key:%d %d %d}\n",
			 exported->hid.id, exported->hid.rng_key[0],
			 exported->hid.rng_key[1], exported->hid.rng_key[2]);

		unexport_attr.hid = exported->hid;
		unexport_attr.delay_ms = 0;

		hyper_dmabuf_unexport_ioctl(filp, &unexport_attr);
	}
}

static int hyper_dmabuf_open(struct inode *inode, struct file *filp)
{
	int ret = 0;

	/* Do not allow exclusive open */
	if (filp->f_flags & O_EXCL)
		return -EBUSY;

	return ret;
}

static int hyper_dmabuf_release(struct inode *inode, struct file *filp)
{
	hyper_dmabuf_foreach_exported(force_free, filp);

	return 0;
}

static const struct file_operations hyper_dmabuf_driver_fops = {
	.owner = THIS_MODULE,
	.open = hyper_dmabuf_open,
	.release = hyper_dmabuf_release,
	.unlocked_ioctl = hyper_dmabuf_ioctl,
};

static struct miscdevice hyper_dmabuf_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "hyper_dmabuf",
	.fops = &hyper_dmabuf_driver_fops,
};

static int register_device(void)
{
	int ret = 0;

	ret = misc_register(&hyper_dmabuf_miscdev);

	if (ret) {
		pr_err("hyper_dmabuf: driver can't be registered\n");
		return ret;
	}

	hy_drv_priv->dev = hyper_dmabuf_miscdev.this_device;

	/* TODO: Check if there is a different way to initialize dma mask */
	dma_coerce_mask_and_coherent(hy_drv_priv->dev, DMA_BIT_MASK(64));

	return ret;
}

static void unregister_device(void)
{
	dev_info(hy_drv_priv->dev,
		"hyper_dmabuf: %s is called\n", __func__);

	misc_deregister(&hyper_dmabuf_miscdev);
}

static int __init hyper_dmabuf_drv_init(void)
{
	int ret = 0;

	pr_notice("hyper_dmabuf_starting: Initialization started\n");

	hy_drv_priv = kcalloc(1, sizeof(struct hyper_dmabuf_private),
			      GFP_KERNEL);

	if (!hy_drv_priv)
		return -ENOMEM;

	ret = register_device();
	if (ret < 0) {
		kfree(hy_drv_priv);
		return ret;
	}

	hy_drv_priv->bknd_ops = NULL;

	if (hy_drv_priv->bknd_ops == NULL) {
		pr_err("Hyper_dmabuf: no backend found\n");
		kfree(hy_drv_priv);
		return -1;
	}

	mutex_init(&hy_drv_priv->lock);

	mutex_lock(&hy_drv_priv->lock);

	hy_drv_priv->initialized = false;

	dev_info(hy_drv_priv->dev,
		 "initializing database for imported/exported dmabufs\n");

	hy_drv_priv->work_queue = create_workqueue("hyper_dmabuf_wqueue");

	ret = hyper_dmabuf_table_init();
	if (ret < 0) {
		dev_err(hy_drv_priv->dev,
			"fail to init table for exported/imported entries\n");
		mutex_unlock(&hy_drv_priv->lock);
		kfree(hy_drv_priv);
		return ret;
	}

#ifdef CONFIG_HYPER_DMABUF_SYSFS
	ret = hyper_dmabuf_register_sysfs(hy_drv_priv->dev);
	if (ret < 0) {
		dev_err(hy_drv_priv->dev,
			"failed to initialize sysfs\n");
		mutex_unlock(&hy_drv_priv->lock);
		kfree(hy_drv_priv);
		return ret;
	}
#endif

	if (hy_drv_priv->bknd_ops->init) {
		ret = hy_drv_priv->bknd_ops->init();

		if (ret < 0) {
			dev_dbg(hy_drv_priv->dev,
				"failed to initialize backend.\n");
			mutex_unlock(&hy_drv_priv->lock);
			kfree(hy_drv_priv);
			return ret;
		}
	}

	hy_drv_priv->domid = hy_drv_priv->bknd_ops->get_vm_id();

	ret = hy_drv_priv->bknd_ops->init_comm_env();
	if (ret < 0) {
		dev_dbg(hy_drv_priv->dev,
			"failed to initialize comm-env.\n");
	} else {
		hy_drv_priv->initialized = true;
	}

	mutex_unlock(&hy_drv_priv->lock);

	dev_info(hy_drv_priv->dev,
		"Finishing up initialization of hyper_dmabuf drv\n");

	/* interrupt for comm should be registered here: */
	return ret;
}

static void hyper_dmabuf_drv_exit(void)
{
#ifdef CONFIG_HYPER_DMABUF_SYSFS
	hyper_dmabuf_unregister_sysfs(hy_drv_priv->dev);
#endif

	mutex_lock(&hy_drv_priv->lock);

	/* hash tables for export/import entries and ring_infos */
	hyper_dmabuf_table_destroy();

	hy_drv_priv->bknd_ops->destroy_comm();

	if (hy_drv_priv->bknd_ops->cleanup) {
		hy_drv_priv->bknd_ops->cleanup();
	};

	/* destroy workqueue */
	if (hy_drv_priv->work_queue)
		destroy_workqueue(hy_drv_priv->work_queue);

	/* destroy id_queue */
	if (hy_drv_priv->id_queue)
		hyper_dmabuf_free_hid_list();

	mutex_unlock(&hy_drv_priv->lock);

	dev_info(hy_drv_priv->dev,
		 "hyper_dmabuf driver: Exiting\n");

	kfree(hy_drv_priv);

	unregister_device();
}

module_init(hyper_dmabuf_drv_init);
module_exit(hyper_dmabuf_drv_exit);
