#ifndef __HYPER_DMABUF_XEN_DRV_H__
#define __HYPER_DMABUF_XEN_DRV_H__
#include <xen/interface/grant_table.h>

extern struct hyper_dmabuf_backend_ops xen_backend_ops;

/* Main purpose of this structure is to keep
 * all references created or acquired for sharing
 * pages with another domain for freeing those later
 * when unsharing.
 */
struct xen_shared_pages_info {
        grant_ref_t lvl3_gref; /* top level refid */
        grant_ref_t *lvl3_table; /* page of top level addressing, it contains refids of 2nd level pages */
        grant_ref_t *lvl2_table; /* table of 2nd level pages, that contains refids to data pages */
        struct gnttab_unmap_grant_ref* unmap_ops; /* unmap ops for mapped pages */
        struct page **data_pages; /* data pages to be unmapped */
};

#endif // __HYPER_DMABUF_XEN_COMM_H__
