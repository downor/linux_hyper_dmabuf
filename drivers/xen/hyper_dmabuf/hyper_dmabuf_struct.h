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

#ifndef __HYPER_DMABUF_STRUCT_H__
#define __HYPER_DMABUF_STRUCT_H__

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
	int nents; /* number of pages, which may be different than sgt->nents */
	struct sgt_list *active_sgts;
	struct attachment_list *active_attached;
	struct kmap_vaddr_list *va_kmapped;
	struct vmap_vaddr_list *va_vmapped;
	bool valid;
	int importer_exported; /* exported locally on importer's side */
	void *refs_info; /* hypervisor-specific info for the references */
	int private[4]; /* device specific info (e.g. image's meta info?) */
};

/* Importer store references (before mapping) on shared pages
 * Importer store these references in the table and map it in
 * its own memory map once userspace asks for reference for the buffer */
struct hyper_dmabuf_imported_sgt_info {
	int hyper_dmabuf_id; /* unique id to reference dmabuf (HYPER_DMABUF_ID_IMPORTER(source domain id, exporter's hyper_dmabuf_id */
	int ref_handle; /* reference number of top level addressing page of shared pages */
	int frst_ofst;	/* start offset in shared page #1 */
	int last_len;	/* length of data in the last shared page */
	int nents;	/* number of pages to be shared */
	struct dma_buf *dma_buf;
	struct sg_table *sgt; /* sgt pointer after importing buffer */
	void *refs_info;
	bool valid;
	int num_importers;
	int private[4]; /* device specific info (e.g. image's meta info?) */
};

#endif /* __HYPER_DMABUF_STRUCT_H__ */
