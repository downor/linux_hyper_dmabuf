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
#include <linux/cdev.h>
#include <asm/uaccess.h>
#include <linux/hashtable.h>
#include <linux/dma-buf.h>
#include "hyper_dmabuf_drv.h"
#include "hyper_dmabuf_list.h"
#include "hyper_dmabuf_id.h"
#include "hyper_dmabuf_event.h"

extern struct hyper_dmabuf_private hyper_dmabuf_private;

DECLARE_HASHTABLE(hyper_dmabuf_hash_imported, MAX_ENTRY_IMPORTED);
DECLARE_HASHTABLE(hyper_dmabuf_hash_exported, MAX_ENTRY_EXPORTED);

#ifdef CONFIG_HYPER_DMABUF_SYSFS
static ssize_t hyper_dmabuf_imported_show(struct device *drv, struct device_attribute *attr, char *buf)
{
	struct hyper_dmabuf_info_entry_imported *info_entry;
	int bkt;
	ssize_t count = 0;
	size_t total = 0;

	hash_for_each(hyper_dmabuf_hash_imported, bkt, info_entry, node) {
		hyper_dmabuf_id_t hid = info_entry->info->hid;
		int nents = info_entry->info->nents;
		bool valid = info_entry->info->valid;
		int num_importers = info_entry->info->num_importers;
		total += nents;
		count += scnprintf(buf + count, PAGE_SIZE - count,
				   "hid:{id:%d keys:%d %d %d}, nents:%d, v:%c, numi:%d\n",
				   hid.id, hid.rng_key[0], hid.rng_key[1], hid.rng_key[2],
				   nents, (valid ? 't' : 'f'), num_importers);
	}
	count += scnprintf(buf + count, PAGE_SIZE - count, "total nents: %lu\n",
			   total);

	return count;
}

static ssize_t hyper_dmabuf_exported_show(struct device *drv, struct device_attribute *attr, char *buf)
{
	struct hyper_dmabuf_info_entry_exported *info_entry;
	int bkt;
	ssize_t count = 0;
	size_t total = 0;

	hash_for_each(hyper_dmabuf_hash_exported, bkt, info_entry, node) {
		hyper_dmabuf_id_t hid = info_entry->info->hid;
		int nents = info_entry->info->nents;
		bool valid = info_entry->info->valid;
		int importer_exported = info_entry->info->importer_exported;
		total += nents;
		count += scnprintf(buf + count, PAGE_SIZE - count,
				   "hid:{hid:%d keys:%d %d %d}, nents:%d, v:%c, ie:%d\n",
				   hid.id, hid.rng_key[0], hid.rng_key[1], hid.rng_key[2],
				   nents, (valid ? 't' : 'f'), importer_exported);
	}
	count += scnprintf(buf + count, PAGE_SIZE - count, "total nents: %lu\n",
			   total);

	return count;
}

static DEVICE_ATTR(imported, S_IRUSR, hyper_dmabuf_imported_show, NULL);
static DEVICE_ATTR(exported, S_IRUSR, hyper_dmabuf_exported_show, NULL);

int hyper_dmabuf_register_sysfs(struct device *dev)
{
	int err;

	err = device_create_file(dev, &dev_attr_imported);
	if (err < 0)
		goto err1;
	err = device_create_file(dev, &dev_attr_exported);
	if (err < 0)
		goto err2;

	return 0;
err2:
	device_remove_file(dev, &dev_attr_imported);
err1:
	return -1;
}

int hyper_dmabuf_unregister_sysfs(struct device *dev)
{
	device_remove_file(dev, &dev_attr_imported);
	device_remove_file(dev, &dev_attr_exported);
	return 0;
}
#endif

int hyper_dmabuf_table_init()
{
	hash_init(hyper_dmabuf_hash_imported);
	hash_init(hyper_dmabuf_hash_exported);
	return 0;
}

int hyper_dmabuf_table_destroy()
{
	/* TODO: cleanup hyper_dmabuf_hash_imported and hyper_dmabuf_hash_exported */
	return 0;
}

int hyper_dmabuf_register_exported(struct hyper_dmabuf_sgt_info *info)
{
	struct hyper_dmabuf_info_entry_exported *info_entry;

	info_entry = kmalloc(sizeof(*info_entry), GFP_KERNEL);

	if (!info_entry) {
		dev_err(hyper_dmabuf_private.device,
                        "No memory left to be allocated\n");
		return -ENOMEM;
	}

	info_entry->info = info;

	hash_add(hyper_dmabuf_hash_exported, &info_entry->node,
		 info_entry->info->hid.id);

	return 0;
}

