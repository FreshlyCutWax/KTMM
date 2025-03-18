#include <linux/kprobes.h>

/**
 * These should be moved later to its own file for generic use.
 */
typedef unsigned long (*kallsyms_lookup_name_t)(const char *sym_name);
kallsyms_lookup_name_t tmem_kallsyms_lookup_name;


static struct kprobe kp_symslookup = {
	.symbol_name = "kallsyms_lookup_name"
};


void init_module_syms()
{
    // register kallsyms_lookup_name
	register_kprobe(&kp_sysmlookup);
	tmem_kallsyms_lookup_name = (kallsyms_lookup_name_t) kp.addr;
	unregister_kprobe(&kp_sysmlookup);
}
