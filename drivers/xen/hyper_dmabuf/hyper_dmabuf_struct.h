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
struct pages_info {
        int frst_ofst; /* offset of data in the first page */
        int last_len; /* length of data in the last page */
        int nents; /* # of pages */
        struct page **pgs; /* pages that contains reference numbers of shared pages*/
};


/* Exporter stores references to sgt in a hash table
 * Exporter keeps these references for synchronization and tracking purposes
 */
struct exported_sgt_info {
        hyper_dmabuf_id_t hid; /* unique id to reference dmabuf in remote domain */
	int rdomid; /* domain importing this sgt */

	struct dma_buf *dma_buf; /* needed to store this for freeing it later */
	int nents;

	/* list of remote activities on dma_buf */
	struct sgt_list *active_sgts;
	struct attachment_list *active_attached;
	struct kmap_vaddr_list *va_kmapped;
	struct vmap_vaddr_list *va_vmapped;

	bool valid; /* set to 0 once unexported. Needed to prevent further mapping by importer */
	int active; /* locally shared on importer's side */
	void *refs_info; /* hypervisor-specific info for the references */
	struct delayed_work unexport;
	bool unexport_sched;

	/* owner of buffer
	 * TODO: that is naiive as buffer may be reused by
	 * another userspace app, so here list of struct file should be kept
	 * and emergency unexport should be executed only after last of buffer
	 * uses releases hyper_dmabuf device
	 */
	struct file *filp;

	size_t sz_priv;
	char *priv; /* device specific info (e.g. image's meta info?) */
};

/* Importer store references (before mapping) on shared pages
 * Importer store these references in the table and map it in
 * its own memory map once userspace asks for reference for the buffer */
struct imported_sgt_info {
	hyper_dmabuf_id_t hid; /* unique id for shared dmabuf imported */

	int ref_handle; /* reference number of top level addressing page of shared pages */
	int frst_ofst;	/* start offset in first shared page */
	int last_len;	/* length of data in the last shared page */
	int nents;	/* number of pages to be shared */

	struct dma_buf *dma_buf;
	struct sg_table *sgt; /* sgt pointer after importing buffer */

	void *refs_info;
	bool valid;
	int importers;

	size_t sz_priv;
	char *priv; /* device specific info (e.g. image's meta info?) */
};

#endif /* __HYPER_DMABUF_STRUCT_H__ */
