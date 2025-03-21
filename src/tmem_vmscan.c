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
// Probably remove this later and store tmemd in place of kswapd.
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
 * This is really hacky, but we had no other choice.
 */
static void scan_node(pg_data_t *pgdat, int nid)
{
	enum lru_list lru;
	struct mem_cgroup *memcg;
	struct mem_cgroup *root;
	struct lruvec *lruvec;

	// get memory cgroup for the node
	root = NULL;
	memcg = tmem_cgroup_iter(root, NULL, NULL);

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


static int tmemd(void *p) 
{
	pg_data_t *pgdat; 
	int nid;

	pgdat = (pg_data_t *)p;
	nid = pgdat->node_id;

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
 * tmem_start_available - start tmemd on all available nodes
 */
void tmemd_start_available(void) 
{
	int nid;
	
	for_each_online_node(nid)
	{
		pg_data_t *pgdat = NODE_DATA(nid);
		
        	tmem_d_list[nid] = kthread_run(&tmemd, pgdat, "tmemd");
	}
	//return num_online_nodes();
}


void tmemd_stop_all(void)
{
    int nid;

    for_each_online_node(nid)
    {
        kthread_stop(tmem_d_list[nid]);
    }
}
