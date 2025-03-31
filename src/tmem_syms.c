/*
 * tmem_syms.c
 *
 * Symbol lookup code for all inaccessible symbols in the kernel.
 *
 * This will be mostly used for gaining access to functions that are not
 * available to modules.
 */

#include <linux/kprobes.h>
#include <linux/memcontrol.h>

#include "tmem_syms.h"

// kallsyms_lookup_name
typedef unsigned long (*kallsyms_lookup_name_t)(const char *symbol_name);

// mem_cgroup_iter
typedef struct mem_cgroup *(*mem_cgroup_iter_t)(
		struct mem_cgroup *root,
		struct mem_cgroup *prev,
		struct mem_cgroup_reclaim_cookie *reclaim);


/**
 * Create instances of our "hidden" kernel symbols here.
 */
static kallsyms_lookup_name_t symbol_lookup;	//kallsyms_lookup_name
static mem_cgroup_iter_t cgroup_iter;		//mem_cgroup_iter


/**
 * kp_symslookup - kprobe for acquiring kallsyms_lookup_name
 *
 * When this is registered, it will establish a break point
 * in the kernel where this symbol is located.
 */
static struct kprobe kp_symslookup = {
	.symbol_name = "kallsyms_lookup_name"
};

/**
 * This will use kallsyms_lookup_name to acquire and establish needed
 * functions and structures that would otherwise be unavailable to
 * kernel modules. Symbol addresses are used to recreate the functions
 * and structures for the module.
 *
 * A kprobe is first used to obtain kallsyms_lookup_name, then subsequent
 * functions can be reconstructed by looking up symbols by name.
 *
 * Reconstructions made in this function should not be used directly
 * outside this portion of the module. Wrappers should be made available
 * to be used by the rest of the module.
 */
bool register_module_symbols() {
	unsigned long addr;

	// acquire kallsyms_lookup_name w/ kprobe
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
	// failure to register all symbols should return false
	pr_info("tmem module failed to register needed symbols");
	return false;
}

/******************************************************************************
 * WRAPPERS
 ******************************************************************************/
struct mem_cgroup *tmem_cgroup_iter(struct mem_cgroup *root,
			struct mem_cgroup *prev,
			struct mem_cgroup_reclaim_cookie *reclaim)
{
	return cgroup_iter(root, prev, reclaim);
}
