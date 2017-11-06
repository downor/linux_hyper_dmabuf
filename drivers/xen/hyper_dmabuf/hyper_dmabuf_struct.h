#ifndef __HYPER_DMABUF_STRUCT_H__
#define __HYPER_DMABUF_STRUCT_H__

#include <xen/interface/grant_table.h>

/* Importer combine source domain id with given hyper_dmabuf_id
 * to make it unique in case there are multiple exporters */

#define HYPER_DMABUF_ID_IMPORTER(sdomain, id) \
	((((sdomain) & 0xFF) << 24) | ((id) & 0xFFFFFF))

#define HYPER_DMABUF_ID_IMPORTER_GET_SDOMAIN_ID(id) \
	(((id) >> 24) & 0xFF)

/* each grant_ref_t is 4 bytes, so total 4096 grant_ref_t can be
 * in this block meaning we can share 4KB*4096 = 16MB of buffer
 * (needs to be increased for large buffer use-cases such as 4K
 * frame buffer) */
#define MAX_ALLOWED_NUM_PAGES_FOR_GREF_NUM_ARRAYS 4

enum hyper_dmabuf_sgt_flags {
        HYPER_DMABUF_SGT_INVALID = 0x10,
        HYPER_DMABUF_SGT_UNEXPORTED,
};

/* stack of mapped sgts */
struct sgt_list {
	struct sg_table *sgt;
	struct list_head list;
};

/* stack of attachments */
struct attachment_list {
	struct dma_buf_attachment *attach;
	struct list_head list;
};

/* stack of vaddr mapped via kmap */
struct kmap_vaddr_list {
	void *vaddr;
	struct list_head list;
};

/* stack of vaddr mapped via vmap */
struct vmap_vaddr_list {
	void *vaddr;
	struct list_head list;
};

struct hyper_dmabuf_shared_pages_info {
	grant_ref_t *data_refs;	/* table with shared buffer pages refid */
	grant_ref_t *addr_pages; /* pages of 2nd level addressing */
	grant_ref_t *top_level_page; /* page of top level addressing, it contains refids of 2nd level pages */
	grant_ref_t top_level_ref; /* top level refid */
	struct gnttab_unmap_grant_ref* unmap_ops; /* unmap ops for mapped pages */
	struct page **data_pages; /* data pages to be unmapped */
};

/* Exporter builds pages_info before sharing pages */
struct hyper_dmabuf_pages_info {
        int hyper_dmabuf_id; /* unique id to reference dmabuf in source domain */
        int hyper_dmabuf_rdomain; /* currenting considering just one remote domain access it */
        int frst_ofst; /* offset of data in the first page */
        int last_len; /* length of data in the last page */
        int nents; /* # of pages */
        struct page **pages; /* pages that contains reference numbers of shared pages*/
};

/* Both importer and exporter use this structure to point to sg lists
 *
 * Exporter stores references to sgt in a hash table
 * Exporter keeps these references for synchronization and tracking purposes
 *
 * Importer use this structure exporting to other drivers in the same domain */
struct hyper_dmabuf_sgt_info {
        int hyper_dmabuf_id; /* unique id to reference dmabuf in remote domain */
	int hyper_dmabuf_rdomain; /* domain importing this sgt */

	struct dma_buf *dma_buf; /* needed to store this for freeing it later */
	struct sgt_list *active_sgts;
	struct attachment_list *active_attached;
	struct kmap_vaddr_list *va_kmapped;
	struct vmap_vaddr_list *va_vmapped;
	int flags;
	struct hyper_dmabuf_shared_pages_info shared_pages_info;
	int private[4]; /* device specific info (e.g. image's meta info?) */
};

/* Importer store references (before mapping) on shared pages
 * Importer store these references in the table and map it in
 * its own memory map once userspace asks for reference for the buffer */
struct hyper_dmabuf_imported_sgt_info {
	int hyper_dmabuf_id; /* unique id to reference dmabuf (HYPER_DMABUF_ID_IMPORTER(source domain id, exporter's hyper_dmabuf_id */
	int frst_ofst;	/* start offset in shared page #1 */
	int last_len;	/* length of data in the last shared page */
	int nents;	/* number of pages to be shared */
	grant_ref_t gref; /* reference number of top level addressing page of shared pages */
	struct sg_table *sgt; /* sgt pointer after importing buffer */
	struct hyper_dmabuf_shared_pages_info shared_pages_info;
	int flags;
	int ref_count;
	int private[4]; /* device specific info (e.g. image's meta info?) */
};

#endif /* __HYPER_DMABUF_STRUCT_H__ */
