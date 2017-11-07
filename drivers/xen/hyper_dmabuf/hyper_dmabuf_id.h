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

#ifndef __HYPER_DMABUF_ID_H__
#define __HYPER_DMABUF_ID_H__

/* Importer combine source domain id with given hyper_dmabuf_id
 * to make it unique in case there are multiple exporters */

#define HYPER_DMABUF_ID_CREATE(domid, id) \
	((((domid) & 0xFF) << 24) | ((id) & 0xFFFFFF))

#define HYPER_DMABUF_DOM_ID(id) \
	(((id) >> 24) & 0xFF)

/* currently maximum number of buffers shared
 * at any given moment is limited to 1000
 */
#define HYPER_DMABUF_ID_MAX 1000

void store_reusable_id(int id);

void destroy_reusable_list(void);

int hyper_dmabuf_get_id(void);

#endif /*__HYPER_DMABUF_ID_H*/
