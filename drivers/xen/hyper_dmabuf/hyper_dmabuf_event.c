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
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/dma-buf.h>
#include <xen/grant_table.h>
#include <asm/xen/page.h>
#include "hyper_dmabuf_drv.h"
#include "hyper_dmabuf_struct.h"
#include "hyper_dmabuf_list.h"
#include "hyper_dmabuf_event.h"

extern struct hyper_dmabuf_private hyper_dmabuf_private;

static void hyper_dmabuf_send_event_locked(struct hyper_dmabuf_event *e)
{
	struct hyper_dmabuf_event *oldest;

	assert_spin_locked(&hyper_dmabuf_private.event_lock);

	/* check current number of event then if it hits the max num allowed
	 * then remove the oldest event in the list */
	if (hyper_dmabuf_private.curr_num_event > MAX_DEPTH_EVENT_QUEUE - 1) {
		oldest = list_first_entry(&hyper_dmabuf_private.event_list,
				struct hyper_dmabuf_event, link);
		list_del(&oldest->link);
		hyper_dmabuf_private.curr_num_event--;
		kfree(oldest);
	}

	list_add_tail(&e->link,
		      &hyper_dmabuf_private.event_list);

	hyper_dmabuf_private.curr_num_event++;

	wake_up_interruptible(&hyper_dmabuf_private.event_wait);
}

void hyper_dmabuf_events_release()
{
	struct hyper_dmabuf_event *e, *et;
	unsigned long irqflags;

	spin_lock_irqsave(&hyper_dmabuf_private.event_lock, irqflags);

	list_for_each_entry_safe(e, et, &hyper_dmabuf_private.event_list,
				 link) {
		list_del(&e->link);
		kfree(e);
		hyper_dmabuf_private.curr_num_event--;
	}

	if (hyper_dmabuf_private.curr_num_event) {
		dev_err(hyper_dmabuf_private.device,
			"possible leak on event_list\n");
	}

	spin_unlock_irqrestore(&hyper_dmabuf_private.event_lock, irqflags);
}

int hyper_dmabuf_import_event(hyper_dmabuf_id_t hid)
{
	struct hyper_dmabuf_event *e;
	struct hyper_dmabuf_imported_sgt_info *imported_sgt_info;

	unsigned long irqflags;

	imported_sgt_info = hyper_dmabuf_find_imported(hid);

	if (!imported_sgt_info) {
		dev_err(hyper_dmabuf_private.device,
			"can't find imported_sgt_info in the list\n");
		return -EINVAL;
	}

	e = kzalloc(sizeof(*e), GFP_KERNEL);

	e->event_data.hdr.event_type = HYPER_DMABUF_NEW_IMPORT;
	e->event_data.hdr.hid = hid;
	e->event_data.data = (void*)imported_sgt_info->priv;
	e->event_data.hdr.size = imported_sgt_info->sz_priv;

	spin_lock_irqsave(&hyper_dmabuf_private.event_lock, irqflags);

	hyper_dmabuf_send_event_locked(e);

	spin_unlock_irqrestore(&hyper_dmabuf_private.event_lock, irqflags);

	dev_dbg(hyper_dmabuf_private.device,
			"event number = %d :", hyper_dmabuf_private.curr_num_event);

	dev_dbg(hyper_dmabuf_private.device,
			"generating events for {%d, %d, %d, %d}\n",
			imported_sgt_info->hid.id, imported_sgt_info->hid.rng_key[0],
			imported_sgt_info->hid.rng_key[1], imported_sgt_info->hid.rng_key[2]);

	return 0;
}
