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
 * Authors:
 *    Dongwon Kim <dongwon.kim@intel.com>
 *    Mateusz Polrola <mateuszx.potrola@intel.com>
 *
 */

#include <linux/dma-buf.h>
#include <linux/uaccess.h>
#include "hyper_dmabuf_drv.h"
#include "hyper_dmabuf_struct.h"
#include "hyper_dmabuf_id.h"

extern struct hyper_dmabuf_private hyper_dmabuf_private;

#define HYPER_DMABUF_SIZE(nents, first_offset, last_len) \
	((nents)*PAGE_SIZE - (first_offset) - PAGE_SIZE + (last_len))

int hyper_dmabuf_query_exported(struct hyper_dmabuf_sgt_info *sgt_info,
				int query, unsigned long* info)
{
	int n;

	switch (query)
	{
		case HYPER_DMABUF_QUERY_TYPE:
			*info = EXPORTED;
			break;

		/* exporting domain of this specific dmabuf*/
		case HYPER_DMABUF_QUERY_EXPORTER:
			*info = HYPER_DMABUF_DOM_ID(sgt_info->hid);
			break;

		/* importing domain of this specific dmabuf */
		case HYPER_DMABUF_QUERY_IMPORTER:
			*info = sgt_info->hyper_dmabuf_rdomain;
			break;

		/* size of dmabuf in byte */
		case HYPER_DMABUF_QUERY_SIZE:
			*info = sgt_info->dma_buf->size;
			break;

		/* whether the buffer is used by importer */
		case HYPER_DMABUF_QUERY_BUSY:
			*info = (sgt_info->importer_exported == 0) ? false : true;
			break;

		/* whether the buffer is unexported */
		case HYPER_DMABUF_QUERY_UNEXPORTED:
			*info = !sgt_info->valid;
			break;

		/* whether the buffer is scheduled to be unexported */
		case HYPER_DMABUF_QUERY_DELAYED_UNEXPORTED:
			*info = !sgt_info->unexport_scheduled;
			break;

		/* size of private info attached to buffer */
		case HYPER_DMABUF_QUERY_PRIV_INFO_SIZE:
			*info = sgt_info->sz_priv;
			break;

		/* copy private info attached to buffer */
		case HYPER_DMABUF_QUERY_PRIV_INFO:
			if (sgt_info->sz_priv > 0) {
				n = copy_to_user((void __user*) *info,
						sgt_info->priv,
						sgt_info->sz_priv);
				if (n != 0)
					return -EINVAL;
			}
			break;

		default:
			return -EINVAL;
	}

	return 0;
}


int hyper_dmabuf_query_imported(struct hyper_dmabuf_imported_sgt_info *imported_sgt_info,
				int query, unsigned long *info)
{
	int n;

	switch (query)
	{
		case HYPER_DMABUF_QUERY_TYPE:
			*info = IMPORTED;
			break;

		/* exporting domain of this specific dmabuf*/
		case HYPER_DMABUF_QUERY_EXPORTER:
			*info = HYPER_DMABUF_DOM_ID(imported_sgt_info->hid);
			break;

		/* importing domain of this specific dmabuf */
		case HYPER_DMABUF_QUERY_IMPORTER:
			*info = hyper_dmabuf_private.domid;
			break;

		/* size of dmabuf in byte */
		case HYPER_DMABUF_QUERY_SIZE:
			if (imported_sgt_info->dma_buf) {
				/* if local dma_buf is created (if it's ever mapped),
				 * retrieve it directly from struct dma_buf *
				 */
				*info = imported_sgt_info->dma_buf->size;
			} else {
				/* calcuate it from given nents, frst_ofst and last_len */
				*info = HYPER_DMABUF_SIZE(imported_sgt_info->nents,
							  imported_sgt_info->frst_ofst,
							  imported_sgt_info->last_len);
			}
			break;

		/* whether the buffer is used or not */
		case HYPER_DMABUF_QUERY_BUSY:
			/* checks if it's used by importer */
			*info = (imported_sgt_info->num_importers > 0) ? true : false;
			break;

		/* whether the buffer is unexported */
		case HYPER_DMABUF_QUERY_UNEXPORTED:
			*info = !imported_sgt_info->valid;
			break;
		/* size of private info attached to buffer */
		case HYPER_DMABUF_QUERY_PRIV_INFO_SIZE:
			*info = imported_sgt_info->sz_priv;
			break;

		/* copy private info attached to buffer */
		case HYPER_DMABUF_QUERY_PRIV_INFO:
			if (imported_sgt_info->sz_priv > 0) {
				n = copy_to_user((void __user*) *info,
						imported_sgt_info->priv,
						imported_sgt_info->sz_priv);
				if (n != 0)
					return -EINVAL;
			}
			break;

		default:
			return -EINVAL;
	}

	return 0;
}
