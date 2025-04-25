/*
 *  ktmm_vmscan.c
 *
 *  Page scanning and related functions.
 */

#define pr_fmt(fmt) "[ KTMM Mod ] vmscan - " fmt

#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/cgroup.h>
#include <linux/delay.h>
#include <linux/freezer.h>
#include <linux/gfp.h>
#include <linux/hashtable.h> //***
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

// which node is the pmem node
int pmem_node = -1;

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


//promote list hash table
DEFINE_HASHTABLE(promote_vec, 10);

/*
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
*/

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


static void (*pt_mem_cgroup_css_free)(struct cgroup_subsys_state *css);


static void (*pt_free_unref_page_list)(struct list_head *list);


static void (*pt_lru_add_drain)(void);


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


static void ktmm_free_unref_page_list(struct list_head *list)
{
	return pt_free_unref_page_list(list);
}


static void ktmm_lru_add_drain(void)
{
	pt_lru_add_drain();
}


/*****************************************************************************
 * NODE & LRUVEC
 *****************************************************************************/

enum lru_promote_list {
    LRU_PROMOTE_ANON_LIST = 0,
    LRU_PROMOTE_FILE_LIST = 1,
    NR_PROMOTE_LISTS,
};


struct promote_lists {
	struct list_head anon_list;
	struct list_head file_list;

	spinlock_t lock;
	struct hlist_node hnode;
	unsigned long key;
};


static struct promote_lists *lruvec_promote_lists(struct lruvec *lruvec)
{
	struct promote_lists *entry;
	unsigned long key = (unsigned long) lruvec;

	// try to get existing promote lists 
	hash_for_each_possible(promote_vec, entry, hnode, key) {
		if (entry->key == key) return entry;
	}
	
	// if the promote lists do not exist, create them 
	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry) {
		kfree(entry);
		return NULL;
	}

	// initialize struct elements
	INIT_LIST_HEAD(&entry->anon_list);
	INIT_LIST_HEAD(&entry->file_list);
	spin_lock_init(&entry->lock);
	entry->key = key;

	// add lists to hash table
	hash_add(promote_vec, &entry->hnode, key);

	return entry;
}


/* called when cgroup is freed */
static void promote_lists_free(struct mem_cgroup *memcg)
{
	int nid;
	struct lruvec *lruvec;
	struct mem_cgroup_per_node *memcg_pn;
	struct promote_lists *entry;
	unsigned long key;

	for_each_online_node(nid) {
		memcg_pn = memcg->nodeinfo[nid];

		if (!memcg_pn) continue;

		lruvec = &memcg_pn->lruvec;
		key = (unsigned long) lruvec;
		
		hash_for_each_possible(promote_vec, entry, hnode, key) {

			if (entry->key == key) {
				spin_lock(&entry->lock);

				//check page references free pages

				spin_unlock(&entry->lock);

				// free allocated list
				kfree(entry);
			}
		}
	}
}


/* This is a hooked function */
static void ktmm_mem_cgroup_css_free(struct cgroup_subsys_state *css)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(css);
	promote_lists_free(memcg);

	pt_mem_cgroup_css_free(css);
}


/*
static int move_promote_to_active(struct lruvec *lruvec, 
					struct list_head *list)
{
	struct folio *folio, *next;

	lruvec = &memcg->nodeinfo[nid]->lruvec;
	
	list_for_each(folio, next, list) {
		
	}
}
*/


