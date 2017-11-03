#ifndef __HYPER_DMABUF_XEN_COMM_H__
#define __HYPER_DMABUF_XEN_COMM_H__

#include "xen/interface/io/ring.h"
#include "xen/xenbus.h"

#define MAX_NUMBER_OF_OPERANDS 9

struct hyper_dmabuf_ring_rq {
        unsigned int request_id;
        unsigned int status;
        unsigned int command;
        unsigned int operands[MAX_NUMBER_OF_OPERANDS];
};

struct hyper_dmabuf_ring_rp {
        unsigned int response_id;
        unsigned int status;
        unsigned int command;
        unsigned int operands[MAX_NUMBER_OF_OPERANDS];
};

DEFINE_RING_TYPES(hyper_dmabuf, struct hyper_dmabuf_ring_rq, struct hyper_dmabuf_ring_rp);

struct hyper_dmabuf_ring_info_export {
        struct hyper_dmabuf_front_ring ring_front;
	int rdomain;
        int gref_ring;
        int irq;
        int port;
	struct xenbus_watch watch;
};

struct hyper_dmabuf_ring_info_import {
        int sdomain;
        int irq;
        int evtchn;
        struct hyper_dmabuf_back_ring ring_back;
	struct gnttab_unmap_grant_ref unmap_op;
};

int32_t hyper_dmabuf_get_domid(void);
int32_t hyper_dmabuf_setup_data_dir(void);
int32_t hyper_dmabuf_destroy_data_dir(void);

int hyper_dmabuf_next_req_id_export(void);

/* exporter needs to generated info for page sharing */
int hyper_dmabuf_exporter_ringbuf_init(int rdomain);

/* importer needs to know about shared page and port numbers for ring buffer and event channel */
int hyper_dmabuf_importer_ringbuf_init(int sdomain);

/* cleans up exporter ring created for given domain */
void hyper_dmabuf_exporter_ringbuf_cleanup(int rdomain);

/* cleans up importer ring created for given domain */
void hyper_dmabuf_importer_ringbuf_cleanup(int sdomain);

/* cleans up all exporter/importer rings */
void hyper_dmabuf_cleanup_ringbufs(void);

/* send request to the remote domain */
int hyper_dmabuf_send_request(int domain, struct hyper_dmabuf_ring_rq *req);

/* called by interrupt (WORKQUEUE) */
int hyper_dmabuf_send_response(struct hyper_dmabuf_ring_rp* response, int domain);

#endif // __HYPER_DMABUF_XEN_COMM_H__
