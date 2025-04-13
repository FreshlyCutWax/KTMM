/* ktmm_mm header.h */
#ifndef KTMM_MM_HEADER_H
#define KTMM_MM_HEADER_H

#include <linux/numa.h>
#include <linux/mmzone.h>
#include <linux/sched.h>

/**
 * pglist_data_ext - pglist_data extension for ktmm
 */
struct pglist_data_ext {
	struct pglist_data *pgdat;

	wait_queue_head_t tmemd_wait;
	struct task_struct *tmemd;
	int pmem_node;
};

/* which node is has persistent memory (GLOBAL) */
extern int pmem_node_id;

/* holds all the ptrs to ALL instances of pglist_data_ext */
extern struct pglist_data_ext *node_data_ext[MAX_NUMNODES];


/* returns a pointer, so make sure you use it correctly */
#define NODE_DATA_EXT(nid)	(node_data_ext[nid])


/* get the next page/folio in the list */
#define lru_to_page_next(head) (list_entry((head)->next, struct page, lru))
#define lru_to_folio_next(head) (list_entry((head)->next, struct folio, lru))

/* 
 * GFP bitmask for persistent memory
 *
 * This might need changing for later versions of the kernel.
 * This would be bit 29 of a 32-bit unsigned int (unreserved).
 *
 * Combination: GFP_PMEM | GFP_USER
 *
 * GFP_USER = __GFP_HARDWALL | __GFP_FS | __GFP_IO
 *
 * Checkout linux/gfp_types.h for more.
 */
#define GFP_PMEM	0x10000000u


#endif /* KTMM_MM_HEADER_H */
