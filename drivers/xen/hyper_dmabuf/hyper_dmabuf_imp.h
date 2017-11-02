#ifndef __HYPER_DMABUF_IMP_H__
#define __HYPER_DMABUF_IMP_H__

#include "hyper_dmabuf_struct.h"

/* extract pages directly from struct sg_table */
struct hyper_dmabuf_pages_info *hyper_dmabuf_ext_pgs(struct sg_table *sgt);

/* create sg_table with given pages and other parameters */
struct sg_table* hyper_dmabuf_create_sgt(struct page **pages,
                                int frst_ofst, int last_len, int nents);

grant_ref_t hyper_dmabuf_create_gref_table(struct page **pages, int rdomain, int nents,
					   struct hyper_dmabuf_shared_pages_info *shared_pages_info);

int hyper_dmabuf_cleanup_gref_table(struct hyper_dmabuf_sgt_info *sgt_info);

int hyper_dmabuf_cleanup_imported_pages(struct hyper_dmabuf_imported_sgt_info *sgt_info);

/* map first level tables that contains reference numbers for actual shared pages */
grant_ref_t *hyper_dmabuf_map_gref_table(grant_ref_t *gref_table, int n_pages_table);

/* map and construct sg_lists from reference numbers */
struct sg_table* hyper_dmabuf_map_pages(grant_ref_t gref, int frst_ofst, int last_len, int nents, int sdomain,
					struct hyper_dmabuf_shared_pages_info *shared_pages_info);

int hyper_dmabuf_export_fd(struct hyper_dmabuf_imported_sgt_info *dinfo, int flags);

struct dma_buf* hyper_dmabuf_export_dma_buf(struct hyper_dmabuf_imported_sgt_info *dinfo);

#endif /* __HYPER_DMABUF_IMP_H__ */
