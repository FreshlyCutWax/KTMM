/*
 *  ktmm_vmscan.c
 *
 *  Page scanning and related functions.
 */

#define pr_fmt(fmt) "[ KTMM Mod ] vmscan - " fmt

#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/gfp.h>
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

// possibly needs to be GFP_USER?
#define TMEMD_GFP_FLAGS GFP_NOIO

//static struct pglist_data_ext *node_data_ext[MAX_NUMNODES];

// Temporary list to hold references to tmem daemons.
// Replace kswapd task_struct in pglist_data?
static struct task_struct *tmemd_list[MAX_NUMNODES];

// watermark watchdog
static struct task_struct *wd;

//Temporary list to hold our wait sleep queues for tmem daemons
//Replace kswapd kswapd_wait in pglist_data?
wait_queue_head_t tmemd_wait[MAX_NUMNODES];


// zones we care about watermarks & moving pages
// ZONE_NORMAL, ZONE_HIGHMEM
const int ktmm_zone_watchlist[] = {2, 3};


/* get the next page/folio in the list */
#define lru_to_page_next(head) (list_entry((head)->next, struct page, lru))

/**
 * pglist_data_ext - pglist_data extension for ktmm
 */
struct pglist_data_ext {
	struct pglist_data *pgdat;
	struct lruvec_ext *lruvec_ext;

	wait_queue_head_t tmemd_wait;
	struct task_struct *tmemd;
	int pmem_node;
};

/* returns a pointer, so make sure you use it correctly */
#define NODE_DATA_EXT(nid)	(node_data_ext[nid])
#define ___GFP_PMEM 		0x1000000u
#define __GFP_PMEM ((__force gfp_t)___GFP_PMEM)
#define LRU_PROMOTE_BASE 0

enum promote_lru_list {
	LRU_PROMOTE_ANON = LRU_PROMOTE_BASE,
	LRU_PROMOTE_FILE,
	NR_PROMOTE_LISTS,
};


struct lruvec_ext {
	struct lruvec *lruvec;
	struct list_head *promote_lru[NR_PROMOTE_LISTS];
};

struct page* vmscan_alloc_pmem_page(struct  page *page, unsigned long data)
{
		gfp_t gfp_mask = GFP_USER | __GFP_PMEM;
		//return alloc_pages_node(pmem_node_id, gfp_mask, 0);
		return alloc_page(gfp_mask);
}

struct page* vmscan_alloc_normal_page(struct page *page, unsigned long data)
{
        gfp_t gfp_mask = GFP_USER;
        return alloc_page(gfp_mask);
}

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

	/* Searching for pages to promote */
	unsigned int only_promote:1;

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

/************** IMPORTED/HOOKED PROTOTYPES HERE *****************************/
static struct mem_cgroup *(*pt_mem_cgroup_iter)(struct mem_cgroup *root,
				struct mem_cgroup *prev,
				struct mem_cgroup_reclaim_cookie *reclaim);

/* FROM: vmscan.c */
static int (*pt_balance_pgdat)(pg_data_t *pgdat, 
				int order, 
				int highest_zoneidx);

/* FROM: vmscan.c */
static unsigned long (*pt_shrink_inactive_list)(unsigned long nr_to_scan, 
				struct lruvec_ext *lruvec_ext,
				struct scan_control *sc,
				enum lru_list lru);


/* FROM: page_alloc.c */
static bool (*pt_zone_watermark_ok_safe)(struct zone *z,
					unsigned int order,
					unsigned long mark,
					int highest_zoneidx);


/* FROM: mmzone.c */
static struct pglist_data *(*pt_first_online_pgdat)(void);


/* FROM: mmzone.c */
static struct zone *(*pt_next_zone)(struct zone *zone);


/**************** END IMPORTED/HOOKED PROTOTYPES *****************************/
static int ktmm_balance_pgdat(pg_data_t *pgdat,
				int order,
				int highest_zoneidx)
{
	int ret;
	
	ret = pt_balance_pgdat(pgdat, order, highest_zoneidx);

	return ret;
}


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


static struct pglist_data *ktmm_first_online_pgdat(void)
{
	return pt_first_online_pgdat();
}


