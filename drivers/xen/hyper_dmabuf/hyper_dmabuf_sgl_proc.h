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

#ifndef __HYPER_DMABUF_IMP_H__
#define __HYPER_DMABUF_IMP_H__

int dmabuf_refcount(struct dma_buf *dma_buf);

/* extract pages directly from struct sg_table */
struct hyper_dmabuf_pages_info *hyper_dmabuf_ext_pgs(struct sg_table *sgt);

/* create sg_table with given pages and other parameters */
struct sg_table* hyper_dmabuf_create_sgt(struct page **pages,
                                int frst_ofst, int last_len, int nents);

int hyper_dmabuf_cleanup_sgt_info(struct hyper_dmabuf_sgt_info *sgt_info, int force);

void hyper_dmabuf_free_sgt(struct sg_table *sgt);

#endif /* __HYPER_DMABUF_IMP_H__ */
