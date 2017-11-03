#ifndef __HYPER_DMABUF_LIST_H__
#define __HYPER_DMABUF_LIST_H__

#include "hyper_dmabuf_struct.h"

/* number of bits to be used for exported dmabufs hash table */
#define MAX_ENTRY_EXPORTED 7
/* number of bits to be used for imported dmabufs hash table */
#define MAX_ENTRY_IMPORTED 7

struct hyper_dmabuf_info_entry_exported {
        struct hyper_dmabuf_sgt_info *info;
        struct hlist_node node;
};

struct hyper_dmabuf_info_entry_imported {
        struct hyper_dmabuf_imported_sgt_info *info;
        struct hlist_node node;
};

int hyper_dmabuf_table_init(void);

int hyper_dmabuf_table_destroy(void);

int hyper_dmabuf_register_exported(struct hyper_dmabuf_sgt_info *info);

/* search for pre-exported sgt and return id of it if it exist */
int hyper_dmabuf_find_id(struct dma_buf *dmabuf, int domid);

int hyper_dmabuf_register_imported(struct hyper_dmabuf_imported_sgt_info* info);

struct hyper_dmabuf_sgt_info *hyper_dmabuf_find_exported(int id);

struct hyper_dmabuf_imported_sgt_info *hyper_dmabuf_find_imported(int id);

int hyper_dmabuf_remove_exported(int id);

int hyper_dmabuf_remove_imported(int id);

#endif // __HYPER_DMABUF_LIST_H__
