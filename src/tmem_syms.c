/*
 * tmem_syms.c
 */

#include <linux/kprobes.h>
#include <linux/memcontrol.h>

#include "tmem_syms.h"


typedef unsigned long (*kallsyms_lookup_name_t)(const char *symbol_name);

typedef struct mem_cgroup *(*mem_cgroup_iter_t)(
		struct mem_cgroup *root,
		struct mem_cgroup *prev,
		struct mem_cgroup_reclaim_cookie *reclaim);


kallsyms_lookup_name_t symbol_lookup;	//kallsyms_lookup_name
mem_cgroup_iter_t cgroup_iter;		//mem_cgroup_iter

struct kprobe kp_symslookup = {
	.symbol_name = "kallsyms_lookup_name"
};

bool register_module_symbols() {
	unsigned long addr;

	register_kprobe(&kp_symslookup);
	symbol_lookup = (kallsyms_lookup_name_t) kp_symslookup.addr;
	unregister_kprobe(&kp_symslookup);
	
	
	// register mem_cgroup_iter
	addr = symbol_lookup("mem_cgroup_iter");
	if(!addr) goto failure;

	cgroup_iter = (struct mem_cgroup *(*)(struct mem_cgroup *,
			struct mem_cgroup *,
			struct mem_cgroup_reclaim_cookie *)
			)addr;

	//successfully registered all symbols
	return true;


failure:
	pr_info("tmem module failed to register needed symbols");
	return false;
}

struct mem_cgroup *tmem_cgroup_iter(struct mem_cgroup *root,
			struct mem_cgroup *prev,
			struct mem_cgroup_reclaim_cookie *reclaim)
{
	return cgroup_iter(root, prev, reclaim);
}
