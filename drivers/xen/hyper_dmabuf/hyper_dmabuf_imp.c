#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/dma-buf.h>
#include <xen/grant_table.h>
#include <asm/xen/page.h>
#include "hyper_dmabuf_struct.h"
#include "hyper_dmabuf_imp.h"
#include "xen/hyper_dmabuf_xen_comm.h"
#include "hyper_dmabuf_msg.h"

#define REFS_PER_PAGE (PAGE_SIZE/sizeof(grant_ref_t))

/* return total number of pages referecned by a sgt
 * for pre-calculation of # of pages behind a given sgt
 */
static int hyper_dmabuf_get_num_pgs(struct sg_table *sgt)
{
	struct scatterlist *sgl;
	int length, i;
	/* at least one page */
	int num_pages = 1;

	sgl = sgt->sgl;

	length = sgl->length - PAGE_SIZE + sgl->offset;
	num_pages += ((length + PAGE_SIZE - 1)/PAGE_SIZE); /* round-up */

	for (i = 1; i < sgt->nents; i++) {
		sgl = sg_next(sgl);
		num_pages += ((sgl->length + PAGE_SIZE - 1) / PAGE_SIZE); /* round-up */
	}

	return num_pages;
}

/* extract pages directly from struct sg_table */
struct hyper_dmabuf_pages_info *hyper_dmabuf_ext_pgs(struct sg_table *sgt)
{
	struct hyper_dmabuf_pages_info *pinfo;
	int i, j, k;
	int length;
	struct scatterlist *sgl;

	pinfo = kmalloc(sizeof(*pinfo), GFP_KERNEL);
	if (pinfo == NULL)
		return NULL;

	pinfo->pages = kmalloc(sizeof(struct page *)*hyper_dmabuf_get_num_pgs(sgt), GFP_KERNEL);
	if (pinfo->pages == NULL)
		return NULL;

	sgl = sgt->sgl;

	pinfo->nents = 1;
	pinfo->frst_ofst = sgl->offset;
	pinfo->pages[0] = sg_page(sgl);
	length = sgl->length - PAGE_SIZE + sgl->offset;
	i = 1;

	while (length > 0) {
		pinfo->pages[i] = nth_page(sg_page(sgl), i);
		length -= PAGE_SIZE;
		pinfo->nents++;
		i++;
	}

	for (j = 1; j < sgt->nents; j++) {
		sgl = sg_next(sgl);
		pinfo->pages[i++] = sg_page(sgl);
		length = sgl->length - PAGE_SIZE;
		pinfo->nents++;
		k = 1;

		while (length > 0) {
			pinfo->pages[i++] = nth_page(sg_page(sgl), k++);
			length -= PAGE_SIZE;
			pinfo->nents++;
		}
	}

	/*
	 * lenght at that point will be 0 or negative,
	 * so to calculate last page size just add it to PAGE_SIZE
	 */
	pinfo->last_len = PAGE_SIZE + length;

	return pinfo;
}

/* create sg_table with given pages and other parameters */
struct sg_table* hyper_dmabuf_create_sgt(struct page **pages,
				int frst_ofst, int last_len, int nents)
{
	struct sg_table *sgt;
	struct scatterlist *sgl;
	int i, ret;

	sgt = kmalloc(sizeof(struct sg_table), GFP_KERNEL);
	if (sgt == NULL) {
		return NULL;
	}

	ret = sg_alloc_table(sgt, nents, GFP_KERNEL);
	if (ret) {
		kfree(sgt);
		return NULL;
	}

	sgl = sgt->sgl;

	sg_set_page(sgl, pages[0], PAGE_SIZE-frst_ofst, frst_ofst);

	for (i=1; i<nents-1; i++) {
		sgl = sg_next(sgl);
		sg_set_page(sgl, pages[i], PAGE_SIZE, 0);
	}

	if (i > 1) /* more than one page */ {
		sgl = sg_next(sgl);
		sg_set_page(sgl, pages[i], last_len, 0);
	}

	return sgt;
}

