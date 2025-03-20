/*
 * tmem_syms.c
 */

#include <linux/kprobes.h>

#include "tmem_syms.h"

/**
 * These should be moved later to its own file for generic use.
 */
struct kprobe kp_symslookup = {
	.symbol_name = "kallsyms_lookup_name"
};


void tmem_kallsyms_probe(kallsyms_lookup_name_t *fn)
{
	register_kprobe(&kp_symslookup);
	*fn = (kallsyms_lookup_name_t) kp_symslookup.addr;
	unregister_kprobe(&kp_symslookup);
}
