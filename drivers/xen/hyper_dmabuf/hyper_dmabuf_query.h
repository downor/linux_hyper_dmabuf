#ifndef __HYPER_DMABUF_QUERY_H__
#define __HYPER_DMABUF_QUERY_H__

enum hyper_dmabuf_query {
	DMABUF_QUERY_TYPE_LIST = 0x10,
	DMABUF_QUERY_EXPORTER,
	DMABUF_QUERY_IMPORTER,
	DMABUF_QUERY_SIZE
};

enum hyper_dmabuf_status {
	EXPORTED = 0x01,
	IMPORTED
};

#endif /* __HYPER_DMABUF_QUERY_H__ */
