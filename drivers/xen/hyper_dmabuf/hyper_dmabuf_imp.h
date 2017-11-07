#ifndef __HYPER_DMABUF_IMP_H__
#define __HYPER_DMABUF_IMP_H__

#include <linux/fs.h>
#include "hyper_dmabuf_struct.h"

/* extract pages directly from struct sg_table */
struct hyper_dmabuf_pages_info *hyper_dmabuf_ext_pgs(struct sg_table *sgt);

/* create sg_table with given pages and other parameters */
struct sg_table* hyper_dmabuf_create_sgt(struct page **pages,
                                int frst_ofst, int last_len, int nents);

int hyper_dmabuf_cleanup_sgt_info(struct hyper_dmabuf_sgt_info *sgt_info, int force);

void hyper_dmabuf_free_sgt(struct sg_table *sgt);

int hyper_dmabuf_export_fd(struct hyper_dmabuf_imported_sgt_info *dinfo, int flags);

void hyper_dmabuf_export_dma_buf(struct hyper_dmabuf_imported_sgt_info *dinfo);

int dmabuf_refcount(struct dma_buf *dma_buf);

#endif /* __HYPER_DMABUF_IMP_H__ */
