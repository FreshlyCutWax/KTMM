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
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/swap.h>
#include <linux/vmstat.h>
#include <linux/wait.h>

#include "ktmm_hook.h"
#include "ktmm_vmscan.h"

// possibly needs to be GFP_USER?
#define TMEMD_GFP_FLAGS GFP_NOIO

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


// these will not work correctly because we cannot modify GFP flags
#define ___GFP_PMEM 		0x1000000u
#define __GFP_PMEM ((__force gfp_t)___GFP_PMEM)


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

/************** IMPORTED/HOOKED PROTOTYPES HERE *****************************/
static struct mem_cgroup *(*pt_mem_cgroup_iter)(struct mem_cgroup *root,
				struct mem_cgroup *prev,
				struct mem_cgroup_reclaim_cookie *reclaim);


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
static struct mem_cgroup *ktmm_mem_cgroup_iter(struct mem_cgroup *root,
				struct mem_cgroup *prev,
				struct mem_cgroup_reclaim_cookie *reclaim)
{
	return pt_mem_cgroup_iter(root, prev, reclaim);
}


static bool ktmm_zone_watermark_ok_safe(struct zone *z,
					unsigned int order,
					unsigned long mark,
					int highest_zoneidx)
{
	return pt_zone_watermark_ok_safe(z, order, mark, highest_zoneidx);
}


static struct pglist_data *ktmm_first_online_pgdat(void)
{
	return pt_first_online_pgdat();
}


static struct zone *ktmm_next_zone(struct zone *zone)
{
	return pt_next_zone(zone);
}


/*****************************************************************************
 * NODE & LRUVEC
 *****************************************************************************/

#define LRU_PROMOTE 2
#define KTMM_LRU_FILE 3

enum ktmm_lru_list {
	INACTIVE_ANON_LIST = LRU_BASE,
	ACTIVE_ANON_LIST = LRU_BASE + LRU_ACTIVE,
	PROMOTE_ANON_LIST = LRU_BASE + LRU_PROMOTE,
	INACTIVE_FILE_LIST = LRU_BASE + KTMM_LRU_FILE,
	ACTIVE_FILE_LIST = LRU_BASE + KTMM_LRU_FILE + LRU_ACTIVE,
	PROMOTE_FILE_LIST = LRU_BASE + KTMM_LRU_FILE + LRU_PROMOTE,
	UNEVICTABLE_LIST,
	NR_LISTS_LRU,
};


struct lruvec_ext {
	struct lruvec *lruvec;
	struct list_head promote_anon_list;
	struct list_head promote_file_list;
	struct list_head *lists[NR_LISTS_LRU];
};


/**
 * pglist_data_ext - pglist_data extension for ktmm
 */
struct pglist_data_ext {
	struct pglist_data *pgdat;
	struct lruvec_ext lruvec_ext;

	wait_queue_head_t tmemd_wait;
	struct task_struct *tmemd;

	int pmem_node;
};

/* global list of extension data for nodes */
static struct pglist_data_ext node_data_ext[MAX_NUMNODES];


/* returns a pointer, so make sure you use it correctly */
#define NODE_DATA_EXT(nid)	(&node_data_ext[nid])


