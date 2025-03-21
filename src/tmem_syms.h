/* tmem_syms header.h */
#ifndef TMEM_SYMS_HEADER_H
#define TMEM_SYMS_HEADER_H

#include <linux/memcontrol.h>

/**
 * register_module_symbols - register hidden kernel symbols
 *
 * @return:	bool, if registering ALL symbols was successful
 *
 */
bool register_module_symbols(void);

/**
 * tmem_cgroup_iter - module wrapper for mem_cgroup_iter
 *
 * Please reference [src/include/memcontrol.h]
 */
struct mem_cgroup *tmem_cgroup_iter(struct mem_cgroup *root,
			struct mem_cgroup *prev,
			struct mem_cgroup_reclaim_cookie *reclaim);

#endif /* TMEM_SYMS_HEADER_H */
