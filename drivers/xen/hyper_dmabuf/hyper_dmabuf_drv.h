#ifndef __LINUX_PUBLIC_HYPER_DMABUF_DRV_H__
#define __LINUX_PUBLIC_HYPER_DMABUF_DRV_H__

struct list_reusable_id {
	int id;
	struct list_head list;
};

struct hyper_dmabuf_backend_ops {
	/* retreiving id of current virtual machine */
	int (*get_vm_id)(void);

	/* get pages shared via hypervisor-specific method */
	int (*share_pages)(struct page **, int, int, void **);

	/* make shared pages unshared via hypervisor specific method */
	int (*unshare_pages)(void **, int);

	/* map remotely shared pages on importer's side via
	 * hypervisor-specific method
	 */
	struct page ** (*map_shared_pages)(int, int, int, void **);

	/* unmap and free shared pages on importer's side via
	 * hypervisor-specific method
	 */
	int (*unmap_shared_pages)(void **, int);

	/* initialize communication environment */
	int (*init_comm_env)(void);

	void (*destroy_comm)(void);

	/* upstream ch setup (receiving and responding) */
	int (*init_rx_ch)(int);

	/* downstream ch setup (transmitting and parsing responses) */
	int (*init_tx_ch)(int);

	int (*send_req)(int, struct hyper_dmabuf_req *, int);
};

struct hyper_dmabuf_private {
        struct device *device;
	int domid;
	struct workqueue_struct *work_queue;
	struct list_reusable_id *id_queue;

	/* backend ops - hypervisor specific */
	struct hyper_dmabuf_backend_ops *backend_ops;
};

#endif /* __LINUX_PUBLIC_HYPER_DMABUF_DRV_H__ */