static void init_node_lruvec_ext(struct pglist_data_ext *pgdat_ext)
{
	struct mem_cgroup *memcg; 
	struct lruvec *lruvec;
	struct lruvec_ext *lruvec_ext = &pgdat_ext->lruvec_ext;
	int nid = pgdat_ext->pgdat->node_id;

	/* get node lruvec */
	memcg = ktmm_mem_cgroup_iter(NULL, NULL, NULL);
	//lruvec = mem_cgroup_lruvec(memcg, pgdat_ext->pgdat);
	lruvec = &memcg->nodeinfo[nid]->lruvec;
	lruvec_ext->lruvec = lruvec;

	 /* init promote lists */
	INIT_LIST_HEAD(&lruvec_ext->promote_anon_list);
	INIT_LIST_HEAD(&lruvec_ext->promote_file_list);

	/* set pointer list to all lru lists (including promote) */
	lruvec_ext->lists[INACTIVE_ANON_LIST] = &lruvec->lists[LRU_INACTIVE_ANON];
	lruvec_ext->lists[ACTIVE_ANON_LIST] = &lruvec->lists[LRU_ACTIVE_ANON];
	lruvec_ext->lists[PROMOTE_ANON_LIST] = &lruvec_ext->promote_anon_list;
	lruvec_ext->lists[INACTIVE_FILE_LIST] = &lruvec->lists[LRU_INACTIVE_FILE];
	lruvec_ext->lists[ACTIVE_FILE_LIST] = &lruvec->lists[LRU_ACTIVE_FILE];
	lruvec_ext->lists[PROMOTE_FILE_LIST] = &lruvec_ext->promote_file_list;
	lruvec_ext->lists[UNEVICTABLE_LIST] = &lruvec->lists[LRU_UNEVICTABLE];
}


static void init_node_data_ext(int nid)
{
	struct pglist_data_ext *pgdat_ext = NODE_DATA_EXT(nid);

	pgdat_ext->pgdat = NODE_DATA(nid);
	pgdat_ext->pmem_node = -1;

	pr_debug("init node data");

	init_waitqueue_head(&pgdat_ext->tmemd_wait);
	pr_debug("init node tmemd wait");

	init_node_lruvec_ext(pgdat_ext);
	pr_debug("init node lruvec_ext");
}

/* get the next page/folio in the list */
#define lru_to_page_next(head) (list_entry((head)->next, struct page, lru))


/*****************************************************************************
 * Node Scanning, Shrinking, and Promotion
 *****************************************************************************/

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


bool watching_zonetype(struct zone *z)
{
	int i;
	int last = ARRAY_SIZE(ktmm_zone_watchlist);
	unsigned long zid = zone_idx(z);

	for (i = 0; i < last; i++)
		if (ktmm_zone_watchlist[i] == zid) return true;

	return false;
}


#define for_each_observed_zone(zone) 				\
	for (zone = (ktmm_first_online_pgdat())->node_zones; 	\
		zone;						\
		zone = ktmm_next_zone(zone))			\
			if(!populated_zone(zone) || !watching_zonetype(zone)); \
				/* do nothing */		\
			else


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


