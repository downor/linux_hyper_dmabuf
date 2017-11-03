#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#include <asm/uaccess.h>
#include <linux/hashtable.h>
#include <xen/grant_table.h>
#include "hyper_dmabuf_xen_comm.h"
#include "hyper_dmabuf_xen_comm_list.h"

DECLARE_HASHTABLE(hyper_dmabuf_hash_importer_ring, MAX_ENTRY_IMPORT_RING);
DECLARE_HASHTABLE(hyper_dmabuf_hash_exporter_ring, MAX_ENTRY_EXPORT_RING);

int hyper_dmabuf_ring_table_init()
{
	hash_init(hyper_dmabuf_hash_importer_ring);
	hash_init(hyper_dmabuf_hash_exporter_ring);
	return 0;
}

int hyper_dmabuf_ring_table_destroy()
{
	/* TODO: cleanup tables*/
	return 0;
}

int hyper_dmabuf_register_exporter_ring(struct hyper_dmabuf_ring_info_export *ring_info)
{
	struct hyper_dmabuf_exporter_ring_info *info_entry;

	info_entry = kmalloc(sizeof(*info_entry), GFP_KERNEL);

	info_entry->info = ring_info;

	hash_add(hyper_dmabuf_hash_exporter_ring, &info_entry->node,
		info_entry->info->rdomain);

	return 0;
}

int hyper_dmabuf_register_importer_ring(struct hyper_dmabuf_ring_info_import *ring_info)
{
	struct hyper_dmabuf_importer_ring_info *info_entry;

	info_entry = kmalloc(sizeof(*info_entry), GFP_KERNEL);

	info_entry->info = ring_info;

	hash_add(hyper_dmabuf_hash_importer_ring, &info_entry->node,
		info_entry->info->sdomain);

	return 0;
}

struct hyper_dmabuf_ring_info_export *hyper_dmabuf_find_exporter_ring(int domid)
{
	struct hyper_dmabuf_exporter_ring_info *info_entry;
	int bkt;

	hash_for_each(hyper_dmabuf_hash_exporter_ring, bkt, info_entry, node)
		if(info_entry->info->rdomain == domid)
			return info_entry->info;

	return NULL;
}

struct hyper_dmabuf_ring_info_import *hyper_dmabuf_find_importer_ring(int domid)
{
	struct hyper_dmabuf_importer_ring_info *info_entry;
	int bkt;

	hash_for_each(hyper_dmabuf_hash_importer_ring, bkt, info_entry, node)
		if(info_entry->info->sdomain == domid)
			return info_entry->info;

	return NULL;
}

int hyper_dmabuf_remove_exporter_ring(int domid)
{
	struct hyper_dmabuf_exporter_ring_info *info_entry;
	int bkt;

	hash_for_each(hyper_dmabuf_hash_exporter_ring, bkt, info_entry, node)
		if(info_entry->info->rdomain == domid) {
			hash_del(&info_entry->node);
			kfree(info_entry);
			return 0;
		}

	return -1;
}

int hyper_dmabuf_remove_importer_ring(int domid)
{
	struct hyper_dmabuf_importer_ring_info *info_entry;
	int bkt;

	hash_for_each(hyper_dmabuf_hash_importer_ring, bkt, info_entry, node)
		if(info_entry->info->sdomain == domid) {
			hash_del(&info_entry->node);
			kfree(info_entry);
			return 0;
		}

	return -1;
}

void hyper_dmabuf_foreach_exporter_ring(void (*func)(int rdom))
{
	struct hyper_dmabuf_exporter_ring_info *info_entry;
	struct hlist_node *tmp;
	int bkt;

	hash_for_each_safe(hyper_dmabuf_hash_exporter_ring, bkt, tmp, info_entry, node) {
		func(info_entry->info->rdomain);
	}
}

void hyper_dmabuf_foreach_importer_ring(void (*func)(int sdom))
{
	struct hyper_dmabuf_importer_ring_info *info_entry;
	struct hlist_node *tmp;
	int bkt;

	hash_for_each_safe(hyper_dmabuf_hash_importer_ring, bkt, tmp, info_entry, node) {
		func(info_entry->info->sdomain);
	}
}
