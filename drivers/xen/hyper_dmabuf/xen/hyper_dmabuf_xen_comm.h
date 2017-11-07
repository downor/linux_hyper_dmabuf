#ifndef __HYPER_DMABUF_XEN_COMM_H__
#define __HYPER_DMABUF_XEN_COMM_H__

#include "xen/interface/io/ring.h"
#include "xen/xenbus.h"
#include "../hyper_dmabuf_msg.h"

#define MAX_NUMBER_OF_OPERANDS 9

DEFINE_RING_TYPES(xen_comm, struct hyper_dmabuf_req, struct hyper_dmabuf_resp);

struct xen_comm_tx_ring_info {
        struct xen_comm_front_ring ring_front;
	int rdomain;
        int gref_ring;
        int irq;
        int port;
	struct xenbus_watch watch;
};

struct xen_comm_rx_ring_info {
        int sdomain;
        int irq;
        int evtchn;
        struct xen_comm_back_ring ring_back;
	struct gnttab_unmap_grant_ref unmap_op;
};

int hyper_dmabuf_get_domid(void);

int hyper_dmabuf_xen_init_comm_env(void);

/* exporter needs to generated info for page sharing */
int hyper_dmabuf_xen_init_tx_rbuf(int domid);

/* importer needs to know about shared page and port numbers
 * for ring buffer and event channel
 */
int hyper_dmabuf_xen_init_rx_rbuf(int domid);

/* cleans up exporter ring created for given domain */
void hyper_dmabuf_xen_cleanup_tx_rbuf(int domid);

/* cleans up importer ring created for given domain */
void hyper_dmabuf_xen_cleanup_rx_rbuf(int domid);

void hyper_dmabuf_xen_destroy_comm(void);

/* send request to the remote domain */
int hyper_dmabuf_xen_send_req(int domid, struct hyper_dmabuf_req *req, int wait);

#endif // __HYPER_DMABUF_XEN_COMM_H__