/*
 * Creates 2 level page directory structure for referencing shared pages.
 * Top level page is a single page that contains up to 1024 refids that
 * point to 2nd level pages.
 * Each 2nd level page contains up to 1024 refids that point to shared
 * data pages.
 * There will always be one top level page and number of 2nd level pages
 * depends on number of shared data pages.
 *
 *      Top level page                2nd level pages            Data pages
 * +-------------------------+   ┌>+--------------------+ ┌--->+------------+
 * |2nd level page 0 refid   |---┘ |Data page 0 refid   |-┘    |Data page 0 |
 * |2nd level page 1 refid   |---┐ |Data page 1 refid   |-┐    +------------+
 * |           ...           |   | |     ....           | |
 * |2nd level page 1023 refid|-┐ | |Data page 1023 refid| └--->+------------+
 * +-------------------------+ | | +--------------------+      |Data page 1 |
 *                             | |                             +------------+
 *                             | └>+--------------------+
 *                             |   |Data page 1024 refid|
 *                             |   |Data page 1025 refid|
 *                             |   |       ...          |
 *                             |   |Data page 2047 refid|
 *                             |   +--------------------+
 *                             |
 *                             |        .....
 *                             └-->+-----------------------+
 *                                 |Data page 1047552 refid|
 *                                 |Data page 1047553 refid|
 *                                 |       ...             |
 *                                 |Data page 1048575 refid|-->+------------------+
 *                                 +-----------------------+   |Data page 1048575 |
 *                                                             +------------------+
 *
 * Using such 2 level structure it is possible to reference up to 4GB of
 * shared data using single refid pointing to top level page.
 *
 * Returns refid of top level page.
 */
grant_ref_t hyper_dmabuf_create_addressing_tables(grant_ref_t *data_refs, int nents, int rdomain,
						  struct hyper_dmabuf_shared_pages_info *shared_pages_info)
{
	/*
	 * Calculate number of pages needed for 2nd level addresing:
	 */
	int n_2nd_level_pages = (nents/REFS_PER_PAGE + ((nents % REFS_PER_PAGE) ? 1: 0));/* rounding */
	int i;
	unsigned long gref_page_start;
	grant_ref_t *tmp_page;
	grant_ref_t top_level_ref;
	grant_ref_t * addr_refs;
	addr_refs = kcalloc(sizeof(grant_ref_t), n_2nd_level_pages, GFP_KERNEL);

	gref_page_start = __get_free_pages(GFP_KERNEL, n_2nd_level_pages);
	tmp_page = (grant_ref_t *)gref_page_start;

	/* Store 2nd level pages to be freed later */
	shared_pages_info->addr_pages = tmp_page;

	/*TODO: make sure that allocated memory is filled with 0*/

	/* Share 2nd level addressing pages in readonly mode*/
	for (i=0; i< n_2nd_level_pages; i++) {
		addr_refs[i] = gnttab_grant_foreign_access(rdomain, virt_to_mfn((unsigned long)tmp_page+i*PAGE_SIZE ), 1);
	}

	/*
	 * fill second level pages with data refs
	 */
	for (i = 0; i < nents; i++) {
		tmp_page[i] = data_refs[i];
	}


	/* allocate top level page */
	gref_page_start = __get_free_pages(GFP_KERNEL, 1);
	tmp_page = (grant_ref_t *)gref_page_start;

	/* Store top level page to be freed later */
	shared_pages_info->top_level_page = tmp_page;

	/*
	 * fill top level page with reference numbers of second level pages refs.
	 */
	for (i=0; i< n_2nd_level_pages; i++) {
		tmp_page[i] =  addr_refs[i];
	}

	/* Share top level addressing page in readonly mode*/
	top_level_ref = gnttab_grant_foreign_access(rdomain, virt_to_mfn((unsigned long)tmp_page), 1);

	kfree(addr_refs);

	return top_level_ref;
}

/*
 * Maps provided top level ref id and then return array of pages containing data refs.
 */
struct page** hyper_dmabuf_get_data_refs(grant_ref_t top_level_ref, int domid, int nents,
					 struct hyper_dmabuf_shared_pages_info *shared_pages_info)
{
	struct page *top_level_page;
	struct page **level2_pages;

	grant_ref_t *top_level_refs;

	struct gnttab_map_grant_ref top_level_map_ops;
	struct gnttab_unmap_grant_ref top_level_unmap_ops;

	struct gnttab_map_grant_ref *map_ops;
	struct gnttab_unmap_grant_ref *unmap_ops;

