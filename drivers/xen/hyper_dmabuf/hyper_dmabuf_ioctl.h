/*
 * Copyright © 2017 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#ifndef __LINUX_PUBLIC_HYPER_DMABUF_IOCTL_H__
#define __LINUX_PUBLIC_HYPER_DMABUF_IOCTL_H__

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

#define IOCTL_HYPER_DMABUF_TX_CH_SETUP \
_IOC(_IOC_NONE, 'G', 0, sizeof(struct ioctl_hyper_dmabuf_tx_ch_setup))
struct ioctl_hyper_dmabuf_tx_ch_setup {
	/* IN parameters */
	/* Remote domain id */
	int remote_domain;
};

#define IOCTL_HYPER_DMABUF_RX_CH_SETUP \
_IOC(_IOC_NONE, 'G', 1, sizeof(struct ioctl_hyper_dmabuf_rx_ch_setup))
struct ioctl_hyper_dmabuf_rx_ch_setup {
	/* IN parameters */
	/* Source domain id */
	int source_domain;
};

#define IOCTL_HYPER_DMABUF_EXPORT_REMOTE \
_IOC(_IOC_NONE, 'G', 2, sizeof(struct ioctl_hyper_dmabuf_export_remote))
struct ioctl_hyper_dmabuf_export_remote {
	/* IN parameters */
	/* DMA buf fd to be exported */
	int dmabuf_fd;
	/* Domain id to which buffer should be exported */
	int remote_domain;
	/* exported dma buf id */
	int hyper_dmabuf_id;
	int private[4];
};

#define IOCTL_HYPER_DMABUF_EXPORT_FD \
_IOC(_IOC_NONE, 'G', 3, sizeof(struct ioctl_hyper_dmabuf_export_fd))
struct ioctl_hyper_dmabuf_export_fd {
	/* IN parameters */
	/* hyper dmabuf id to be imported */
	int hyper_dmabuf_id;
	/* flags */
	int flags;
	/* OUT parameters */
	/* exported dma buf fd */
	int fd;
};

#define IOCTL_HYPER_DMABUF_UNEXPORT \
_IOC(_IOC_NONE, 'G', 4, sizeof(struct ioctl_hyper_dmabuf_unexport))
struct ioctl_hyper_dmabuf_unexport {
	/* IN parameters */
	/* hyper dmabuf id to be unexported */
	int hyper_dmabuf_id;
	/* OUT parameters */
	/* Status of request */
	int status;
};

#define IOCTL_HYPER_DMABUF_QUERY \
_IOC(_IOC_NONE, 'G', 5, sizeof(struct ioctl_hyper_dmabuf_query))
struct ioctl_hyper_dmabuf_query {
	/* in parameters */
	/* hyper dmabuf id to be queried */
	int hyper_dmabuf_id;
	/* item to be queried */
	int item;
	/* OUT parameters */
	/* Value of queried item */
	int info;
};

#endif //__LINUX_PUBLIC_HYPER_DMABUF_DRV_H__
