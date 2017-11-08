#ifndef __HYPER_DMABUF_QUERY_H__
#define __HYPER_DMABUF_QUERY_H__

int hyper_dmabuf_query_imported(struct hyper_dmabuf_imported_sgt_info *imported_sgt_info, int query);

int hyper_dmabuf_query_exported(struct hyper_dmabuf_sgt_info *sgt_info, int query);

#endif // __HYPER_DMABUF_QUERY_H__