	unsigned long addr;
	int n_level2_refs = 0;
	int i;

	n_level2_refs = (nents / REFS_PER_PAGE) + ((nents % REFS_PER_PAGE) ? 1 : 0);

	level2_pages = kcalloc(sizeof(struct page*), n_level2_refs, GFP_KERNEL);

	map_ops = kcalloc(sizeof(map_ops[0]), REFS_PER_PAGE, GFP_KERNEL);
	unmap_ops = kcalloc(sizeof(unmap_ops[0]), REFS_PER_PAGE, GFP_KERNEL);

	/* Map top level addressing page */
	if (gnttab_alloc_pages(1, &top_level_page)) {
		printk("Cannot allocate pages\n");
		return NULL;
	}

	addr = (unsigned long)pfn_to_kaddr(page_to_pfn(top_level_page));
	gnttab_set_map_op(&top_level_map_ops, addr, GNTMAP_host_map | GNTMAP_readonly, top_level_ref, domid);
	gnttab_set_unmap_op(&top_level_unmap_ops, addr, GNTMAP_host_map | GNTMAP_readonly, -1);

	if (gnttab_map_refs(&top_level_map_ops, NULL, &top_level_page, 1)) {
		printk("\nxen: dom0: HYPERVISOR map grant ref failed");
		return NULL;
	}

	if (top_level_map_ops.status) {
		printk("\nxen: dom0: HYPERVISOR map grant ref failed status = %d",
				top_level_map_ops.status);
		return NULL;
	} else {
		top_level_unmap_ops.handle = top_level_map_ops.handle;
	}

	/* Parse contents of top level addressing page to find how many second level pages is there*/
	top_level_refs = pfn_to_kaddr(page_to_pfn(top_level_page));

	/* Map all second level pages */
	if (gnttab_alloc_pages(n_level2_refs, level2_pages)) {
		printk("Cannot allocate pages\n");
		return NULL;
	}

	for (i = 0; i < n_level2_refs; i++) {
		addr = (unsigned long)pfn_to_kaddr(page_to_pfn(level2_pages[i]));
		gnttab_set_map_op(&map_ops[i], addr, GNTMAP_host_map | GNTMAP_readonly, top_level_refs[i], domid);
		gnttab_set_unmap_op(&unmap_ops[i], addr, GNTMAP_host_map | GNTMAP_readonly, -1);
	}

	if (gnttab_map_refs(map_ops, NULL, level2_pages, n_level2_refs)) {
		printk("\nxen: dom0: HYPERVISOR map grant ref failed");
		return NULL;
	}

	/* Checks if pages were mapped correctly and at the same time is calculating total number of data refids*/
	for (i = 0; i < n_level2_refs; i++) {
		if (map_ops[i].status) {
			printk("\nxen: dom0: HYPERVISOR map grant ref failed status = %d",
					map_ops[i].status);
			return NULL;
		} else {
			unmap_ops[i].handle = map_ops[i].handle;
		}
	}

	/* Unmap top level page, as it won't be needed any longer */
	if (gnttab_unmap_refs(&top_level_unmap_ops, NULL, &top_level_page, 1)) {
		printk("\xen: cannot unmap top level page\n");
		return NULL;
	}

	gnttab_free_pages(1, &top_level_page);
	kfree(map_ops);
	shared_pages_info->unmap_ops = unmap_ops;

	return level2_pages;
}


/* This collects all reference numbers for 2nd level shared pages and create a table
 * with those in 1st level shared pages then return reference numbers for this top level
 * table. */
grant_ref_t hyper_dmabuf_create_gref_table(struct page **pages, int rdomain, int nents,
					   struct hyper_dmabuf_shared_pages_info *shared_pages_info)
{
	int i = 0;
	grant_ref_t *data_refs;
	grant_ref_t top_level_ref;

	/* allocate temp array for refs of shared data pages */
	data_refs = kcalloc(nents, sizeof(grant_ref_t), GFP_KERNEL);

	/* share data pages in rw mode*/
	for (i=0; i<nents; i++) {
		data_refs[i] = gnttab_grant_foreign_access(rdomain, pfn_to_mfn(page_to_pfn(pages[i])), 0);
	}

