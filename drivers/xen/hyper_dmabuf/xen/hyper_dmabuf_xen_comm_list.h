#ifndef __HYPER_DMABUF_XEN_COMM_LIST_H__
#define __HYPER_DMABUF_XEN_COMM_LIST_H__

/* number of bits to be used for exported dmabufs hash table */
#define MAX_ENTRY_TX_RING 7
/* number of bits to be used for imported dmabufs hash table */
#define MAX_ENTRY_RX_RING 7

struct xen_comm_tx_ring_info_entry {
        struct xen_comm_tx_ring_info *info;
        struct hlist_node node;
};

struct xen_comm_rx_ring_info_entry {
        struct xen_comm_rx_ring_info *info;
        struct hlist_node node;
};

void xen_comm_ring_table_init(void);

int xen_comm_add_tx_ring(struct xen_comm_tx_ring_info *ring_info);

int xen_comm_add_rx_ring(struct xen_comm_rx_ring_info *ring_info);

int xen_comm_remove_tx_ring(int domid);

int xen_comm_remove_rx_ring(int domid);

struct xen_comm_tx_ring_info *xen_comm_find_tx_ring(int domid);

struct xen_comm_rx_ring_info *xen_comm_find_rx_ring(int domid);

/* iterates over all exporter rings and calls provided function for each of them */
void xen_comm_foreach_tx_ring(void (*func)(int domid));

/* iterates over all importer rings and calls provided function for each of them */
void xen_comm_foreach_rx_ring(void (*func)(int domid));

#endif // __HYPER_DMABUF_XEN_COMM_LIST_H__
