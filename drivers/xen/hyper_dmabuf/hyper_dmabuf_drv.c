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
#include <linux/workqueue.h>
#include <linux/device.h>
#include "hyper_dmabuf_conf.h"
#include "hyper_dmabuf_msg.h"
#include "hyper_dmabuf_drv.h"
#include "hyper_dmabuf_list.h"
#include "hyper_dmabuf_id.h"

#ifdef CONFIG_HYPER_DMABUF_XEN
#include "xen/hyper_dmabuf_xen_drv.h"
extern struct hyper_dmabuf_backend_ops xen_backend_ops;
#endif

MODULE_LICENSE("GPL and additional rights");
MODULE_AUTHOR("Intel Corporation");

int register_device(void);
int unregister_device(void);

struct hyper_dmabuf_private hyper_dmabuf_private;

/*===============================================================================================*/
static int __init hyper_dmabuf_drv_init(void)
{
	int ret = 0;

	printk( KERN_NOTICE "hyper_dmabuf_starting: Initialization started" );

	mutex_init(&hyper_dmabuf_private.lock);

	ret = register_device();
	if (ret < 0) {
		return -EINVAL;
	}

#ifdef CONFIG_HYPER_DMABUF_XEN
	hyper_dmabuf_private.backend_ops = &xen_backend_ops;
#endif

	dev_info(hyper_dmabuf_private.device,
		 "initializing database for imported/exported dmabufs\n");

	/* device structure initialization */
	/* currently only does work-queue initialization */
	hyper_dmabuf_private.work_queue = create_workqueue("hyper_dmabuf_wqueue");
	hyper_dmabuf_private.domid = hyper_dmabuf_private.backend_ops->get_vm_id();

	ret = hyper_dmabuf_table_init();
	if (ret < 0) {
		return -EINVAL;
	}

	ret = hyper_dmabuf_private.backend_ops->init_comm_env();
	if (ret < 0) {
		return -EINVAL;
	}

#ifdef CONFIG_HYPER_DMABUF_SYSFS
	ret = hyper_dmabuf_register_sysfs(hyper_dmabuf_private.device);
	if (ret < 0) {
		return -EINVAL;
	}
#endif

	/* interrupt for comm should be registered here: */
	return ret;
}

/*-----------------------------------------------------------------------------------------------*/
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

	printk( KERN_NOTICE "dma_buf-src_sink model: Exiting" );
	unregister_device();
}
/*===============================================================================================*/

module_init(hyper_dmabuf_drv_init);
module_exit(hyper_dmabuf_drv_exit);
