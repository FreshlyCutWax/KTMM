#include <linux/delay.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/stat.h>


#include "tmem_vmscan.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("CSC450 Group 4");
MODULE_DESCRIPTION("Tiered memory module.");


static struct task_struct *kernel_thread1;


int thread_function(void *arg) {
	pr_info( "tmem-csc450 thread started..\n" );
	init_kallsyms();
	while (!kthread_should_stop()) {
		avail_nodes();
		msleep(10000);
	}
	return 0;
}


static int __init tmem_init(void) {
	pr_info( "tmem-csc450 module initializing..\n" );
	kernel_thread1 = kthread_run(&thread_function, NULL, "thread_function");
	return 0;
}


static void __exit tmem_exit(void) {
	pr_info("tmem-csc450 exiting..\n");
	kthread_stop(kernel_thread1);
}


module_init(tmem_init);
module_exit(tmem_exit);
