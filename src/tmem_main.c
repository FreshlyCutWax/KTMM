#include <linux/delay.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/stat.h>

#include "tmem_syms.h"
#include "tmem_vmscan.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("CSC450 Group 4");
MODULE_DESCRIPTION("Tiered memory module.");


static int __init tmem_init(void) {
	int ret;

	pr_info( "tmem-csc450 module initializing..\n" );
	
	if(init_vmscan_symbols())
	{
		available_nodes();
		ret = 0;
	}
	else
		ret = -EFAULT;
	
	return ret;
}


static void __exit tmem_exit(void) {
	pr_info("tmem-csc450 exiting..\n");
	tmem_try_to_stop();
}


module_init(tmem_init);
module_exit(tmem_exit);
