#ifndef __HYPER_DMABUF_ID_H__
#define __HYPER_DMABUF_ID_H__

/* Importer combine source domain id with given hyper_dmabuf_id
 * to make it unique in case there are multiple exporters */

#define HYPER_DMABUF_ID_CREATE(domid, id) \
	((((domid) & 0xFF) << 24) | ((id) & 0xFFFFFF))

#define HYPER_DMABUF_DOM_ID(id) \
	(((id) >> 24) & 0xFF)

/* currently maximum number of buffers shared
 * at any given moment is limited to 1000
 */
#define HYPER_DMABUF_ID_MAX 1000

void store_reusable_id(int id);

void destroy_reusable_list(void);

int hyper_dmabuf_get_id(void);

#endif /*__HYPER_DMABUF_ID_H*/
