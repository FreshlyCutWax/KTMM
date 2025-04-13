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

/* which node is has persistent memory */
extern int pmem_node_id;

/* holds all the ptrs to ALL instances of pglist_data_ext */
extern struct pglist_data_ext *node_data_ext[MAX_NUMNODES];


/* returns a pointer, so make sure you use it correctly */
#define NODE_DATA_EXT(nid)	(node_data_ext[nid])


#endif /* KTMM_MM_HEADER_H */
