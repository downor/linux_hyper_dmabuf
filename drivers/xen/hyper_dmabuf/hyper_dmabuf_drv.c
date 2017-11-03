#include <linux/init.h>       /* module_init, module_exit */
#include <linux/module.h> /* version info, MODULE_LICENSE, MODULE_AUTHOR, printk() */
#include <linux/workqueue.h>
#include <xen/grant_table.h>
#include "hyper_dmabuf_drv.h"
#include "hyper_dmabuf_conf.h"
#include "hyper_dmabuf_list.h"
#include "xen/hyper_dmabuf_xen_comm_list.h"
#include "xen/hyper_dmabuf_xen_comm.h"

MODULE_LICENSE("Dual BSD/GPL");
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

	printk( KERN_NOTICE "initializing database for imported/exported dmabufs\n");

	/* device structure initialization */
	/* currently only does work-queue initialization */
	hyper_dmabuf_private.work_queue = create_workqueue("hyper_dmabuf_wqueue");

	ret = hyper_dmabuf_table_init();
	if (ret < 0) {
		return -EINVAL;
	}

	ret = hyper_dmabuf_ring_table_init();
	if (ret < 0) {
		return -EINVAL;
	}

	ret = hyper_dmabuf_setup_data_dir();
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

	hyper_dmabuf_cleanup_ringbufs();
	hyper_dmabuf_ring_table_destroy();

	/* destroy workqueue */
	if (hyper_dmabuf_private.work_queue)
		destroy_workqueue(hyper_dmabuf_private.work_queue);

	hyper_dmabuf_destroy_data_dir();
	printk( KERN_NOTICE "dma_buf-src_sink model: Exiting" );
	unregister_device();
}
/*===============================================================================================*/

module_init(hyper_dmabuf_drv_init);
module_exit(hyper_dmabuf_drv_exit);