/* called on exit to move pages back to kernel lru lists */
static int remove_promote_lists(struct lruvec *lruvec)
{
	struct promote_lists *entry;
	unsigned long key = (unsigned long) lruvec;

	// try to get promote lists if they existed
	hash_for_each_possible(promote_vec, entry, hnode, key) {

		if (entry->key == key) {
			spin_lock(&lruvec->lru_lock);
			spin_lock(&entry->lock);

			//move_promote_to_active(lruvec, &entry->anon_list);
			//move_promote_to_active(lruvec, &entry->anon_list);

			spin_unlock(&entry->lock);
			spin_unlock(&lruvec->lru_lock);

			// free allocated list
			kfree(entry);
		}
	}
	return 0;
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
	unsigned long nr_to_scan;
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

	unsigned long nr_lru_pages;
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


static bool ktmm_cgroup_below_low(struct mem_cgroup *memcg)
{
	return READ_ONCE(memcg->memory.elow) >=
		page_counter_read(&memcg->memory);
}


static bool ktmm_cgroup_below_min(struct mem_cgroup *memcg)
{
	return READ_ONCE(memcg->memory.emin) >=
		page_counter_read(&memcg->memory);
}


static void scan_promotion_lists(struct lruvec *lruvec,
				struct promote_lists *pr_lists,
				struct scan_control *sc)
{
	return;
}


/* SIMILAR TO: shrink_active_list */
static void scan_active_list(struct lruvec *lruvec,
				struct promote_lists *pr_lists,
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
static unsigned long scan_inactive_list(struct lruvec *lruvec,
					struct scan_control *sc,
					enum lru_list lru)
{
	LIST_HEAD(folio_list);
	//unsigned long nr_scanned;
	unsigned long nr_taken = 0;
	unsigned long nr_migrated = 0;
	//unsigned long nr_reclaimed = 0;
	//isolate_mode_t isolate_mode = 0;
	//bool file = is_file_lru(lru);
	struct pglist_data *pgdat = lruvec_pgdat(lruvec);
	int nid = pgdat->node_id;

	// make sure pages in per-cpu lru list are added
	ktmm_lru_add_drain();

	// We want to isolate the pages we are going to scan.
	spin_lock_irq(&lruvec->lru_lock);

	/*
	nr_taken = isolate_lru_folios(nr_to_scan, lruvec_ext->lruvec, &folio_list,
				     &nr_scanned, sc, lru);

	__mod_node_page_state(pgdat, NR_ISOLATED_ANON + file, nr_taken);
	*/

	spin_unlock_irq(&lruvec->lru_lock);

	if (nr_taken == 0) return 0;

	// migrate pages down to the pmem node
	if (pmem_node == nid) {
		/*
		int ret = migrate_pages(&folio_list, vmscan_alloc_pmem_page, NULL, 
					0, MIGRATE_SYNC, MR_MEMORY_HOTPLUG);
		nr_migrated = (ret >= 0 ? nr_taken - ret : 0);
		__mod_node_page_state(pgdat, NR_DEMOTED, nr_reclaimed);
		*/
	}

	spin_lock_irq(&lruvec->lru_lock);
	/*
	move_folios_to_lru(lruvec, &folio_list);
	__mod_node_page_state(pgdat, NR_ISOLATED_ANON + file, -nr_taken);
	*/
	spin_unlock_irq(&lruvec->lru_lock);

	mem_cgroup_uncharge_list(&folio_list);
	ktmm_free_unref_page_list(&folio_list);

	return nr_migrated;
}


/* SIMILAR TO: shrink_list() */
static unsigned long scan_lru_list(enum lru_list lru, 
				struct lruvec *lruvec, 
				struct promote_lists *pr_lists,
				struct scan_control *sc)
{
	if (is_active_lru(lru))
		scan_active_list(lruvec, pr_lists, sc, lru);

	return scan_inactive_list(lruvec, sc, lru);
}


/* similar to: shrink_lruvec() 
 * 
 * This might later consume scan_lru_list(), as it might be more simple to put
 * its code here instead.
 */
static void scan_lruvec(struct lruvec *lruvec, 
			struct promote_lists *pr_lists,
			struct scan_control *sc)
{
	enum lru_list lru;

	for_each_evictable_lru(lru) {
		scan_lru_list(lru, lruvec, pr_lists, sc);
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
	struct mem_cgroup *memcg;
	int memcg_count;
	int nid = pgdat->node_id;

	struct mem_cgroup_reclaim_cookie reclaim = {
		.pgdat = pgdat,
	};

	struct scan_control sc = {
		//.nr_to_reclaim = SWAP_CLUSTER_MAX,
		.nr_to_scan = 1024,
		.nr_to_reclaim = 0,
		.gfp_mask = TMEMD_GFP_FLAGS,
		.priority = DEF_PRIORITY,
		.may_writepage = !laptop_mode, //do not delay writing to disk
		.may_unmap = 1,
		.may_swap = 1,
		//.reclaim_idx = MAX_NR_ZONES - 1,
		.reclaim_idx = gfp_zone(TMEMD_GFP_FLAGS),
		.target_mem_cgroup = NULL,
	};

	memset(&sc.nr, 0, sizeof(sc.nr));

	// needs exposed to the module
	//set_task_reclaim_state(current, &reclaim_state);
	//task->reclaim_state = &reclaim_state;

	// get the root memory cgroup
	memcg = ktmm_mem_cgroup_iter(sc.target_mem_cgroup, NULL, &reclaim);
	
	pr_info("scanning lists on node %d", nid);

	pr_info("Counting memory cgroups...");
	memcg_count = 0;
	do {
		struct lruvec *lruvec = &memcg->nodeinfo[nid]->lruvec;
		struct promote_lists *pr_lists = lruvec_promote_lists(lruvec);
		unsigned long reclaimed;
		unsigned long scanned;

		memcg_count += 1;
		pr_info("Count: %d", memcg_count);

		if (ktmm_cgroup_below_min(memcg)) {
			/*
			 * Hard protection.
			 * If there is no reclaimable memory, OOM.
			 */
			continue;
		} else if (ktmm_cgroup_below_low(memcg)) {
			/*
			 * Soft protection.
			 * Respect the protection only as long as
			 * there is an unprotected supply of 
			 * reclaimable memory from other cgroups.
			 */
			if (!sc.memcg_low_reclaim) {
				sc.memcg_low_skipped = 1;
			}
		}
		// memcg_memory_event(memcg, MEMCG_LOW);
		

		reclaimed = sc.nr_reclaimed;
		scanned = sc.nr_scanned;

		scan_lruvec(lruvec, pr_lists, &sc);
		scan_promotion_lists(lruvec, pr_lists, &sc);

		pr_debug("memcg count: %d", memcg_count);

	} while ((memcg = ktmm_mem_cgroup_iter(NULL, memcg, NULL)));
}


static void cleanup_node_lists(pg_data_t *pgdat)
{
	struct mem_cgroup *memcg;
	struct lruvec *lruvec;
	int memcg_count;
	int nid = pgdat->node_id;

	// get the root memory cgroup
	memcg = ktmm_mem_cgroup_iter(NULL, NULL, NULL);

	memcg_count = 0;
	do {
		// acquire the lruvec structure
		lruvec = &memcg->nodeinfo[nid]->lruvec;

		remove_promote_lists(lruvec);

		memcg_count += 1;
		pr_debug("Removing lists: %d", memcg_count);
	} while ((memcg = ktmm_mem_cgroup_iter(NULL, memcg, NULL)));
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

	cleanup_node_lists(pgdat);

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
	HOOK("mem_cgroup_css_free", ktmm_mem_cgroup_css_free, &pt_mem_cgroup_css_free),
	HOOK("free_unref_page_list", ktmm_free_unref_page_list, &pt_free_unref_page_list),
	HOOK("lru_add_drain", ktmm_lru_add_drain, &pt_lru_add_drain),
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


