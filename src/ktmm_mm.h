#ifndef KTMM_MM_HEADER_H
#define KTMM_MM_HEADER_H

#include <linux/mmzone.h>

#include "ktmm_vmhooks.h"
#include "ktmm_vmscan.h"

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
extern struct pglist_data_ext node_data_ext[MAX_NUMNODES];


/* returns a pointer, so make sure you use it correctly */
#define NODE_DATA_EXT(nid)	(&node_data_ext[nid])



/* get the next page/folio in the list */
#define lru_to_page_next(head) (list_entry((head)->next, struct page, lru))


void init_node_data_ext(int nid);

#endif /* KTMM_MM_HEADER_H */
