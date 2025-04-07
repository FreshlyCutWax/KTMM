/*
 *  tmemscan.c
 *
 *  Page scanning and related functions for tmem module.
 *
 */
#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/freezer.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/kthread.h>
//#include <linux/linkage.h>
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
#include <linux/swap.h>
#include <linux/wait.h>

#include "tmem_syms.h"
#include "tmem_vmscan.h"

// Temporary list to hold references to tmem daemons.
// Should replace kswapd task_struct in pglist_data
static struct task_struct *tmemd_list[MAX_NUMNODES];
//wait_queue_head_t tmemd_wait[MAX_NUMNODES];

/**
 * This is a recreation of the scan_control struct from mm/vmscan.c. All of the
 * original members of the struct will be present, with possible additions that
 * should be added to the end to keep the original data alignment as much as
 * possible.
 *
 * We will remove most of the comments for the original members for conciseness.
 * For any unconventional use or addtitions to the struct, comments will be
 * added for clarification.
 */
struct scan_control {
	unsigned long nr_to_reclaim;
	nodemask_t	*nodemask;
	struct mem_cgroup *target_mem_cgroup;
	unsigned long	anon_cost;
	unsigned long	file_cost;

#define DEACTIVATE_ANON 1
#define DEACTIVATE_FILE 2
	unsigned int may_deactivate:2;
	unsigned int force_deactivate:1;
	unsigned int skipped_deactivate:1;
	unsigned int may_writepage:1;
	unsigned int may_unmap:1;
	unsigned int may_swap:1;
	unsigned int proactive:1;

	/*
	 * Cgroup memory below memory.low is protected as long as we
	 * don't threaten to OOM. If any cgroup is reclaimed at
	 * reduced force or passed over entirely due to its memory.low
	 * setting (memcg_low_skipped), and nothing is reclaimed as a
	 * result, then go back for one more cycle that reclaims the protected
	 * memory (memcg_low_reclaim) to avert OOM.
	 */
	unsigned int memcg_low_reclaim:1;
	unsigned int memcg_low_skipped:1;
	unsigned int hibernation_mode:1;
	unsigned int compaction_ready:1;
	unsigned int cache_trim_mode:1;
	unsigned int file_is_tiny:1;
	unsigned int no_demotion:1;

#ifdef CONFIG_LRU_GEN
	/* help kswapd make better choices among multiple memcgs */
	unsigned int memcgs_need_aging:1;
	unsigned long last_reclaimed;
#endif

	/* Allocation order */
	s8 order;

	/* Scan (total_size >> priority) pages at once */
	s8 priority;

	/* The highest zone to isolate folios for reclaim from */
	s8 reclaim_idx;

	/* This context's GFP mask */
	gfp_t gfp_mask;

	unsigned long nr_scanned;
	unsigned long nr_reclaimed;

	struct {
		unsigned int dirty;
		unsigned int unqueued_dirty;
		unsigned int congested;
		unsigned int writeback;
		unsigned int immediate;
		unsigned int file_taken;
		unsigned int taken;
	} nr;

	/* for recording the reclaimed slab by now */
	struct reclaim_state reclaim_state;
};

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
		//pr_info("Scanning evictable LRU list on node %d: %d\n", nid, lru);
		
		spin_lock_irqsave(&lruvec->lru_lock, flags);
		
		// call list_scan function
		ref_count = scan_lru_list(list);
		
		spin_unlock_irqrestore(&lruvec->lru_lock, flags);
		
		// for debug purpses, change later
		//pr_info("Reference count: %d\n", ref_count);
	}
}

/*
static void tmemd_try_to_sleep(pg_data_t *pgdat)
{
	int nid = READ_ONCE(pgdat->node_id);

	DEFINE_WAIT(wait);

	if (freezing(current) || kthread_should_stop())
		return;
	
	prepare_to_wait(&tmemd_wait[nid], &wait, TASK_INTERRUPTIBLE);
	if (kthread_should_stop())
		schedule_timeout(HZ);

	finish_wait(&tmemd_wait[nid], &wait);
}
*/

