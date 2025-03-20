/* tmem_vmscan header.h */
#ifndef TMEM_VMSCAN_HEADER_H
#define TMEM_VMSCAN_HEADER_H

#include <linux/memcontrol.h>

typedef struct mem_cgroup *(*tmem_cgroup_iter_t)(
		struct mem_cgroup *root,
		struct mem_cgroup *prev,
		struct mem_cgroup_reclaim_cookie *reclaim);

bool init_vmscan_symbols(void);

void available_nodes(void);

void tmem_try_to_stop(void);

#endif /* TMEM_VMSCAN_HEADER_H */
