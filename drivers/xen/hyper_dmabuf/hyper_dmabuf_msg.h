#ifndef __HYPER_DMABUF_MSG_H__
#define __HYPER_DMABUF_MSG_H__

enum hyper_dmabuf_command {
	HYPER_DMABUF_EXPORT = 0x10,
	HYPER_DMABUF_DESTROY,
	HYPER_DMABUF_DESTROY_FINISH,
	HYPER_DMABUF_OPS_TO_REMOTE,
	HYPER_DMABUF_OPS_TO_SOURCE,
};

enum hyper_dmabuf_ops {
	HYPER_DMABUF_OPS_ATTACH = 0x1000,
	HYPER_DMABUF_OPS_DETACH,
	HYPER_DMABUF_OPS_MAP,
	HYPER_DMABUF_OPS_UNMAP,
	HYPER_DMABUF_OPS_RELEASE,
	HYPER_DMABUF_OPS_BEGIN_CPU_ACCESS,
	HYPER_DMABUF_OPS_END_CPU_ACCESS,
	HYPER_DMABUF_OPS_KMAP_ATOMIC,
	HYPER_DMABUF_OPS_KUNMAP_ATOMIC,
	HYPER_DMABUF_OPS_KMAP,
	HYPER_DMABUF_OPS_KUNMAP,
	HYPER_DMABUF_OPS_MMAP,
	HYPER_DMABUF_OPS_VMAP,
	HYPER_DMABUF_OPS_VUNMAP,
};

enum hyper_dmabuf_req_feedback {
	HYPER_DMABUF_REQ_PROCESSED = 0x100,
	HYPER_DMABUF_REQ_NEEDS_FOLLOW_UP,
	HYPER_DMABUF_REQ_ERROR,
	HYPER_DMABUF_REQ_NOT_RESPONDED
};

/* create a request packet with given command and operands */
void hyper_dmabuf_create_request(struct hyper_dmabuf_ring_rq *request,
                                        enum hyper_dmabuf_command command, int *operands);

/* parse incoming request packet (or response) and take appropriate actions for those */
int hyper_dmabuf_msg_parse(int domid, struct hyper_dmabuf_ring_rq *req);

#endif // __HYPER_DMABUF_MSG_H__
