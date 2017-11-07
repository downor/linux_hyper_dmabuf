#include <linux/list.h>
#include <linux/slab.h>
#include "hyper_dmabuf_msg.h"
#include "hyper_dmabuf_drv.h"
#include "hyper_dmabuf_id.h"

extern struct hyper_dmabuf_private hyper_dmabuf_private;

void store_reusable_id(int id)
{
	struct list_reusable_id *reusable_head = hyper_dmabuf_private.id_queue;
	struct list_reusable_id *new_reusable;

	new_reusable = kmalloc(sizeof(*new_reusable), GFP_KERNEL);
	new_reusable->id = id;

	list_add(&new_reusable->list, &reusable_head->list);
}

static int retrieve_reusable_id(void)
{
	struct list_reusable_id *reusable_head = hyper_dmabuf_private.id_queue;
	int id;

	/* check there is reusable id */
	if (!list_empty(&reusable_head->list)) {
		reusable_head = list_first_entry(&reusable_head->list,
						 struct list_reusable_id,
						 list);

		list_del(&reusable_head->list);
		id = reusable_head->id;
		kfree(reusable_head);
		return id;
	}

	return -1;
}

void destroy_reusable_list(void)
{
	struct list_reusable_id *reusable_head = hyper_dmabuf_private.id_queue;
	struct list_reusable_id *temp_head;

	if (reusable_head) {
		/* freeing mem space all reusable ids in the stack */
		while (!list_empty(&reusable_head->list)) {
			temp_head = list_first_entry(&reusable_head->list,
						     struct list_reusable_id,
						     list);
			list_del(&temp_head->list);
			kfree(temp_head);
		}

		/* freeing head */
		kfree(reusable_head);
	}
}

int hyper_dmabuf_get_id(void)
{
	static int id = 0;
	struct list_reusable_id *reusable_head;
	int ret;

	/* first cla to hyper_dmabuf_get_id */
	if (id == 0) {
		reusable_head = kmalloc(sizeof(*reusable_head), GFP_KERNEL);
		reusable_head->id = -1; /* list head have invalid id */
		INIT_LIST_HEAD(&reusable_head->list);
		hyper_dmabuf_private.id_queue = reusable_head;
	}

	ret = retrieve_reusable_id();

	if (ret < 0 && id < HYPER_DMABUF_ID_MAX)
		return HYPER_DMABUF_ID_CREATE(hyper_dmabuf_private.domid, id++);

	return ret;
}