/* SIMILAR TO: shrink_active_list */
static void scan_active_list(unsigned long nr_to_scan, 
				struct lruvec_ext *lruvec_ext,
				struct scan_control *sc,
				enum lru_list lru)
{
	/*
	unsigned long nr_taken;
	unsigned long nr_scanned;
	unsigned long vm_flags;
	unsigned long lock_flags;
	LIST_HEAD(l_hold);	* The folios which were snipped off
	LIST_HEAD(l_active);
	LIST_HEAD(l_inactive);

	// multi-clock
	unsigned long nr_promote;
	unsigned long pmem_page_sal = 0;
	unsigned long active_to_promote = 0;
	LIST_HEAD(l_promote);
	// end multi-clock

	unsigned nr_deactivate, nr_activate;
	unsigned nr_rotated = 0;
	int file = is_file_lru(lru);
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);

	* make sure pages in per-cpu lru list are added
	lru_add_drain();

	spin_lock_irqsave(&lruvec->lru_lock, flags);

	nr_taken = isolate_lru_folios(nr_to_scan, lruvec, &l_hold,
				     &nr_scanned, sc, lru);

	__mod_node_page_state(pgdat, NR_ISOLATED_ANON + file, nr_taken);

	if (!cgroup_reclaim(sc))
		__count_vm_events(PGREFILL, nr_scanned);
	__count_memcg_events(lruvec_memcg(lruvec), PGREFILL, nr_scanned);

	spin_unlock_irqrestore(&lruvec->lru_lock, flags);

	while (!list_empty(&l_hold)) {
		struct folio *folio;

		cond_resched();
		folio = lru_to_folio(&l_hold);
		list_del(&folio->lru);

		if (unlikely(!folio_evictable(folio))) {
			folio_putback_lru(folio);
			continue;
		}

		if (unlikely(buffer_heads_over_limit)) {
			if (folio_needs_release(folio) &&
			    folio_trylock(folio)) {
				filemap_release_folio(folio, 0);
				folio_unlock(folio);
			}
		}

		// MULTI-CLOCK
		if (pgdat_ext->pm_node != 0) {
			pmem_page_sal++;
			if (page_referenced(page, 0, sc->target_mem_cgroup, & vm_flags)) {
				//SetPagePromote(page); NEEDS TO BE MODULE TRACKED
				list_add(&page->lru, &l_promote);
				active_to_promote++;
				continue;
			}
		}

		// might not need, we only care about promoting here in the
		// module
		if (sc->only_promote) {
			list_add(&page->lru, &l_active);
		}
		// END MULTI-CLOCK

		* Referenced or rmap lock contention: rotate
		if (folio_referenced(folio, 0, sc->target_mem_cgroup,
				     &vm_flags) != 0) {
			 *
			 * Identify referenced, file-backed active folios and
			 * give them one more trip around the active list. So
			 * that executable code get better chances to stay in
			 * memory under moderate memory pressure.  Anon folios
			 * are not likely to be evicted by use-once streaming
			 * IO, plus JVM can create lots of anon VM_EXEC folios,
			 * so we ignore them here.
			 *
			if ((vm_flags & VM_EXEC) && folio_is_file_lru(folio)) {
				nr_rotated += folio_nr_pages(folio);
				list_add(&folio->lru, &l_active);
				continue;
			}
		}

		folio_clear_active(folio);	* we are de-activating
		folio_set_workingset(folio);
		list_add(&folio->lru, &l_inactive);
	}

	 *
	 * Move folios back to the lru list.
	 *
	spin_lock_irqsave(&lruvec->lru_lock, flags);

	nr_activate = move_folios_to_lru(lruvec, &l_active);
	nr_deactivate = move_folios_to_lru(lruvec, &l_inactive);

	 * Keep all free folios in l_active list
	list_splice(&l_inactive, &l_active);

	__count_vm_events(PGDEACTIVATE, nr_deactivate);
	__count_memcg_events(lruvec_memcg(lruvec), PGDEACTIVATE, nr_deactivate);

	__mod_node_page_state(pgdat, NR_ISOLATED_ANON + file, -nr_taken);

	spin_unlock_irqrestore(&lruvec->lru_lock, flags);

	mem_cgroup_uncharge_list(&l_active);
	free_unref_page_list(&l_active);
	trace_mm_vmscan_lru_shrink_active(pgdat->node_id, nr_taken, nr_activate,
			nr_deactivate, nr_rotated, sc->priority, file);
	return;
	*/
	return;
}