	/* create additional shared pages with 2 level addressing of data pages */
	top_level_ref = hyper_dmabuf_create_addressing_tables(data_refs, nents, rdomain,
							      shared_pages_info);

	/* Store exported pages refid to be unshared later */
	shared_pages_info->data_refs = data_refs;
	shared_pages_info->top_level_ref = top_level_ref;

	return top_level_ref;
}

int hyper_dmabuf_cleanup_gref_table(struct hyper_dmabuf_sgt_info *sgt_info) {
	uint32_t i = 0;
	struct hyper_dmabuf_shared_pages_info *shared_pages_info = &sgt_info->shared_pages_info;

	grant_ref_t *ref = shared_pages_info->top_level_page;
	int n_2nd_level_pages = (sgt_info->sgt->nents/REFS_PER_PAGE + ((sgt_info->sgt->nents % REFS_PER_PAGE) ? 1: 0));/* rounding */


	if (shared_pages_info->data_refs == NULL ||
	    shared_pages_info->addr_pages ==  NULL ||
	    shared_pages_info->top_level_page == NULL ||
	    shared_pages_info->top_level_ref == -1) {
		printk("gref table for hyper_dmabuf already cleaned up\n");
		return 0;
	}

	/* End foreign access for 2nd level addressing pages */
	while(ref[i] != 0 && i < n_2nd_level_pages) {
		if (gnttab_query_foreign_access(ref[i])) {
			printk("refid not shared !!\n");
		}
		if (!gnttab_end_foreign_access_ref(ref[i], 1)) {
			printk("refid still in use!!!\n");
		}
		i++;
	}
	free_pages((unsigned long)shared_pages_info->addr_pages, i);

	/* End foreign access for top level addressing page */
	if (gnttab_query_foreign_access(shared_pages_info->top_level_ref)) {
		printk("refid not shared !!\n");
	}
	if (!gnttab_end_foreign_access_ref(shared_pages_info->top_level_ref, 1)) {
		printk("refid still in use!!!\n");
	}
	gnttab_end_foreign_access_ref(shared_pages_info->top_level_ref, 1);
	free_pages((unsigned long)shared_pages_info->top_level_page, 1);

	/* End foreign access for data pages, but do not free them */
	for (i = 0; i < sgt_info->sgt->nents; i++) {
		if (gnttab_query_foreign_access(shared_pages_info->data_refs[i])) {
			printk("refid not shared !!\n");
		}
		gnttab_end_foreign_access_ref(shared_pages_info->data_refs[i], 0);
	}

	kfree(shared_pages_info->data_refs);

	shared_pages_info->data_refs = NULL;
	shared_pages_info->addr_pages = NULL;
	shared_pages_info->top_level_page = NULL;
	shared_pages_info->top_level_ref = -1;

	return 0;
}

int hyper_dmabuf_cleanup_imported_pages(struct hyper_dmabuf_imported_sgt_info *sgt_info) {
	struct hyper_dmabuf_shared_pages_info *shared_pages_info = &sgt_info->shared_pages_info;

	if(shared_pages_info->unmap_ops == NULL || shared_pages_info->data_pages == NULL) {
		printk("Imported pages already cleaned up or buffer was not imported yet\n");
		return 0;
	}

	if (gnttab_unmap_refs(shared_pages_info->unmap_ops, NULL, shared_pages_info->data_pages, sgt_info->nents) ) {
		printk("Cannot unmap data pages\n");
		return -EINVAL;
	}

	gnttab_free_pages(sgt_info->nents, shared_pages_info->data_pages);
	kfree(shared_pages_info->data_pages);
	kfree(shared_pages_info->unmap_ops);
	shared_pages_info->unmap_ops = NULL;
	shared_pages_info->data_pages = NULL;

	return 0;
}

/* map and construct sg_lists from reference numbers */
struct sg_table* hyper_dmabuf_map_pages(grant_ref_t top_level_gref, int frst_ofst, int last_len, int nents, int sdomain,
					struct hyper_dmabuf_shared_pages_info *shared_pages_info)
{
	struct sg_table *st;
	struct page **pages;
	struct gnttab_map_grant_ref *ops;
	struct gnttab_unmap_grant_ref *unmap_ops;
	unsigned long addr;
	grant_ref_t *refs;
	int i;
	int n_level2_refs = (nents / REFS_PER_PAGE) + ((nents % REFS_PER_PAGE) ? 1 : 0);

