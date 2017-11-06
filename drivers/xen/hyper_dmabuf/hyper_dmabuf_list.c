#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#include <asm/uaccess.h>
#include <linux/hashtable.h>
#include <linux/dma-buf.h>
#include "hyper_dmabuf_list.h"

DECLARE_HASHTABLE(hyper_dmabuf_hash_imported, MAX_ENTRY_IMPORTED);
DECLARE_HASHTABLE(hyper_dmabuf_hash_exported, MAX_ENTRY_EXPORTED);

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

	info_entry->info = info;

	hash_add(hyper_dmabuf_hash_exported, &info_entry->node,
		 info_entry->info->hyper_dmabuf_id);

	return 0;
}

int hyper_dmabuf_register_imported(struct hyper_dmabuf_imported_sgt_info* info)
{
	struct hyper_dmabuf_info_entry_imported *info_entry;

	info_entry = kmalloc(sizeof(*info_entry), GFP_KERNEL);

	info_entry->info = info;

	hash_add(hyper_dmabuf_hash_imported, &info_entry->node,
		 info_entry->info->hyper_dmabuf_id);

	return 0;
}

struct hyper_dmabuf_sgt_info *hyper_dmabuf_find_exported(int id)
{
	struct hyper_dmabuf_info_entry_exported *info_entry;
	int bkt;

	hash_for_each(hyper_dmabuf_hash_exported, bkt, info_entry, node)
		if(info_entry->info->hyper_dmabuf_id == id)
			return info_entry->info;

	return NULL;
}

/* search for pre-exported sgt and return id of it if it exist */
int hyper_dmabuf_find_id_exported(struct dma_buf *dmabuf, int domid)
{
	struct hyper_dmabuf_info_entry_exported *info_entry;
	int bkt;

	hash_for_each(hyper_dmabuf_hash_exported, bkt, info_entry, node)
		if(info_entry->info->dma_buf == dmabuf &&
		   info_entry->info->hyper_dmabuf_rdomain == domid)
			return info_entry->info->hyper_dmabuf_id;

	return -1;
}

struct hyper_dmabuf_imported_sgt_info *hyper_dmabuf_find_imported(int id)
{
	struct hyper_dmabuf_info_entry_imported *info_entry;
	int bkt;

	hash_for_each(hyper_dmabuf_hash_imported, bkt, info_entry, node)
		if(info_entry->info->hyper_dmabuf_id == id)
			return info_entry->info;

	return NULL;
}

int hyper_dmabuf_remove_exported(int id)
{
	struct hyper_dmabuf_info_entry_exported *info_entry;
	int bkt;

	hash_for_each(hyper_dmabuf_hash_exported, bkt, info_entry, node)
		if(info_entry->info->hyper_dmabuf_id == id) {
			hash_del(&info_entry->node);
			kfree(info_entry);
			return 0;
		}

	return -1;
}

int hyper_dmabuf_remove_imported(int id)
{
	struct hyper_dmabuf_info_entry_imported *info_entry;
	int bkt;

	hash_for_each(hyper_dmabuf_hash_imported, bkt, info_entry, node)
		if(info_entry->info->hyper_dmabuf_id == id) {
			hash_del(&info_entry->node);
			kfree(info_entry);
			return 0;
		}

	return -1;
}