int hyper_dmabuf_register_imported(struct hyper_dmabuf_imported_sgt_info* info)
{
	struct hyper_dmabuf_info_entry_imported *info_entry;

	info_entry = kmalloc(sizeof(*info_entry), GFP_KERNEL);

	if (!info_entry) {
		dev_err(hyper_dmabuf_private.device,
                        "No memory left to be allocated\n");
		return -ENOMEM;
	}

	info_entry->info = info;

	hash_add(hyper_dmabuf_hash_imported, &info_entry->node,
		 info_entry->info->hid.id);

	return 0;
}

struct hyper_dmabuf_sgt_info *hyper_dmabuf_find_exported(hyper_dmabuf_id_t hid)
{
	struct hyper_dmabuf_info_entry_exported *info_entry;
	int bkt;

	hash_for_each(hyper_dmabuf_hash_exported, bkt, info_entry, node)
		/* checking hid.id first */
		if(info_entry->info->hid.id == hid.id) {
			/* then key is compared */
			if(hyper_dmabuf_hid_keycomp(info_entry->info->hid, hid))
				return info_entry->info;
			/* if key is unmatched, given HID is invalid, so returning NULL */
			else
				break;
		}

	return NULL;
}

/* search for pre-exported sgt and return id of it if it exist */
hyper_dmabuf_id_t hyper_dmabuf_find_hid_exported(struct dma_buf *dmabuf, int domid)
{
	struct hyper_dmabuf_info_entry_exported *info_entry;
	hyper_dmabuf_id_t hid = {-1, {0, 0, 0}};
	int bkt;

	hash_for_each(hyper_dmabuf_hash_exported, bkt, info_entry, node)
		if(info_entry->info->dma_buf == dmabuf &&
		   info_entry->info->hyper_dmabuf_rdomain == domid)
			return info_entry->info->hid;

	return hid;
}

struct hyper_dmabuf_imported_sgt_info *hyper_dmabuf_find_imported(hyper_dmabuf_id_t hid)
{
	struct hyper_dmabuf_info_entry_imported *info_entry;
	int bkt;

	hash_for_each(hyper_dmabuf_hash_imported, bkt, info_entry, node)
		/* checking hid.id first */
		if(info_entry->info->hid.id == hid.id) {
			/* then key is compared */
			if(hyper_dmabuf_hid_keycomp(info_entry->info->hid, hid))
				return info_entry->info;
			/* if key is unmatched, given HID is invalid, so returning NULL */
			else {
				break;
			}
		}

	return NULL;
}

int hyper_dmabuf_remove_exported(hyper_dmabuf_id_t hid)
{
	struct hyper_dmabuf_info_entry_exported *info_entry;
	int bkt;

	hash_for_each(hyper_dmabuf_hash_exported, bkt, info_entry, node)
		/* checking hid.id first */
		if(info_entry->info->hid.id == hid.id) {
			/* then key is compared */
			if(hyper_dmabuf_hid_keycomp(info_entry->info->hid, hid)) {
				hash_del(&info_entry->node);
				kfree(info_entry);
				return 0;
			} else {
				break;
			}
		}

	return -ENOENT;
}

int hyper_dmabuf_remove_imported(hyper_dmabuf_id_t hid)
{
	struct hyper_dmabuf_info_entry_imported *info_entry;
	int bkt;

	hash_for_each(hyper_dmabuf_hash_imported, bkt, info_entry, node)
		/* checking hid.id first */
		if(info_entry->info->hid.id == hid.id) {
			/* then key is compared */
			if(hyper_dmabuf_hid_keycomp(info_entry->info->hid, hid)) {
				hash_del(&info_entry->node);
				kfree(info_entry);
				return 0;
			} else {
				break;
			}
		}

	return -ENOENT;
}

void hyper_dmabuf_foreach_exported(
	void (*func)(struct hyper_dmabuf_sgt_info *, void *attr),
	void *attr)
{
	struct hyper_dmabuf_info_entry_exported *info_entry;
	struct hlist_node *tmp;
	int bkt;

	hash_for_each_safe(hyper_dmabuf_hash_exported, bkt, tmp,
			info_entry, node) {
		func(info_entry->info, attr);
	}
}
