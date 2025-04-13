/* tmem_syms header.h */
#ifndef TMEM_SYMS_HEADER_H
#define TMEM_SYMS_HEADER_H

#include <linux/ftrace.h>
#include <linux/memcontrol.h>

// this is arbitrary, maybe change later?
#define TMEM_BUFSIZE_MAX 128

unsigned long symbol_lookup(const char *name);

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
	size_t len;
	struct tmem_hook buf[TMEM_BUFSIZE_MAX];
};

/**
 * @name: 	kernel function symbol name
 * @callback:	function to call when mcount is executed
 *
 * Should be of type struct tmem_hook.
 */
#define HOOK(_name, _callback)			\
	{					\
		.kfunc_name = (_name),		\
		.callback = (_callback),	\
	}

/**
 * @arr: buf member (array) of struct tmem_hook_buffer
 *
 * Should be of type struct tmem_hook_buffer.
 */
#define INIT_BUF_LEN(arr) \
	(sizeof(arr) > 0) ? sizeof(arr)/sizeof(arr[0]) : 0

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


int tmem_balance_pgdat(pg_data_t *pgdat, int order, int highest_zoneidx);

#endif /* TMEM_SYMS_HEADER_H */
