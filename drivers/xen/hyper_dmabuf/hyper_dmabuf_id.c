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

#include <linux/list.h>
#include <linux/slab.h>
#include "hyper_dmabuf_msg.h"
#include "hyper_dmabuf_drv.h"
#include "hyper_dmabuf_id.h"

extern struct hyper_dmabuf_private hyper_dmabuf_private;

void store_reusable_id(int id)
{
	struct list_reusable_id *reusable_head = hyper_dmabuf_private.id_queue;
	struct list_reusable_id *new_reusable;

	new_reusable = kmalloc(sizeof(*new_reusable), GFP_KERNEL);

	if (!new_reusable) {
		dev_err(hyper_dmabuf_private.device,
			"No memory left to be allocated\n");
		return;
	}

	new_reusable->id = id;

	list_add(&new_reusable->list, &reusable_head->list);
}

static int retrieve_reusable_id(void)
{
	struct list_reusable_id *reusable_head = hyper_dmabuf_private.id_queue;
	int id;

	/* check there is reusable id */
	if (!list_empty(&reusable_head->list)) {
		reusable_head = list_first_entry(&reusable_head->list,
						 struct list_reusable_id,
						 list);

		list_del(&reusable_head->list);
		id = reusable_head->id;
		kfree(reusable_head);
		return id;
	}

	return -ENOENT;
}

void destroy_reusable_list(void)
{
	struct list_reusable_id *reusable_head = hyper_dmabuf_private.id_queue;
	struct list_reusable_id *temp_head;

	if (reusable_head) {
		/* freeing mem space all reusable ids in the stack */
		while (!list_empty(&reusable_head->list)) {
			temp_head = list_first_entry(&reusable_head->list,
						     struct list_reusable_id,
						     list);
			list_del(&temp_head->list);
			kfree(temp_head);
		}

		/* freeing head */
		kfree(reusable_head);
	}
}

int hyper_dmabuf_get_id(void)
{
	static int id = 0;
	struct list_reusable_id *reusable_head;
	int ret;

	/* first cla to hyper_dmabuf_get_id */
	if (id == 0) {
		reusable_head = kmalloc(sizeof(*reusable_head), GFP_KERNEL);

		if (!reusable_head) {
			dev_err(hyper_dmabuf_private.device,
				"No memory left to be allocated\n");
			return -ENOMEM;
		}

		reusable_head->id = -1; /* list head have invalid id */
		INIT_LIST_HEAD(&reusable_head->list);
		hyper_dmabuf_private.id_queue = reusable_head;
	}

	ret = retrieve_reusable_id();

	if (ret < 0 && id < HYPER_DMABUF_ID_MAX)
		return HYPER_DMABUF_ID_CREATE(hyper_dmabuf_private.domid, id++);

	return ret;
}
