#ifndef __HYPER_DMABUF_XEN_COMM_LIST_H__
#define __HYPER_DMABUF_XEN_COMM_LIST_H__

/* number of bits to be used for exported dmabufs hash table */
#define MAX_ENTRY_EXPORT_RING 7
/* number of bits to be used for imported dmabufs hash table */
#define MAX_ENTRY_IMPORT_RING 7

struct hyper_dmabuf_exporter_ring_info {
        struct hyper_dmabuf_ring_info_export *info;
        struct hlist_node node;
};

struct hyper_dmabuf_importer_ring_info {
        struct hyper_dmabuf_ring_info_import *info;
        struct hlist_node node;
};

int hyper_dmabuf_ring_table_init(void);

int hyper_dmabuf_ring_table_destroy(void);

int hyper_dmabuf_register_exporter_ring(struct hyper_dmabuf_ring_info_export *ring_info);

int hyper_dmabuf_register_importer_ring(struct hyper_dmabuf_ring_info_import *ring_info);

struct hyper_dmabuf_ring_info_export *hyper_dmabuf_find_exporter_ring(int domid);

struct hyper_dmabuf_ring_info_import *hyper_dmabuf_find_importer_ring(int domid);

int hyper_dmabuf_remove_exporter_ring(int domid);

int hyper_dmabuf_remove_importer_ring(int domid);

#endif // __HYPER_DMABUF_XEN_COMM_LIST_H__
