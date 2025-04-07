/* tmem_syms header.h */
#ifndef TMEM_SYMS_HEADER_H
#define TMEM_SYMS_HEADER_H

#include <linux/ftrace.h>
#include <linux/memcontrol.h>

// this is arbitrary, maybe change later?
#define TMEM_BUFSIZE_MAX 128

int symbol_lookup(const char *name);

/**
 * Do not use this directly. Use the macro to create a hook instead.
 */
struct tmem_hook {
	const char *kfunc_name;
	void *callback;

	void *kfunc;
	unsigned long kfunc_addr;
	unsigned long callback_addr;
	struct ftrace_ops ops;
};

struct tmem_hook_buffer {
	bool err;
	size_t len;
	struct tmem_hook buf[TMEM_BUFSIZE_MAX];
};

/**
 * @name: 	kernel function symbol name
 * @callback:	function to call when mcount is executed
 */
#define HOOK(name,callback)		\
	{				\
		.kfunc_name = (name),	\
		.callback = (callback),	\
	}

int uninstall_hooks(struct tmem_hook_buffer *buf);

int install_hooks(struct tmem_hook_buffer *buf);

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
