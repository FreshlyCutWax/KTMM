/*
 * Kernel Tiered Memory Module
 *
 * Copyright (c) FreshlyCutWax
 */

#define pr_fmt(fmt) "[ KTMM Mod ] " fmt
#define KERN_VER_MAJOR 6
#define KERN_VER_MINOR 1
#define KERN_VER_PATCH 0

/*
 * KERNEL
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/version.h>

/*
 * MODULE
 */
#include "ktmm_hook.h"
//#include "ktmm_vmscan.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jared Draper, Josh Borthick, Grant Wilke, Camilo Palomino");
MODULE_DESCRIPTION("Tiered Memory Module.");


static int __init tmem_init(void) {
	int ret;

	pr_info("Module initializing..\n");

#if LINUX_VERSION_CODE < KERNEL_VERSION(KERN_VER_MAJOR, \
					KERN_VER_MINOR, \
					KERN_VER_PATCH) 
	ret = 0;
	
#else
	pr_info("Minimum kernel version not met.");
	ret = -EINVAL;
#endif

	return ret;
}


static void __exit tmem_exit(void) {
	pr_info("Module exiting..\n");
}


module_init(tmem_init);
module_exit(tmem_exit);