	/* Get data refids */
	struct page** refid_pages = hyper_dmabuf_get_data_refs(top_level_gref, sdomain, nents,
							       shared_pages_info);

	pages = kcalloc(sizeof(struct page*), nents, GFP_KERNEL);
	if (pages == NULL) {
		return NULL;
	}

	/* allocate new pages that are mapped to shared pages via grant-table */
	if (gnttab_alloc_pages(nents, pages)) {
		printk("Cannot allocate pages\n");
		return NULL;
	}

	ops = (struct gnttab_map_grant_ref *)kcalloc(nents, sizeof(struct gnttab_map_grant_ref), GFP_KERNEL);
	unmap_ops = (struct gnttab_unmap_grant_ref *)kcalloc(nents, sizeof(struct gnttab_unmap_grant_ref), GFP_KERNEL);

	for (i=0; i<nents; i++) {
		addr = (unsigned long)pfn_to_kaddr(page_to_pfn(pages[i]));
		refs = pfn_to_kaddr(page_to_pfn(refid_pages[i / REFS_PER_PAGE]));
		gnttab_set_map_op(&ops[i], addr, GNTMAP_host_map | GNTMAP_readonly, refs[i % REFS_PER_PAGE], sdomain);
		gnttab_set_unmap_op(&unmap_ops[i], addr, GNTMAP_host_map | GNTMAP_readonly, -1);
	}

	if (gnttab_map_refs(ops, NULL, pages, nents)) {
		printk("\nxen: dom0: HYPERVISOR map grant ref failed\n");
		return NULL;
	}

	for (i=0; i<nents; i++) {
		if (ops[i].status) {
			printk("\nxen: dom0: HYPERVISOR map grant ref failed status = %d\n",
				ops[0].status);
			return NULL;
		} else {
			unmap_ops[i].handle = ops[i].handle;
		}
	}

	st = hyper_dmabuf_create_sgt(pages, frst_ofst, last_len, nents);

	if (gnttab_unmap_refs(shared_pages_info->unmap_ops, NULL, refid_pages, n_level2_refs) ) {
		printk("Cannot unmap 2nd level refs\n");
		return NULL;
	}

	gnttab_free_pages(n_level2_refs, refid_pages);
	kfree(refid_pages);

	kfree(shared_pages_info->unmap_ops);
	shared_pages_info->unmap_ops = unmap_ops;
	shared_pages_info->data_pages = pages;
	kfree(ops);

	return st;
}

inline int hyper_dmabuf_sync_request_and_wait(int id, int ops)
{
	struct hyper_dmabuf_ring_rq *req;
	int operands[2];
	int ret;

	operands[0] = id;
	operands[1] = ops;

	req = kcalloc(1, sizeof(*req), GFP_KERNEL);

	hyper_dmabuf_create_request(req, HYPER_DMABUF_OPS_TO_SOURCE, &operands[0]);

	/* send request */
	ret = hyper_dmabuf_send_request(id, req);

	/* TODO: wait until it gets response.. or can we just move on? */

	kfree(req);

	return ret;
}

static int hyper_dmabuf_ops_attach(struct dma_buf* dmabuf, struct device* dev,
			struct dma_buf_attachment *attach)
{
	struct hyper_dmabuf_imported_sgt_info *sgt_info;
	int ret;

	if (!attach->dmabuf->priv)
		return -EINVAL;

	sgt_info = (struct hyper_dmabuf_imported_sgt_info *)attach->dmabuf->priv;

	ret = hyper_dmabuf_sync_request_and_wait(HYPER_DMABUF_ID_IMPORTER_GET_SDOMAIN_ID(sgt_info->hyper_dmabuf_id),
						HYPER_DMABUF_OPS_ATTACH);

	if (ret < 0) {
		printk("hyper_dmabuf::%s Error:send dmabuf sync request failed\n", __func__);
	}

	/* Ignoring ret for now */
	return 0;
}

