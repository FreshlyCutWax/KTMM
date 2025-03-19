/* tmem_syms header.h */
#ifndef TMEM_SYMS_HEADER_H
#define TMEM_SYMS_HEADER_H


typedef unsigned long (*kallsyms_lookup_name_t)(const char *sym_name);


kallsyms_lookup_name_t tmem_kallsyms_lookup_name;


void init_module_syms(void);


#endif /* TMEM_SYMS_HEADER_H */
