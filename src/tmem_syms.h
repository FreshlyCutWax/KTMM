/* tmem_syms header.h */
#ifndef TMEM_SYMS_HEADER_H
#define TMEM_SYMS_HEADER_H


typedef unsigned long (*kallsyms_lookup_name_t)(const char *symbol_name);

void tmem_kallsyms_probe(kallsyms_lookup_name_t *fn);


#endif /* TMEM_SYMS_HEADER_H */