/**
 * tmemd - page promotion daemon function
 *
 * @p:	pointer to node data struct (pglist_data)
 *
 * This function will replace the task_struct kswapd* 
 * that is found in the pglist_data pgdat struct.
 * Currently, we only store it in our own local array
 * of type task_struct.
 */
static int tmemd(void *p) 
{
	int nid;
	pg_data_t *pgdat;
	//unsigned int alloc_order, reclaim_order;
	unsigned int highest_zoneidx;
	//struct zone_type current_idx;
	struct task_struct *tsk = current;
	const struct cpumask *cpumask;

	pgdat = (pg_data_t *)p;
	nid = pgdat->node_id;
	cpumask = cpumask_of_node(pgdat->node_id);
	highest_zoneidx = MAX_NR_ZONES - 1;

	pr_info("[tmem debug] Entered tmemd function for node %d\n", nid);

	// Only allow node's CPUs to run this task
	if(!cpumask_empty(cpumask))
		set_cpus_allowed_ptr(tsk, cpumask);

	/*
	 * Tell MM that we are a memory allocator, and that we are actually
	 * kswapd. We are also set to suspend as needed.
	 *
	 * Flags are located in include/sched.h for more info.
	 */
	tsk->flags |= PF_MEMALLOC | PF_KSWAPD;
	//set_freezable();

	/*
	 * Before we enter into the loop, we want to wait here until we can catch
	 * kswapd from the wait queue and put it in our wait queue. Then, we can
	 * inherit kswapd's reclaim order and highest zone.
	 */
	
	/*
	 * Loop every few seconds and scan the node's LRU lists.
	 * If the thread is signaled to stop, we will exit.
	 */
	for ( ; ; )
	{
		//bool ret;

		//alloc_order = reclaim_order = READ_ONCE(pgdat->kswapd_order);

//tmemd_try_sleep:
		//sleep function here	
		msleep(1000);
		//pr_info("[tmem debug] tmemd on node %d trying to sleep\n", nid);
		//tmemd_try_to_sleep(pgdat);
		
		/* 
		 * We might need to read new allocation and reclaim orders
		 * depending on future iterations of tmemd_try_to_sleep
		 * function, so I'm leaving a note here.
		 */
		
		//ret = try_to_freeze();
		if(kthread_should_stop())
			break;
		
		//if (ret) continue;
		
		scan_node(pgdat, nid);
	}

	tsk->flags &= ~(PF_MEMALLOC | PF_KSWAPD);
	
	return 0;
}


/*
 * EXPERIMENT
 */
struct tmem_hook_buffer vmhooks = {
	.buf = { 
		HOOK("balance_pgdat", &tmem_balance_pgdat),
	},
};




/**
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
int tmemd_start_available(void) 
{
	int i;
	int nid;
	int err;
	
	//vmhooks.len = INIT_BUF_LEN(vmhooks.buf);
	vmhooks.len = 1;

	pr_info("[tmem debug] vmhooks length: %lu", vmhooks.len);

	err = install_hooks(&vmhooks);
	if (err) {
		pr_info("[tmem] failed to install hooks");
		return err;
	}

	pr_info("[tmem debug] hooks installed!");

	//for (i = 0; i < MAX_NUMNODES; i++)
	//	init_waitqueue_head(&tmemd_wait[i]);
	
	for_each_online_node(nid)
	{
		pg_data_t *pgdat = NODE_DATA(nid);
		
        	tmemd_list[nid] = kthread_run(&tmemd, pgdat, "tmemd");
	}

	return 0;
}


/**
 * This stops all thread daemons for each node when exiting.
 * It uses the node ID to grab the daemon out of our local list.
 */
void tmemd_stop_all(void)
{
    int nid;
    int ret;

    for_each_online_node(nid)
    {
        kthread_stop(tmemd_list[nid]);
    }

    ret = uninstall_hooks(&vmhooks);
    if (ret) pr_info("[tmem debug] hooks did not uninstall correctly");
}

