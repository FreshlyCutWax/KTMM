/*
 *  tmemscan.c
 *
 *  Page scanning and related functions for tmem module.
 *
 */
#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/memcontrol.h>
#include <linux/mmzone.h>
#include <linux/nodemask.h>
#include <linux/numa.h>
#include <linux/page-flags.h>
#include <linux/page_ref.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/spinlock.h>

#include "tmem_syms.h"
#include "tmem_vmscan.h"

// static struct scan_control = { }

// Temporary list to hold references to tmem daemons.
// Should replace kswapd task_struct in pglist_data
static struct task_struct *tmem_d_list[MAX_NUMNODES];


/*****************************************************************************
 * Node Scanning Functions
 *****************************************************************************/

/* need to acquire spinlock before calling this function */

/**
 * scan_lru_list - scan LRU list for recently accessed pages
 *
 * @list:	the LRU list to scan
 *
 * @returns:	Number of pages with a reference bit set
 *
 * Iterates through the pages in the LRU list and records
 * the number of pages that have the reference bit set.
 *
 * The lru_lock must be acquired before calling this function.
 *
 * struct page [src/include/mmtypes.h]
 * list_for_each_entry_safe() [src/include/list.h]
 * test_bit() [src/include/bitops.h]
 * PG_referenced [src/include/page-flags.h]
 *
 * All linked-list related structures and functions are
 * contained in list.h. All bit flags are found in the
 * pageflags enum in page-flags.h. Bit related operations
 * are found in bitops.h.
 */
static unsigned int scan_lru_list(struct list_head *list)
{
	struct page *page, *next;
	unsigned int nr_page_refs; //number of pages w/ reference bit set
	
	nr_page_refs = 0;
	list_for_each_entry_safe(page, next, list, lru)
	{
		//
		if(test_bit(PG_referenced, &page->flags))
			nr_page_refs++;
	}

	return nr_page_refs;
}


/**
 * scan_node - scan a node's LRU lists
 * 
 * @pgdat:	node data struct
 * @nid:	node ID number
 *
 * This is responsible for scanning a node's list by invoking
 * the scan_lru_list function. The memory control group is 
 * first acquired to gain access to lruvec. This is done by
 * using our casted version of mem_cgroup_iter that is defined
 * in tmem_syms.h. 
 *
 * The memcg struct is a mem_cgroup structure. It contains a
 * pointer to the node's lruvec, which is why we need it to
 * gain access to the lruvec. The member __lruvec in pglist_data
 * is not active, and cannot no longer be used to get the LRU
 * lists.
 *
 * After acquiring the lruvec, we use for_each_evicatable_lru()
 * to iterate over all LRU lists in the node; calling scan_lru_list
 * on each list. Notice, we acquire the lru_lock first before
 * scanning a list to avoid race conditions. 
 *
 * This is partially modeled from scan_node() in multi-clock.
 *
 * struct lru_list, struct lruvec [src/include/mmzone.h]
 * struct mem_cgroup, mem_cgroup_iter() [src/include/memcontrol.h]
 */
static void scan_node(pg_data_t *pgdat, int nid)
{
	enum lru_list lru;
	struct mem_cgroup *memcg;
	struct mem_cgroup *root;
	struct lruvec *lruvec;

	// get memory cgroup for the node
	// tmem_cgroup_iter() = mem_cgroup_iter()
	root = NULL;
	memcg = tmem_cgroup_iter(root, NULL, NULL);
	
	// acquire the lruvec structure
	lruvec = &memcg->nodeinfo[nid]->lruvec;
	
	// scan the LRU lists
	for_each_evictable_lru(lru) 
	{
		unsigned int ref_count;
		unsigned long flags;
		struct list_head *list;
		list = &lruvec->lists[lru];
		
		// for debug purposes, change later
		pr_info("Scanning evictable LRU list: %d\n", lru);
		
		spin_lock_irqsave(&lruvec->lru_lock, flags);
		
		// call list_scan function
		ref_count = scan_lru_list(list);
		
		spin_unlock_irqrestore(&lruvec->lru_lock, flags);
		
		// for debug purpses, change later
		pr_info("Reference count: %d\n", ref_count);
	}
}


/**
 * tmemd - page promotion daemon function
 *
 * @p:	pointer to node data struct (pglist_data)
 *
 * This function will replace the task_struct kswapd* 
 * that is found in the pglist_data pgdat struct.
 * Currently, we only store it in our own local array
 * of type task_struct.
 *
 * struct pglist_data (src/linux/mmzone.h)
 */
static int tmemd(void *p) 
{
	pg_data_t *pgdat;	// struct pglist_data (node)
	int nid; 	  	// node ID (also in pglist_data as node_id)
	
	pgdat = (pg_data_t *)p;
	nid = pgdat->node_id;
	
	// Loop every few seconds and scan the node's LRU lists.
	// If the thread is signaled to stop, we will exit.
	for ( ; ; )
	{
		scan_node(pgdat, nid);
		
		if(kthread_should_stop())
			break;
		
		msleep(10000);
	}
	
	return 0;
}


/**
 * tmem_start_available - start tmem daemons on all online nodes
 *
 * Daemons are only started on online/active nodes. They are
 * currently stored in a local list, but will later need to be
 * stored with the node itself (in-place of kswapd in pglist_data).
 *
 * We will also need to define the behavior for hot-plugging nodes
 * into the system, as this code only sets up daemons on nodes 
 * that are online the moment the module starts.
 *
 * for_each_online_node() & NODE_DATA() [src/include/mmzone.h]
 *
 * kthread_run [src/include/kthread.h]
 */
void tmemd_start_available(void) 
{
	int nid;
	
	for_each_online_node(nid)
	{
		pg_data_t *pgdat = NODE_DATA(nid);
		
        	tmem_d_list[nid] = kthread_run(&tmemd, pgdat, "tmemd");
	}
}


/**
 * tmemd_stop_all - stop all tmem daemons on all online nodes
 *
 * This stops all thread daemons for each node when exiting.
 * It uses the node ID to grab the daemon out of our local list.
 */
void tmemd_stop_all(void)
{
    int nid;

    for_each_online_node(nid)
    {
        kthread_stop(tmem_d_list[nid]);
    }
}
