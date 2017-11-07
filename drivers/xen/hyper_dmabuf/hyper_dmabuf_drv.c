#include <linux/init.h>
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/device.h>
#include "hyper_dmabuf_conf.h"
#include "hyper_dmabuf_msg.h"
#include "hyper_dmabuf_drv.h"
#include "hyper_dmabuf_list.h"
#include "hyper_dmabuf_id.h"

#ifdef CONFIG_XEN
#include "xen/hyper_dmabuf_xen_drv.h"
extern struct hyper_dmabuf_backend_ops xen_backend_ops;
#endif

MODULE_LICENSE("GPL");
MODULE_AUTHOR("IOTG-PED, INTEL");

int register_device(void);
int unregister_device(void);

struct hyper_dmabuf_private hyper_dmabuf_private;

/*===============================================================================================*/
static int hyper_dmabuf_drv_init(void)
{
	int ret = 0;

	printk( KERN_NOTICE "hyper_dmabuf_starting: Initialization started" );

	ret = register_device();
	if (ret < 0) {
		return -EINVAL;
	}

#ifdef CONFIG_XEN
	hyper_dmabuf_private.backend_ops = &xen_backend_ops;
#endif

	dev_info(hyper_dmabuf_private.device,
		 "initializing database for imported/exported dmabufs\n");

	/* device structure initialization */
	/* currently only does work-queue initialization */
	hyper_dmabuf_private.work_queue = create_workqueue("hyper_dmabuf_wqueue");
	hyper_dmabuf_private.domid = hyper_dmabuf_private.backend_ops->get_vm_id();

	ret = hyper_dmabuf_table_init();
	if (ret < 0) {
		return -EINVAL;
	}

	ret = hyper_dmabuf_private.backend_ops->init_comm_env();

	if (ret < 0) {
		return -EINVAL;
	}

	/* interrupt for comm should be registered here: */
	return ret;
}

/*-----------------------------------------------------------------------------------------------*/
static void hyper_dmabuf_drv_exit(void)
{
	/* hash tables for export/import entries and ring_infos */
	hyper_dmabuf_table_destroy();

	hyper_dmabuf_private.backend_ops->destroy_comm();

	/* destroy workqueue */
	if (hyper_dmabuf_private.work_queue)
		destroy_workqueue(hyper_dmabuf_private.work_queue);

	/* destroy id_queue */
	if (hyper_dmabuf_private.id_queue)
		destroy_reusable_list();

	printk( KERN_NOTICE "dma_buf-src_sink model: Exiting" );
	unregister_device();
}
/*===============================================================================================*/

module_init(hyper_dmabuf_drv_init);
module_exit(hyper_dmabuf_drv_exit);