static void hyper_dmabuf_ops_detach(struct dma_buf* dmabuf, struct dma_buf_attachment *attach)
{
	struct hyper_dmabuf_imported_sgt_info *sgt_info;
	int ret;

	if (!attach->dmabuf->priv)
		return;

	sgt_info = (struct hyper_dmabuf_imported_sgt_info *)attach->dmabuf->priv;

	ret = hyper_dmabuf_sync_request_and_wait(HYPER_DMABUF_ID_IMPORTER_GET_SDOMAIN_ID(sgt_info->hyper_dmabuf_id),
						HYPER_DMABUF_OPS_DETACH);

	if (ret < 0) {
		printk("hyper_dmabuf::%s Error:send dmabuf sync request failed\n", __func__);
	}
}

static struct sg_table* hyper_dmabuf_ops_map(struct dma_buf_attachment *attachment,
						enum dma_data_direction dir)
{
	struct sg_table *st;
	struct hyper_dmabuf_imported_sgt_info *sgt_info;
	struct hyper_dmabuf_pages_info *page_info;
	int ret;

	if (!attachment->dmabuf->priv)
		return NULL;

	sgt_info = (struct hyper_dmabuf_imported_sgt_info *)attachment->dmabuf->priv;

	/* extract pages from sgt */
	page_info = hyper_dmabuf_ext_pgs(sgt_info->sgt);

	/* create a new sg_table with extracted pages */
	st = hyper_dmabuf_create_sgt(page_info->pages, page_info->frst_ofst,
				page_info->last_len, page_info->nents);
	if (st == NULL)
		goto err_free_sg;

        if (!dma_map_sg(attachment->dev, st->sgl, st->nents, dir)) {
                goto err_free_sg;
        }

	ret = hyper_dmabuf_sync_request_and_wait(HYPER_DMABUF_ID_IMPORTER_GET_SDOMAIN_ID(sgt_info->hyper_dmabuf_id),
						HYPER_DMABUF_OPS_MAP);

	if (ret < 0) {
		printk("hyper_dmabuf::%s Error:send dmabuf sync request failed\n", __func__);
	}

	return st;

err_free_sg:
	sg_free_table(st);
	kfree(st);
	return NULL;
}

static void hyper_dmabuf_ops_unmap(struct dma_buf_attachment *attachment,
						struct sg_table *sg,
						enum dma_data_direction dir)
{
	struct hyper_dmabuf_imported_sgt_info *sgt_info;
	int ret;

	if (!attachment->dmabuf->priv)
		return;

	sgt_info = (struct hyper_dmabuf_imported_sgt_info *)attachment->dmabuf->priv;

	dma_unmap_sg(attachment->dev, sg->sgl, sg->nents, dir);

	sg_free_table(sg);
	kfree(sg);

	ret = hyper_dmabuf_sync_request_and_wait(HYPER_DMABUF_ID_IMPORTER_GET_SDOMAIN_ID(sgt_info->hyper_dmabuf_id),
						HYPER_DMABUF_OPS_UNMAP);

	if (ret < 0) {
		printk("hyper_dmabuf::%s Error:send dmabuf sync request failed\n", __func__);
	}
}

static void hyper_dmabuf_ops_release(struct dma_buf *dmabuf)
{
	struct hyper_dmabuf_imported_sgt_info *sgt_info;
	int ret;

	if (!dmabuf->priv)
		return;

	sgt_info = (struct hyper_dmabuf_imported_sgt_info *)dmabuf->priv;

	ret = hyper_dmabuf_sync_request_and_wait(HYPER_DMABUF_ID_IMPORTER_GET_SDOMAIN_ID(sgt_info->hyper_dmabuf_id),
						HYPER_DMABUF_OPS_RELEASE);

	if (ret < 0) {
		printk("hyper_dmabuf::%s Error:send dmabuf sync request failed\n", __func__);
	}
}

static int hyper_dmabuf_ops_begin_cpu_access(struct dma_buf *dmabuf, enum dma_data_direction dir)
{
	struct hyper_dmabuf_imported_sgt_info *sgt_info;
	int ret;

	if (!dmabuf->priv)
		return -EINVAL;

	sgt_info = (struct hyper_dmabuf_imported_sgt_info *)dmabuf->priv;

	ret = hyper_dmabuf_sync_request_and_wait(HYPER_DMABUF_ID_IMPORTER_GET_SDOMAIN_ID(sgt_info->hyper_dmabuf_id),
						HYPER_DMABUF_OPS_BEGIN_CPU_ACCESS);
	if (ret < 0) {
		printk("hyper_dmabuf::%s Error:send dmabuf sync request failed\n", __func__);
	}

	return ret;
}