static struct zone *ktmm_next_zone(struct zone *zone)
{
	return pt_next_zone(zone);
}


bool watching_zonetype(struct zone *z)
{
	int i;
	int last = ARRAY_SIZE(ktmm_zone_watchlist);
	unsigned long zid = zone_idx(z);

	for (i = 0; i < last; i++)
		if (ktmm_zone_watchlist[i] == zid) return true;

	return false;
}


#define ktmm_for_each_populated_zone(zone) 			\
	for (zone = (ktmm_first_online_pgdat())->node_zones; 	\
		zone;						\
		zone = ktmm_next_zone(zone))			\
			if(!populated_zone(zone) || !watching_zonetype(zone)); \
				/* do nothing */		\
			else


/*****************************************************************************
 * Node Scanning, Shrinking, and Promotion
 *****************************************************************************/

/*
static void shrink_promotion_list(unsigned long nr_to_scan,
				struct lruvec *lruvec,
				struct scan_control *sc,
				enum lru_list lru)
{
	// needs implementation
	return;
}
*/


static void ktmm_shrink_active_list(unsigned long nr_to_scan, 
				struct lruvec_ext *lruvec_ext,
				struct scan_control *sc,
				enum lru_list lru)
{
	return;
}


static unsigned long ktmm_shrink_inactive_list(unsigned long nr_to_scan, 
					struct lruvec_ext *lruvec_ext,
					struct scan_control *sc,
					enum lru_list lru)
{
	LIST_HEAD(folio_list);
	unsigned long nr_scanned;
	unsigned int nr_reclaimed = 0;
	unsigned long nr_taken;
	struct reclaim_stat stat;
	bool file = is_file_lru(lru);
	enum vm_event_item item;
	struct pglist_data *pgdat = lruvec_pgdat(lruvec_ext->lruvec);
	struct pglist_data_ext *pgext = pglist_data_ext(lruvec_ext->lruvec);
	bool stalled = false;

	while (unlikely(too_many_isolated(pgdat, file, sc))) {
		if (stalled)
			return 0;

		/* wait a bit for the reclaimer. */
		stalled = true;
		reclaim_throttle(pgdat, VMSCAN_THROTTLE_ISOLATED);

		/* We are about to die and free our memory. Return now. */
		if (fatal_signal_pending(current))
			return SWAP_CLUSTER_MAX;
	}

	lru_add_drain();

	spin_lock_irq(&lruvec_ext->lruvec->lru_lock);

	nr_taken = isolate_lru_folios(nr_to_scan, lruvec_ext->lruvec, &folio_list,
				     &nr_scanned, sc, lru);

	__mod_node_page_state(pgdat, NR_ISOLATED_ANON + file, nr_taken);
	item = current_is_kswapd() ? PGSCAN_KSWAPD : PGSCAN_DIRECT;
	if (!cgroup_reclaim(sc))
		__count_vm_events(item, nr_scanned);
	__count_memcg_events(lruvec_memcg(lruvec_ext->lruvec), item, nr_scanned);
	__count_vm_events(PGSCAN_ANON + file, nr_scanned);

	spin_unlock_irq(&lruvec_ext->lruvec->lru_lock);

	if (nr_taken == 0)
		return 0;

	if (pgext->pmem_node == 0) {
		int ret = migrate_pages(&folio_list, vmscan_alloc_pmem_page, NULL, 
								0, MIGRATE_SYNC, MR_MEMORY_HOTPLUG);
		nr_reclaimed = (ret >= 0 ? nr_taken - ret : 0);
		__mod_node_page_state(pgdat, NR_DEMOTED, nr_reclaimed);
	}

	nr_reclaimed = shrink_folio_list(&folio_list, pgdat, sc, &stat, false);

	spin_lock_irq(&lruvec_ext->lruvec->lru_lock);
	move_folios_to_lru(lruvec_ext->lruvec, &folio_list);

