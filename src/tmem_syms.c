/*
 * tmem_syms.c
 *
 * Symbol lookup code for all inaccessible symbols in the kernel.
 *
 * This will be mostly used for gaining access to functions that are not
 * available to modules.
 */

#include <linux/ftrace.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
//#include <linux/linkage.h>
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
static mem_cgroup_iter_t cgroup_iter;			//mem_cgroup_iter


int symbol_lookup(const char *name) 
{
	int ret = 0;
	kallsyms_lookup_name_t symbol_lookup_name;
	
	struct kprobe kp = {
		.symbol_name = "kallsyms_lookup_name"
	};
	
	register_kprobe(&kp);
	symbol_lookup_name = (kallsyms_lookup_name_t) kp.addr;
	unregister_kprobe(&kp);

	if (!symbol_lookup_name) ret = -EFAULT;

	return ret;
}


static void notrace wrap_hook(unsigned long ip,
				unsigned long parent_ip,
				struct ftrace_ops *ops,
				struct ftrace_regs *regs)
{
	struct tmem_hook *hook = container_of(ops, struct tmem_hook, ops);

	ftrace_instruction_pointer_set(regs, hook->callback_addr);
	//regs->ip = (unsigned long) hook->callback;
}


/*
 * 
 */
static int register_hook(struct tmem_hook *hook) 
{
	int err;

	hook->callback_addr = (unsigned long) hook->callback;

	hook->kfunc_addr = symbol_lookup(hook->kfunc_name);

	if (!hook->kfunc_addr)
		return -EFAULT;
	
	/*
	 * We want to effectively skip over the mcount instruction so we don't
	 * accidently call it again if the wrapper ends up calling the original
	 * function.
	 */
	hook->kfunc = (void *) (hook->kfunc_addr + MCOUNT_INSN_SIZE);
	
	hook->ops.func = wrap_hook;
	hook->ops.flags = FTRACE_OPS_FL_SAVE_REGS | FTRACE_OPS_FL_IPMODIFY;

	err = ftrace_set_filter_ip(&hook->ops, hook->kfunc_addr, 0, 0);
	if (err)
		return err;

	err = register_ftrace_function(&hook->ops);
	if (err) {
		ftrace_set_filter_ip(&hook->ops, hook->kfunc_addr, 1, 0);
		
		return err;
	}

	return 0;
}


static int unregister_hook(struct tmem_hook *hook)
{
	int ret;

	ret = unregister_ftrace_function(&hook->ops);
	ftrace_set_filter_ip(&hook->ops, hook->kfunc_addr, 1, 0);

	// can be an error
	return ret;
}


int uninstall_hooks(struct tmem_hook_buffer *buf)
{
	bool err;
	int ret;
	size_t i;
	size_t stop = buf->len;
	struct tmem_hook *hook;

	for (i = 0; i < stop; i++) {
		hook = &buf->buf[i];
		ret = unregister_hook(hook);

		if (ret) err = true;
	}

	if (err) return -EFAULT;

	return 0;
}


int install_hooks(struct tmem_hook_buffer *buf)
{
	int err;
	size_t i;
	size_t stop = buf->len;
	struct tmem_hook *hook;

	for (i = 0; i < stop; i++) {
		hook = &buf->buf[i];
		err = register_hook(hook);

		if (err) goto hook_install_error;
	}

	return 0;

hook_install_error:
	buf->err = true;
	buf->len = i+1;
	uninstall_hooks(buf);
	
	return err;
}


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
bool register_module_symbols() 
{
	unsigned long addr;

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
