/*
 *  ktmm_vmscan.c
 *
 *  Page scanning and related functions.
 */

#define pr_fmt(fmt) "[ KTMM Mod ] vmscan - " fmt

#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/freezer.h>
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
#include <linux/swap.h>
#include <linux/wait.h>

#include "ktmm_hook.h"
#include "ktmm_vmscan.h"

#define TMEMD_GFP_FLAGS GFP_NOIO

// Temporary list to hold references to tmem daemons.
// Replace kswapd task_struct in pglist_data?
static struct task_struct *tmemd_list[MAX_NUMNODES];

// watermark watchdog
static struct task_struct *wd;

//Temporary list to hold our wait sleep queues for tmem daemons
//Replace kswapd kswapd_wait in pglist_data?
wait_queue_head_t tmemd_wait[MAX_NUMNODES];


/**
 * This is a import of the orignal scan_control struct from mm/vmscan.c. We 
 * will remove most of the comments for the original members for conciseness.
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


/************** IMPORTED/HOOKED PROTOTYPES HERE *******************/
static struct mem_cgroup *(*pt_mem_cgroup_iter)(struct mem_cgroup *root,
				struct mem_cgroup *prev,
				struct mem_cgroup_reclaim_cookie *reclaim);

/* FROM: vmscan.c */
static int (*pt_balance_pgdat)(pg_data_t *pgdat, 
				int order, 
				int highest_zoneidx);

/* FROM: page_alloc.c */
/*
void (*pt__fs_reclaim_acquire)(unsigned long ip);
*/


static bool (*pt_zone_watermark_ok_safe)(struct zone *z,
					unsigned int order,
					unsigned long mark,
					int highest_zoneidx);


/******************* HOOK REDEFS HERE *****************************/
static struct mem_cgroup *ktmm_mem_cgroup_iter(struct mem_cgroup *root,
				struct mem_cgroup *prev,
				struct mem_cgroup_reclaim_cookie *reclaim)
{
	struct mem_cgroup *memcg;

	memcg = pt_mem_cgroup_iter(root, prev, reclaim);

	return memcg;
}


static bool ktmm_zone_watermark_ok_safe(struct zone *z,
					unsigned int order,
					unsigned long mark,
					int highest_zoneidx)
{
	bool ret;

	ret = pt_zone_watermark_ok_safe(z, order, mark, highest_zoneidx);

	return ret;
}

/**
 * Acquire the lock to reclaim memory.
 *
void ktmm__fs_reclaim_acquire(unsigned long ip)
{
	pt__fs_reclaim_acquire(ip);
}
*/


static int ktmm_balance_pgdat(pg_data_t *pgdat,
				int order,
				int highest_zoneidx)
{
	int ret;
	
	/*
	int i;
	unsigned long nr_soft_reclaimed;
	unsigned long nr_soft_scanned;
	unsigned long pflags;
	unsigned long nr_boost_reclaim;
	unsigned long zone_boosts[MAX_NR_ZONES] = { 0, };
	bool boosted;
	struct task_struct *tsk = current;
	struct zone *zone;

	struct scan_control sc = {
		.nr_to_reclaim = SWAP_CLUSTER_MAX,
		.gfp_mask = GFP_NOIO,
		.priority = DEF_PRIORITY,
		.may_writepage = !laptop_mode, //do not delay writing to disk
		.may_unmap = 1,
		.may_swap = 1,
		.reclaim_idx = MAX_NR_ZONES - 1,
		.reclaim_state = tsk->reclaim_state,
	};

	ktmm__fs_reclaim_acquire(_THIS_IP_);
	*/
	

	/*
	 * Call the real balance_pgdat().
	 */
	ret = pt_balance_pgdat(pgdat, order, highest_zoneidx);

	return ret;
}


/****************** ADD VMSCAN HOOKS HERE ************************/
static struct ktmm_hook vmscan_hooks[] = {
	HOOK("mem_cgroup_iter", ktmm_mem_cgroup_iter, &pt_mem_cgroup_iter),
	HOOK("balance_pgdat", ktmm_balance_pgdat, &pt_balance_pgdat),
	//HOOK("__fs_reclaim_acquire", ktmm__fs_reclaim_acquire, pt__fs_reclaim_acquire),
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
	memcg = ktmm_mem_cgroup_iter(root, NULL, NULL);
	
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
		pr_debug("scanning evictable LRU list on node %d: %d\n", nid, lru);
		
		spin_lock_irqsave(&lruvec->lru_lock, flags);
		
		// call list_scan function
		ref_count = scan_lru_list(list);
		