static int hyper_dmabuf_ops_end_cpu_access(struct dma_buf *dmabuf, enum dma_data_direction dir)
{
	struct hyper_dmabuf_imported_sgt_info *sgt_info;
	int ret;

	if (!dmabuf->priv)
		return -EINVAL;

	sgt_info = (struct hyper_dmabuf_imported_sgt_info *)dmabuf->priv;

	ret = hyper_dmabuf_sync_request_and_wait(HYPER_DMABUF_ID_IMPORTER_GET_SDOMAIN_ID(sgt_info->hyper_dmabuf_id),
						HYPER_DMABUF_OPS_END_CPU_ACCESS);
	if (ret < 0) {
		printk("hyper_dmabuf::%s Error:send dmabuf sync request failed\n", __func__);
	}

	return 0;
}

static void *hyper_dmabuf_ops_kmap_atomic(struct dma_buf *dmabuf, unsigned long pgnum)
{
	struct hyper_dmabuf_imported_sgt_info *sgt_info;
	int ret;

	if (!dmabuf->priv)
		return NULL;

	sgt_info = (struct hyper_dmabuf_imported_sgt_info *)dmabuf->priv;

	ret = hyper_dmabuf_sync_request_and_wait(HYPER_DMABUF_ID_IMPORTER_GET_SDOMAIN_ID(sgt_info->hyper_dmabuf_id),
						HYPER_DMABUF_OPS_KMAP_ATOMIC);
	if (ret < 0) {
		printk("hyper_dmabuf::%s Error:send dmabuf sync request failed\n", __func__);
	}

	return NULL; /* for now NULL.. need to return the address of mapped region */
}

static void hyper_dmabuf_ops_kunmap_atomic(struct dma_buf *dmabuf, unsigned long pgnum, void *vaddr)
{
	struct hyper_dmabuf_imported_sgt_info *sgt_info;
	int ret;

	if (!dmabuf->priv)
		return;

	sgt_info = (struct hyper_dmabuf_imported_sgt_info *)dmabuf->priv;

	ret = hyper_dmabuf_sync_request_and_wait(HYPER_DMABUF_ID_IMPORTER_GET_SDOMAIN_ID(sgt_info->hyper_dmabuf_id),
						HYPER_DMABUF_OPS_KUNMAP_ATOMIC);
	if (ret < 0) {
		printk("hyper_dmabuf::%s Error:send dmabuf sync request failed\n", __func__);
	}
}

static void *hyper_dmabuf_ops_kmap(struct dma_buf *dmabuf, unsigned long pgnum)
{
	struct hyper_dmabuf_imported_sgt_info *sgt_info;
	int ret;

	if (!dmabuf->priv)
		return NULL;

	sgt_info = (struct hyper_dmabuf_imported_sgt_info *)dmabuf->priv;

	ret = hyper_dmabuf_sync_request_and_wait(HYPER_DMABUF_ID_IMPORTER_GET_SDOMAIN_ID(sgt_info->hyper_dmabuf_id),
						HYPER_DMABUF_OPS_KMAP);
	if (ret < 0) {
		printk("hyper_dmabuf::%s Error:send dmabuf sync request failed\n", __func__);
	}

	return NULL; /* for now NULL.. need to return the address of mapped region */
}

static void hyper_dmabuf_ops_kunmap(struct dma_buf *dmabuf, unsigned long pgnum, void *vaddr)
{
	struct hyper_dmabuf_imported_sgt_info *sgt_info;
	int ret;

	if (!dmabuf->priv)
		return;

	sgt_info = (struct hyper_dmabuf_imported_sgt_info *)dmabuf->priv;

	ret = hyper_dmabuf_sync_request_and_wait(HYPER_DMABUF_ID_IMPORTER_GET_SDOMAIN_ID(sgt_info->hyper_dmabuf_id),
						HYPER_DMABUF_OPS_KUNMAP);
	if (ret < 0) {
		printk("hyper_dmabuf::%s Error:send dmabuf sync request failed\n", __func__);
	}
}