	__mod_node_page_state(pgdat, NR_ISOLATED_ANON + file, -nr_taken);
	item = current_is_kswapd() ? PGSTEAL_KSWAPD : PGSTEAL_DIRECT;
	if (!cgroup_reclaim(sc))
		__count_vm_events(item, nr_reclaimed);
	__count_memcg_events(lruvec_memcg(lruvec_ext->lruvec), item, nr_reclaimed);
	__count_vm_events(PGSTEAL_ANON + file, nr_reclaimed);
	spin_unlock_irq(&lruvec_ext->lruvec->lru_lock);

	lru_note_cost(lruvec_ext->lruvec, file, stat.nr_pageout);
	mem_cgroup_uncharge_list(&folio_list);
	free_unref_page_list(&folio_list);

	/*
	 * If dirty folios are scanned that are not queued for IO, it
	 * implies that flushers are not doing their job. This can
	 * happen when memory pressure pushes dirty folios to the end of
	 * the LRU before the dirty limits are breached and the dirty
	 * data has expired. It can also happen when the proportion of
	 * dirty folios grows not through writes but through memory
	 * pressure reclaiming all the clean cache. And in some cases,
	 * the flushers simply cannot keep up with the allocation
	 * rate. Nudge the flusher threads in case they are asleep.
	 */
	if (stat.nr_unqueued_dirty == nr_taken) {
		wakeup_flusher_threads(WB_REASON_VMSCAN);
		/*
		 * For cgroupv1 dirty throttling is achieved by waking up
		 * the kernel flusher here and later waiting on folios
		 * which are in writeback to finish (see shrink_folio_list()).
		 *
		 * Flusher may not be able to issue writeback quickly
		 * enough for cgroupv1 writeback throttling to work
		 * on a large system.
		 */
		if (!writeback_throttling_sane(sc))
			reclaim_throttle(pgdat, VMSCAN_THROTTLE_WRITEBACK);
	}

	sc->nr.dirty += stat.nr_dirty;
	sc->nr.congested += stat.nr_congested;
	sc->nr.unqueued_dirty += stat.nr_unqueued_dirty;
	sc->nr.writeback += stat.nr_writeback;
	sc->nr.immediate += stat.nr_immediate;
	sc->nr.taken += nr_taken;
	if (file)
		sc->nr.file_taken += nr_taken;

	trace_mm_vmscan_lru_shrink_inactive(pgdat->node_id,
			nr_scanned, nr_reclaimed, &stat, sc->priority, file);
	return nr_reclaimed;
}


/* similar to: shrink_list() */
static unsigned long scan_lru_list(enum lru_list lru, 
				unsigned long nr_to_scan,
				struct lruvec_ext *lruvec_ext, 
				struct scan_control *sc)
{
	if (is_active_lru(lru))
		ktmm_shrink_active_list(nr_to_scan, lruvec_ext, sc, lru);

	return ktmm_shrink_inactive_list(nr_to_scan, lruvec_ext, sc, lru);
}


/* similar to: shrink_lruvec() 
 * 
 * This might later consume scan_lru_list(), as it might be more simple to put
 * its code here instead.
 */
