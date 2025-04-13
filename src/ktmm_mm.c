/**
 * ktmm_mm - General memory management code
 *
 * Copyright (c) FreshlyCutWax
 */

#include "ktmm_mm.h"


void set_pmem_node_id(int nid)
{
	pmem_node_id = nid;
}


void set_pmem_node(int nid)
{
	NODE_DATA_EXT(nid)->pmem_node = 1;
}


