

#include "ktmm_mm.h"
#include "ktmm_vmscan.h"

// these will not work correctly because we cannot modify GFP flags
#define ___GFP_PMEM 		0x1000000u
#define __GFP_PMEM ((__force gfp_t)___GFP_PMEM)



struct page* alloc_pmem_page(struct  page *page, unsigned long data)
{
		gfp_t gfp_mask = GFP_USER | __GFP_PMEM;
		//return alloc_pages_node(pmem_node_id, gfp_mask, 0);
		return alloc_page(gfp_mask);
}


struct page* alloc_normal_page(struct page *page, unsigned long data)
{
        gfp_t gfp_mask = GFP_USER;
        return alloc_page(gfp_mask);
}


static void init_node_lruvec_ext(struct pglist_data_ext *pgdat_ext)
{
	struct mem_cgroup *memcg; 
	struct lruvec *lruvec;
	struct lruvec_ext *lruvec_ext = &pgdat_ext->lruvec_ext;
	int nid = pgdat_ext->pgdat->node_id;

	/* get node lruvec */
	memcg = ktmm_mem_cgroup_iter(NULL, NULL, NULL);
	//lruvec = mem_cgroup_lruvec(memcg, pgdat_ext->pgdat);
	lruvec = &memcg->nodeinfo[nid]->lruvec;
	lruvec_ext->lruvec = lruvec;

	 /* init promote lists */
	INIT_LIST_HEAD(&lruvec_ext->promote_anon_list);
	INIT_LIST_HEAD(&lruvec_ext->promote_file_list);

	/* set pointer list to all lru lists (including promote) */
	lruvec_ext->lists[INACTIVE_ANON_LIST] = &lruvec->lists[LRU_INACTIVE_ANON];
	lruvec_ext->lists[ACTIVE_ANON_LIST] = &lruvec->lists[LRU_ACTIVE_ANON];
	lruvec_ext->lists[PROMOTE_ANON_LIST] = &lruvec_ext->promote_anon_list;
	lruvec_ext->lists[INACTIVE_FILE_LIST] = &lruvec->lists[LRU_INACTIVE_FILE];
	lruvec_ext->lists[ACTIVE_FILE_LIST] = &lruvec->lists[LRU_ACTIVE_FILE];
	lruvec_ext->lists[PROMOTE_FILE_LIST] = &lruvec_ext->promote_file_list;
	lruvec_ext->lists[UNEVICTABLE_LIST] = &lruvec->lists[LRU_UNEVICTABLE];
}


void init_node_data_ext(int nid)
{
	struct pglist_data_ext *pgdat_ext = NODE_DATA_EXT(nid);

	pgdat_ext->pgdat = NODE_DATA(nid);
	pgdat_ext->pmem_node = -1;

	pr_debug("init node data");

	init_waitqueue_head(&pgdat_ext->tmemd_wait);
	pr_debug("init node tmemd wait");

	init_node_lruvec_ext(pgdat_ext);
	pr_debug("init node lruvec_ext");
}