static int hyper_dmabuf_ops_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct hyper_dmabuf_imported_sgt_info *sgt_info;
	int ret;

	if (!dmabuf->priv)
		return -EINVAL;

	sgt_info = (struct hyper_dmabuf_imported_sgt_info *)dmabuf->priv;

	ret = hyper_dmabuf_sync_request_and_wait(HYPER_DMABUF_ID_IMPORTER_GET_SDOMAIN_ID(sgt_info->hyper_dmabuf_id),
						HYPER_DMABUF_OPS_MMAP);
	if (ret < 0) {
		printk("hyper_dmabuf::%s Error:send dmabuf sync request failed\n", __func__);
	}

	return ret;
}

static void *hyper_dmabuf_ops_vmap(struct dma_buf *dmabuf)
{
	struct hyper_dmabuf_imported_sgt_info *sgt_info;
	int ret;

	if (!dmabuf->priv)
		return NULL;

	sgt_info = (struct hyper_dmabuf_imported_sgt_info *)dmabuf->priv;

	ret = hyper_dmabuf_sync_request_and_wait(HYPER_DMABUF_ID_IMPORTER_GET_SDOMAIN_ID(sgt_info->hyper_dmabuf_id),
						HYPER_DMABUF_OPS_VMAP);
	if (ret < 0) {
		printk("hyper_dmabuf::%s Error:send dmabuf sync request failed\n", __func__);
	}

	return NULL;
}

static void hyper_dmabuf_ops_vunmap(struct dma_buf *dmabuf, void *vaddr)
{
	struct hyper_dmabuf_imported_sgt_info *sgt_info;
	int ret;

	if (!dmabuf->priv)
		return;

	sgt_info = (struct hyper_dmabuf_imported_sgt_info *)dmabuf->priv;

	ret = hyper_dmabuf_sync_request_and_wait(HYPER_DMABUF_ID_IMPORTER_GET_SDOMAIN_ID(sgt_info->hyper_dmabuf_id),
						HYPER_DMABUF_OPS_VUNMAP);
	if (ret < 0) {
		printk("hyper_dmabuf::%s Error:send dmabuf sync request failed\n", __func__);
	}
}

static const struct dma_buf_ops hyper_dmabuf_ops = {
		.attach = hyper_dmabuf_ops_attach,
		.detach = hyper_dmabuf_ops_detach,
		.map_dma_buf = hyper_dmabuf_ops_map,
		.unmap_dma_buf = hyper_dmabuf_ops_unmap,
		.release = hyper_dmabuf_ops_release,
		.begin_cpu_access = (void*)hyper_dmabuf_ops_begin_cpu_access,
		.end_cpu_access = (void*)hyper_dmabuf_ops_end_cpu_access,
		.map_atomic = hyper_dmabuf_ops_kmap_atomic,
		.unmap_atomic = hyper_dmabuf_ops_kunmap_atomic,
		.map = hyper_dmabuf_ops_kmap,
		.unmap = hyper_dmabuf_ops_kunmap,
		.mmap = hyper_dmabuf_ops_mmap,
		.vmap = hyper_dmabuf_ops_vmap,
		.vunmap = hyper_dmabuf_ops_vunmap,
};

/* exporting dmabuf as fd */
int hyper_dmabuf_export_fd(struct hyper_dmabuf_imported_sgt_info *dinfo, int flags)
{
	int fd;

	struct dma_buf* dmabuf;

/* call hyper_dmabuf_export_dmabuf and create and bind a handle for it
 * then release */

	dmabuf = hyper_dmabuf_export_dma_buf(dinfo);

	fd = dma_buf_fd(dmabuf, flags);

	return fd;
}

struct dma_buf* hyper_dmabuf_export_dma_buf(struct hyper_dmabuf_imported_sgt_info *dinfo)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);

	exp_info.ops = &hyper_dmabuf_ops;
	exp_info.size = dinfo->sgt->nents * PAGE_SIZE; /* multiple of PAGE_SIZE, not considering offset */
	exp_info.flags = /* not sure about flag */0;
	exp_info.priv = dinfo;

	return dma_buf_export(&exp_info);
};