static void scan_lruvec(struct lruvec_ext *lruvec_ext, struct scan_control *sc)
{
	enum lru_list lru;

	// we need to determine this number dynamically later
	unsigned long nr_to_scan = 1024;

	for_each_evictable_lru(lru) {
		scan_lru_list(lru, nr_to_scan, lruvec_ext, sc);
	}
}


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
/*
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
*/


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
static void scan_node(pg_data_t *pgdat)
{
	//enum lru_list lru;
	struct zone *zone;
	struct mem_cgroup *memcg;
	struct lruvec *lruvec;
	struct lruvec_ext lruvec_ext;
	int nid = pgdat->node_id;
	int z = 0;

	//struct reclaim_state reclaim_state = {
	//	.reclaimed_slab = 0,
	//};

	struct mem_cgroup_reclaim_cookie reclaim = {
		.pgdat = pgdat,
	};

	struct scan_control sc = {
		//.nr_to_reclaim = SWAP_CLUSTER_MAX,
		.nr_to_reclaim = 0,
		.gfp_mask = TMEMD_GFP_FLAGS,
		.priority = DEF_PRIORITY,
		.may_writepage = !laptop_mode, //do not delay writing to disk
		.may_unmap = 1,
		.may_swap = 1,
		.reclaim_idx = MAX_NR_ZONES - 1,
		.target_mem_cgroup = NULL,
	};

	memset(&sc.nr, 0, sizeof(sc.nr));

	/* move number of pages proportional to number of zones */
	for (z = 0; z <= sc.reclaim_idx; z++) {
		zone = pgdat->node_zones + z;
		if (!managed_zone(zone))
			continue;

		sc.nr_to_reclaim += max(high_wmark_pages(zone), SWAP_CLUSTER_MAX);
	}

	// needs exposed to the module
	//set_task_reclaim_state(current, &reclaim_state);
	//task->reclaim_state = &reclaim_state;

	// get memory cgroup for the node
	// tmem_cgroup_iter() = mem_cgroup_iter()
	memcg = ktmm_mem_cgroup_iter(sc.target_mem_cgroup, NULL, &reclaim);
	
	// acquire the lruvec structure
	lruvec = &memcg->nodeinfo[nid]->lruvec;

	//will eventually replace the statement above
	lruvec_ext.lruvec = lruvec;
	
	pr_info("scanning lists on node %d", nid);


	//NEEDED: code to determine if we can claim pages
	//shrink_lruvec_memcg()

	// scan the LRU lists
	/*
	for_each_evictable_lru(lru) 
	{
		unsigned int ref_count;
		unsigned long flags;
		struct list_head *list;
		list = &lruvec->lists[lru];
		
		spin_lock_irqsave(&lruvec->lru_lock, flags);
		
		// call list_scan function
		ref_count = scan_lru_list(list);
		
		spin_unlock_irqrestore(&lruvec->lru_lock, flags);
	}
	*/

	scan_lruvec(&lruvec_ext, &sc);

	//scan promote list here
}


/*****************************************************************************
 * Daemon Functions & Related
 *****************************************************************************/

/**
 * wakeup_tmemd - wake up a sleeping tmemd daemon
 *
 * @nid:	node id
 *
 * @returns:	none
 *
 * Each tmemd is assigned to a node on the system, so passing the node id tells
 * the function which node to wake up. We also check to make sure that tmemd is
 * not currently active and out of sleep before trying to wake it up.
 *
 * This is mainly used by the watermark watchdog to wake up tmemd if levels have
 * reach below satisfactory level (below high watermark).
 */
void wakeup_tmemd(int nid)
{
	//check to make sure tmemd in waiting
	if (!waitqueue_active(&tmemd_wait[nid]))
		return;
	
	//if it is waiting, wake up
	wake_up_interruptible(&tmemd_wait[nid]);
}


/**
 * tmemd_try_to_sleep - put tmemd to sleep if not needed
 *
 * @pgdat:	pglist_data node structure
 * @nid:	node id
 *
 * @returns:	none
 *
 * A helper function for tmemd to check if it can sleep when there is no need
 * for it to scan pages. We only want to it to try and migrate pages between
 * nodes only if memory pressure is great enough to do so.
 */
static void tmemd_try_to_sleep(pg_data_t *pgdat, int nid)
{
	long remaining = 0;
	DEFINE_WAIT(wait);

	if (freezing(current) || kthread_should_stop())
		return;
	
	prepare_to_wait(&tmemd_wait[nid], &wait, TASK_INTERRUPTIBLE);
	remaining = schedule_timeout(HZ);

	finish_wait(&tmemd_wait[nid], &wait);
	prepare_to_wait(&tmemd_wait[nid], &wait, TASK_INTERRUPTIBLE);

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
	int nid = pgdat->node_id;
	struct task_struct *task = current;
	const struct cpumask *cpumask = cpumask_of_node(nid);

	// Only allow node's CPUs to run this task
	if(!cpumask_empty(cpumask))
		set_cpus_allowed_ptr(task, cpumask);

	/*
	 * Tell MM that we are a memory allocator, and that we are actually
	 * kswapd. We are also set to suspend as needed.
	 *
	 * Flags are located in include/sched.h for more info.
	 */
	task->flags |= PF_MEMALLOC | PF_KSWAPD;


	pr_debug("tmemd started on node %d", nid);

	/*
	 * Loop every few seconds and scan the node's LRU lists.
	 * If the thread is signaled to stop, we will exit.
	 */
	for ( ; ; )
	{
		scan_node(pgdat);

		if(kthread_should_stop()) break;
		
//tmemd_try_sleep:
		//msleep(10000);
		tmemd_try_to_sleep(pgdat, nid);
		
	}

	task->flags &= ~(PF_MEMALLOC | PF_KSWAPD);
	current->reclaim_state = NULL;
	
	return 0;
}


