/* tmem_syms header.h */
#ifndef TMEM_SYMS_HEADER_H
#define TMEM_SYMS_HEADER_H

#include <linux/memcontrol.h>

bool register_module_symbols(void);

struct mem_cgroup *tmem_cgroup_iter(struct mem_cgroup *root,
			struct mem_cgroup *prev,
			struct mem_cgroup_reclaim_cookie *reclaim);

#endif /* TMEM_SYMS_HEADER_H */