		spin_unlock_irqrestore(&lruvec->lru_lock, flags);
		
		// for debug purpses, change later
		pr_debug("list reference count: %d\n", ref_count);
	}
}


/**
 * Hook prepare_alloc_pages, and call if zone watermark is lower.
 *
 * gfp_t gfp
 * unsigned int order
 * int preferred_nid
 * nodemask_t *nodemask
 * struct alloc_context *ac
 * gfp_t alloc_gfp
 * unsigned int alloc_flags
 *
 * We can get the node id from the zone when either of these are called:
 *
 * zone_watermark_fast
 * zone_watermark_ok
 */
void wakeup_tmemd(int nid)
{
	//check to make sure tmemd in waiting
	if (!waitqueue_active(&tmemd_wait[nid]))
		return;
	
	//if it is waiting, wake up
	wake_up_interruptible(&tmemd_wait[nid]);
}


static void tmemd_try_to_sleep(pg_data_t *pgdat, int nid)
{
	long remaining = 0;
	DEFINE_WAIT(wait);

	if (freezing(current) || kthread_should_stop())
		return;
	
	prepare_to_wait(&tmemd_wait[nid], &wait, TASK_INTERRUPTIBLE);

	remaining = schedule_timeout(HZ);

	/*
	 * If tmemd is interrupted, then we need to come out of sleep and go
	 * back to scanning and migrating pages between nodes.
	 */
	if (!kthread_should_stop() && !remaining) 
		schedule();

	finish_wait(&tmemd_wait[nid], &wait);
}


/**
 * tmemd - page promotion daemon
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
	pg_data_t *pgdat = (pg_data_t *)p;
	int nid = READ_ONCE(pgdat->node_id);
	struct task_struct *tsk = current;
	const struct cpumask *cpumask = cpumask_of_node(pgdat->node_id);

	pr_debug("tmemd started on node %d", nid);

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

	/*
	 * Loop every few seconds and scan the node's LRU lists.
	 * If the thread is signaled to stop, we will exit.
	 */
	for ( ; ; )
	{
		scan_node(pgdat, nid);

		if(kthread_should_stop()) break;
		
//tmemd_try_sleep:
		//msleep(10000);
		tmemd_try_to_sleep(pgdat, nid);
		
	}

	tsk->flags &= ~(PF_MEMALLOC | PF_KSWAPD);
	
	return 0;
}


static int wmark_watchdogd(void *p)
{
	struct zone *zone;
	gfp_t flags = TMEMD_GFP_FLAGS;
	int nid;
	int highest_zoneidx;
	unsigned long watermark;
	bool ok;

	do {
		for_each_populated_zone(zone) {

			highest_zoneidx = gfp_zone(flags);
			watermark = high_wmark_pages(zone);
			nid = zone_to_nid(zone);

			ok = ktmm_zone_watermark_ok_safe(zone, 0, 
					watermark, highest_zoneidx);

			if (!ok) wakeup_tmemd(nid);
		}

		ssleep(1);

	} while(!kthread_should_stop());

	return 0;
}


/*
int kswapd_convert_available(void)
{
	int ret;

	ret = install_hooks(vmscan_hooks, ARRAY_SIZE(vmscan_hooks));

	return ret;
}


void kswapd_revert_all(void)
{
    uninstall_hooks(vmscan_hooks, ARRAY_SIZE(vmscan_hooks));
}
*/


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
	int ret;

	pr_debug("starting tmemd on available nodes");
	
	/* initialize wait queues for sleeping */
	for (i = 0; i < MAX_NUMNODES; i++)
		init_waitqueue_head(&tmemd_wait[i]);
	
	ret = install_hooks(vmscan_hooks, ARRAY_SIZE(vmscan_hooks));
	
	for_each_online_node(nid)
	{
		pg_data_t *pgdat = NODE_DATA(nid);
		
        	tmemd_list[nid] = kthread_run(&tmemd, pgdat, "tmemd");
	}

	/* start the watermark watchdog */
	wd = kthread_run(&wmark_watchdogd, NULL, "wmark_watchdogd");

	return ret;
}


/**
 * This stops all thread daemons for each node when exiting.
 * It uses the node ID to grab the daemon out of our local list.
 */
void tmemd_stop_all(void)
{
	int nid;

	for_each_online_node(nid)
	{
		kthread_stop(tmemd_list[nid]);
	}

	/* start the watermark watchdog */
	kthread_stop(wd);

	uninstall_hooks(vmscan_hooks, ARRAY_SIZE(vmscan_hooks));
}