/**
 * wmark_watchdogd - watermark watchdog daemon
 *
 * @p:		data (null)
 *
 * @returns:	0 on successful exit
 *
 * The purpose of this is to keep tabs on memory pressure accross all system
 * zones. This will keep tmemd from constantly having to scan pages when there
 * is no need to. When memory pressure falls below the HIGH WATERMARK on any
 * particular zone, then we want to wake up the tmemd on the appropriate node.
 *
 * Keeping tabs on the HIGH WATERMARK is to prevent kswapd from pulling out of
 * sleep to reclaim pages that we may want to move to a lower/higher tier
 * instead.
 *
 * One problem that will need to be addressed later are pages that may do some
 * "camping" on the lower tier if memory pressure is relieved. If pressure is
 * relieved on both tiers, then tmemd will not migrate pages until pressure is
 * high enough again. Some pages on the lower tier may need to be moved if they
 * are being accessed enough to warrrent moving them back up to the higher tier.
 * We'll need a way to wake up tmemd in order to move these pages. Preferably,
 * we may want to try and move as many pages back up to the upper tier as we
 * can.
 */
static int wmark_watchdogd(void *p)
{
	struct zone *zone;
	gfp_t flags = TMEMD_GFP_FLAGS;
	int nid;
	int highest_zoneidx;
	unsigned long watermark;
	bool ok;

	do {
		ktmm_for_each_populated_zone(zone) {

			highest_zoneidx = gfp_zone(flags);
			watermark = high_wmark_pages(zone);
			nid = zone_to_nid(zone);

			//pr_debug("wmark_watchdogd scanned zone on node: %d", nid);
			//pr_debug("zone name: %s", zone->name);
			//pr_debug("zone idx: %lu", zone_idx(zone));

			ok = ktmm_zone_watermark_ok_safe(zone, 0, 
					watermark, highest_zoneidx);

			//pr_debug("Is zone ok? : %d", ok);

			if (!ok) {
				pr_debug("BELOW WMARK: node %d, zone %s (%lu)", nid, zone->name, zone_idx(zone));
				wakeup_tmemd(nid);
			}
		}

		ssleep(1);

	} while(!kthread_should_stop());

	return 0;
}


/*****************************************************************************
 * Start & Stop
 *****************************************************************************/

/****************** ADD VMSCAN HOOKS HERE ************************/
static struct ktmm_hook vmscan_hooks[] = {
	HOOK("mem_cgroup_iter", ktmm_mem_cgroup_iter, &pt_mem_cgroup_iter),
	HOOK("balance_pgdat", ktmm_balance_pgdat, &pt_balance_pgdat),
	HOOK("zone_watermark_ok", ktmm_zone_watermark_ok_safe, &pt_zone_watermark_ok_safe),
	HOOK("first_online_pgdat", ktmm_first_online_pgdat, &pt_first_online_pgdat),
	HOOK("next_zone", ktmm_next_zone, &pt_next_zone),
	HOOK("shrink_inactive_list", ktmm_shrink_inactive_list, &pt_shrink_inactive_list),
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
	int ret;

	pr_debug("starting tmemd on available nodes");
	
	/* initialize wait queues for sleeping */
	for (i = 0; i < MAX_NUMNODES; i++)
		init_waitqueue_head(&tmemd_wait[i]);

	ret = install_hooks(vmscan_hooks, ARRAY_SIZE(vmscan_hooks));
	
	for_each_online_node(nid)
	{
		pg_data_t *pgdat = NODE_DATA(nid);

		/*
		struct pglist_data_ext pgdat_ext = {
			.pgdat = NODE_DATA(nid),
		};

		NODE_DATA_EXT(nid) = &pgdat_ext;
		*/
		
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


