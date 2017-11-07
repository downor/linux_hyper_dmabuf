#ifndef __HYPER_DMABUF_XEN_SHM_H__
#define __HYPER_DMABUF_XEN_SHM_H__

/* This collects all reference numbers for 2nd level shared pages and create a table
 * with those in 1st level shared pages then return reference numbers for this top level
 * table. */
int hyper_dmabuf_xen_share_pages(struct page **pages, int domid, int nents,
				 void **refs_info);

int hyper_dmabuf_xen_unshare_pages(void **refs_info, int nents);

/* Maps provided top level ref id and then return array of pages containing data refs.
 */
struct page ** hyper_dmabuf_xen_map_shared_pages(int lvl3_gref, int domid, int nents,
						void **refs_info);

int hyper_dmabuf_xen_unmap_shared_pages(void **refs_info, int nents);

#endif /* __HYPER_DMABUF_XEN_SHM_H__ */
