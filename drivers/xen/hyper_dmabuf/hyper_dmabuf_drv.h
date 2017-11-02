#ifndef __LINUX_PUBLIC_HYPER_DMABUF_DRV_H__
#define __LINUX_PUBLIC_HYPER_DMABUF_DRV_H__

typedef int (*hyper_dmabuf_ioctl_t)(void *data);

struct hyper_dmabuf_ioctl_desc {
	unsigned int cmd;
	int flags;
	hyper_dmabuf_ioctl_t func;
	const char *name;
};

#define HYPER_DMABUF_IOCTL_DEF(ioctl, _func, _flags) 	\
	[_IOC_NR(ioctl)] = {				\
			.cmd = ioctl,			\
			.func = _func,			\
			.flags = _flags,		\
			.name = #ioctl			\
	}

#define IOCTL_HYPER_DMABUF_EXPORTER_RING_SETUP \
_IOC(_IOC_NONE, 'G', 0, sizeof(struct ioctl_hyper_dmabuf_exporter_ring_setup))
struct ioctl_hyper_dmabuf_exporter_ring_setup {
	/* IN parameters */
	/* Remote domain id */
	uint32_t remote_domain;
	grant_ref_t ring_refid; /* assigned by driver, copied to userspace after initialization */
	uint32_t port; /* assigned by driver, copied to userspace after initialization */
};

#define IOCTL_HYPER_DMABUF_IMPORTER_RING_SETUP \
_IOC(_IOC_NONE, 'G', 1, sizeof(struct ioctl_hyper_dmabuf_importer_ring_setup))
struct ioctl_hyper_dmabuf_importer_ring_setup {
	/* IN parameters */
	/* Source domain id */
	uint32_t source_domain;
	/* Ring shared page refid */
	grant_ref_t ring_refid;
	/* Port number */
	uint32_t port;
};

#define IOCTL_HYPER_DMABUF_EXPORT_REMOTE \
_IOC(_IOC_NONE, 'G', 2, sizeof(struct ioctl_hyper_dmabuf_export_remote))
struct ioctl_hyper_dmabuf_export_remote {
	/* IN parameters */
	/* DMA buf fd to be exported */
	uint32_t dmabuf_fd;
	/* Domain id to which buffer should be exported */
	uint32_t remote_domain;
	/* exported dma buf id */
	uint32_t hyper_dmabuf_id;
	uint32_t private[4];
};

#define IOCTL_HYPER_DMABUF_EXPORT_FD \
_IOC(_IOC_NONE, 'G', 3, sizeof(struct ioctl_hyper_dmabuf_export_fd))
struct ioctl_hyper_dmabuf_export_fd {
	/* IN parameters */
	/* hyper dmabuf id to be imported */
	uint32_t hyper_dmabuf_id;
	/* flags */
	uint32_t flags;
	/* OUT parameters */
	/* exported dma buf fd */
	uint32_t fd;
};

#define IOCTL_HYPER_DMABUF_DESTROY \
_IOC(_IOC_NONE, 'G', 4, sizeof(struct ioctl_hyper_dmabuf_destroy))
struct ioctl_hyper_dmabuf_destroy {
	/* IN parameters */
	/* hyper dmabuf id to be destroyed */
	uint32_t hyper_dmabuf_id;
	/* OUT parameters */
	/* Status of request */
	uint32_t status;
};

#define IOCTL_HYPER_DMABUF_QUERY \
_IOC(_IOC_NONE, 'G', 5, sizeof(struct ioctl_hyper_dmabuf_query))
struct ioctl_hyper_dmabuf_query {
	/* in parameters */
	/* hyper dmabuf id to be queried */
	uint32_t hyper_dmabuf_id;
	/* item to be queried */
	uint32_t item;
	/* OUT parameters */
	/* Value of queried item */
	uint32_t info;
};

#define IOCTL_HYPER_DMABUF_REMOTE_EXPORTER_RING_SETUP \
_IOC(_IOC_NONE, 'G', 6, sizeof(struct ioctl_hyper_dmabuf_remote_exporter_ring_setup))
struct ioctl_hyper_dmabuf_remote_exporter_ring_setup {
	/* in parameters */
	uint32_t rdomain; /* id of remote domain where exporter's ring need to be setup */
	uint32_t info;
};

#endif //__LINUX_PUBLIC_HYPER_DMABUF_DRV_H__