/* SIMILAR TO: shrink_inactive_list() */
static unsigned long scan_inactive_list(unsigned long nr_to_scan, 
					struct lruvec_ext *lruvec_ext,
					struct scan_control *sc,
					enum lru_list lru)
{
	/*
	LIST_HEAD(folio_list);
	unsigned long nr_scanned;
	unsigned int nr_reclaimed = 0;
	unsigned long nr_taken;
	int nr_migrated;
	struct reclaim_stat stat;
	bool file = is_file_lru(lru);
	enum vm_event_item item;
	struct pglist_data *pgdat = lruvec_pgdat(lruvec_ext->lruvec);
	struct pglist_data_ext *pgext = pglist_data_ext(lruvec_ext->lruvec);
	bool stalled = false;

	 * unlikely() checks if the branch is unlikely to be executed (branch
	 * prediction).
	 *
	 * functions that need to be exposed or rewritten here:
	 * 	too_many_isolated (rewrite, needs sc)
	 * 	reclaim_throttle (expose using hook)
	 * 	fatal_signal_pending (expose? from signal.h)
	 *
	while (unlikely(too_many_isolated(pgdat, file, sc))) {
		if (stalled)
			return 0;

		* wait a bit for the reclaimer. 
		stalled = true;
		reclaim_throttle(pgdat, VMSCAN_THROTTLE_ISOLATED);

		* We are about to die and free our memory. Return now.
		if (fatal_signal_pending(current))
			return SWAP_CLUSTER_MAX;
	}

	* make sure pages in per-cpu lru list are added
	lru_add_drain();

	 * We want to isolate the pages we are going to scan.
	 *
	 * functions that need to be exposed or rewritten here:
	 * 	isolate_lru_folio / or pages? (rewrite)
	 * 	
	 *
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

	if (nr_taken == 0) return 0;

	// MULTI-CLOCK
	if (pgext->pmem_node == 0) {
		int ret = migrate_pages(&folio_list, vmscan_alloc_pmem_page, NULL, 
								0, MIGRATE_SYNC, MR_MEMORY_HOTPLUG);
		nr_reclaimed = (ret >= 0 ? nr_taken - ret : 0);
		__mod_node_page_state(pgdat, NR_DEMOTED, nr_reclaimed);
	}
	// END MULTI-CLOCK

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

	 * If dirty pages are scanned that are not queued for IO, it
	 * implies that flushers are not doing their job. This can
	 * happen when memory pressure pushes dirty pages to the end of
	 * the LRU before the dirty limits are breached and the dirty
	 * data has expired. It can also happen when the proportion of
	 * dirty pages grows not through writes but through memory
	 * pressure reclaiming all the clean cache. And in some cases,
	 * the flushers simply cannot keep up with the allocation
	 * rate. Nudge the flusher threads in case they are asleep.
	 *
	if (stat.nr_unqueued_dirty == nr_taken) {
		wakeup_flusher_threads(WB_REASON_VMSCAN);
		 *
		 * For cgroupv1 dirty throttling is achieved by waking up
		 * the kernel flusher here and later waiting on folios
		 * the kernel flusher here and later waiting on pages
		 * which are in writeback to finish (see shrink_folio_list()).
		 *
		 * Flusher may not be able to issue writeback quickly
		 * enough for cgroupv1 writeback throttling to work
		 * on a large system.
		 *
		 * NEED TO REWRITE: writeback_throttling_sane()
		 * EXPOSE USING HOOKS: reclaim_throttle()
		 *
		if (!writeback_throttling_sane(sc))
			reclaim_throttle(pgdat, VMSCAN_THROTTLE_WRITEBACK);
	}

	// possibly remove or rewrite this portion
	// if we dont reclaim, then we don't have these
	sc->nr.dirty += stat.nr_dirty;
	sc->nr.congested += stat.nr_congested;
	sc->nr.unqueued_dirty += stat.nr_unqueued_dirty;
	sc->nr.writeback += stat.nr_writeback;
	sc->nr.immediate += stat.nr_immediate;
	sc->nr.taken += nr_taken;
	if (file)
		sc->nr.file_taken += nr_taken;

	// same here, if we didn't reclaim anything, then why?
	trace_mm_vmscan_lru_shrink_inactive(pgdat->node_id,
			nr_scanned, nr_reclaimed, &stat, sc->priority, file);

	return nr_migrated;
	*/
	return 0;
}


/* SIMILAR TO: shrink_list() */
static unsigned long scan_lru_list(enum lru_list lru, 
				unsigned long nr_to_scan,
				struct lruvec_ext *lruvec_ext, 
				struct scan_control *sc)
{
	if (is_active_lru(lru))
		scan_active_list(nr_to_scan, lruvec_ext, sc, lru);

	return scan_inactive_list(nr_to_scan, lruvec_ext, sc, lru);
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
		for_each_observed_zone(zone) {

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
	HOOK("zone_watermark_ok", ktmm_zone_watermark_ok_safe, &pt_zone_watermark_ok_safe),
	HOOK("first_online_pgdat", ktmm_first_online_pgdat, &pt_first_online_pgdat),
	HOOK("next_zone", ktmm_next_zone, &pt_next_zone),
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
		init_node_data_ext(nid);

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


