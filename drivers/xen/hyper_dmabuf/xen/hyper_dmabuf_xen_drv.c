#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <xen/grant_table.h>
#include "../hyper_dmabuf_msg.h"
#include "../hyper_dmabuf_drv.h"
#include "hyper_dmabuf_xen_drv.h"
#include "hyper_dmabuf_xen_comm.h"
#include "hyper_dmabuf_xen_shm.h"

struct hyper_dmabuf_backend_ops xen_backend_ops = {
	.get_vm_id = hyper_dmabuf_get_domid,
	.share_pages = hyper_dmabuf_xen_share_pages,
	.unshare_pages = hyper_dmabuf_xen_unshare_pages,
	.map_shared_pages = (void *)hyper_dmabuf_xen_map_shared_pages,
	.unmap_shared_pages = hyper_dmabuf_xen_unmap_shared_pages,
	.init_comm_env = hyper_dmabuf_xen_init_comm_env,
	.destroy_comm = hyper_dmabuf_xen_destroy_comm,
	.init_rx_ch = hyper_dmabuf_xen_init_rx_rbuf,
	.init_tx_ch = hyper_dmabuf_xen_init_tx_rbuf,
	.send_req = hyper_dmabuf_xen_send_req,
};
