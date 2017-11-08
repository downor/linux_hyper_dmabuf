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
 */

#ifndef __LINUX_PUBLIC_HYPER_DMABUF_DRV_H__
#define __LINUX_PUBLIC_HYPER_DMABUF_DRV_H__

#include <linux/device.h>
#include <xen/hyper_dmabuf.h>

struct hyper_dmabuf_req;

struct list_reusable_id {
	hyper_dmabuf_id_t hid;
	struct list_head list;
};

struct hyper_dmabuf_backend_ops {
	/* retreiving id of current virtual machine */
	int (*get_vm_id)(void);

	/* get pages shared via hypervisor-specific method */
	int (*share_pages)(struct page **, int, int, void **);

	/* make shared pages unshared via hypervisor specific method */
	int (*unshare_pages)(void **, int);

	/* map remotely shared pages on importer's side via
	 * hypervisor-specific method
	 */
	struct page ** (*map_shared_pages)(int, int, int, void **);

	/* unmap and free shared pages on importer's side via
	 * hypervisor-specific method
	 */
	int (*unmap_shared_pages)(void **, int);

	/* initialize communication environment */
	int (*init_comm_env)(void);

	void (*destroy_comm)(void);

	/* upstream ch setup (receiving and responding) */
	int (*init_rx_ch)(int);

	/* downstream ch setup (transmitting and parsing responses) */
	int (*init_tx_ch)(int);

	int (*send_req)(int, struct hyper_dmabuf_req *, int);
};

struct hyper_dmabuf_private {
        struct device *device;
	int domid;
	struct workqueue_struct *work_queue;
	struct list_reusable_id *id_queue;

	/* backend ops - hypervisor specific */
	struct hyper_dmabuf_backend_ops *backend_ops;
	struct mutex lock;
	bool backend_initialized;
};

#endif /* __LINUX_PUBLIC_HYPER_DMABUF_DRV_H__ */
