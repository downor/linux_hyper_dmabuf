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
#include "hyper_dmabuf_drv.h"
#include "hyper_dmabuf_struct.h"
#include "hyper_dmabuf_id.h"

extern struct hyper_dmabuf_private hyper_dmabuf_private;

#define HYPER_DMABUF_SIZE(nents, first_offset, last_len) \
	((nents)*PAGE_SIZE - (first_offset) - PAGE_SIZE + (last_len))

int hyper_dmabuf_query_exported(struct hyper_dmabuf_sgt_info *sgt_info, int query)
{
	switch (query)
	{
		case HYPER_DMABUF_QUERY_TYPE:
			return EXPORTED;

		/* exporting domain of this specific dmabuf*/
		case HYPER_DMABUF_QUERY_EXPORTER:
			return HYPER_DMABUF_DOM_ID(sgt_info->hid);

		/* importing domain of this specific dmabuf */
		case HYPER_DMABUF_QUERY_IMPORTER:
			return sgt_info->hyper_dmabuf_rdomain;

		/* size of dmabuf in byte */
		case HYPER_DMABUF_QUERY_SIZE:
			return sgt_info->dma_buf->size;

		/* whether the buffer is used by importer */
		case HYPER_DMABUF_QUERY_BUSY:
			return (sgt_info->importer_exported == 0) ? false : true;

		/* whether the buffer is unexported */
		case HYPER_DMABUF_QUERY_UNEXPORTED:
			return !sgt_info->valid;

		/* whether the buffer is scheduled to be unexported */
		case HYPER_DMABUF_QUERY_DELAYED_UNEXPORTED:
			return !sgt_info->unexport_scheduled;
	}

	return -EINVAL;
}


int hyper_dmabuf_query_imported(struct hyper_dmabuf_imported_sgt_info *imported_sgt_info, int query)
{
	switch (query)
	{
		case HYPER_DMABUF_QUERY_TYPE:
			return IMPORTED;

		/* exporting domain of this specific dmabuf*/
		case HYPER_DMABUF_QUERY_EXPORTER:
			return HYPER_DMABUF_DOM_ID(imported_sgt_info->hid);

		/* importing domain of this specific dmabuf */
		case HYPER_DMABUF_QUERY_IMPORTER:
			return  hyper_dmabuf_private.domid;

		/* size of dmabuf in byte */
		case HYPER_DMABUF_QUERY_SIZE:
			if (imported_sgt_info->dma_buf) {
				/* if local dma_buf is created (if it's ever mapped),
				 * retrieve it directly from struct dma_buf *
				 */
				return imported_sgt_info->dma_buf->size;
			} else {
				/* calcuate it from given nents, frst_ofst and last_len */
				return HYPER_DMABUF_SIZE(imported_sgt_info->nents,
							 imported_sgt_info->frst_ofst,
							 imported_sgt_info->last_len);
			}

		/* whether the buffer is used or not */
		case HYPER_DMABUF_QUERY_BUSY:
			/* checks if it's used by importer */
			return (imported_sgt_info->num_importers > 0) ? true : false;

		/* whether the buffer is unexported */
		case HYPER_DMABUF_QUERY_UNEXPORTED:
			return !imported_sgt_info->valid;
	}

	return -EINVAL;
}
