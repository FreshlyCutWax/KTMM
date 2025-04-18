
#include <linux/mmzone.h>
#include <linux/memcontrol.h>

#include "ktmm_vmhooks.h"
#include "ktmm_hook.h"
#include "ktmm_vmscan.h"

/******************* KERNEL PROTOTYPES HERE *****************************/
struct mem_cgroup *(*pt_mem_cgroup_iter)(struct mem_cgroup *root,
				struct mem_cgroup *prev,
				struct mem_cgroup_reclaim_cookie *reclaim);


/* FROM: page_alloc.c */
bool (*pt_zone_watermark_ok_safe)(struct zone *z,
					unsigned int order,
					unsigned long mark,
					int highest_zoneidx);


/* FROM: mmzone.c */
struct pglist_data *(*pt_first_online_pgdat)(void);


/* FROM: mmzone.c */
struct zone *(*pt_next_zone)(struct zone *zone);


/******************* MODULE PROTOTYPES HERE *****************************/
struct mem_cgroup *ktmm_mem_cgroup_iter(struct mem_cgroup *root,
				struct mem_cgroup *prev,
				struct mem_cgroup_reclaim_cookie *reclaim)
{
	return pt_mem_cgroup_iter(root, prev, reclaim);
}


bool ktmm_zone_watermark_ok_safe(struct zone *z,
					unsigned int order,
					unsigned long mark,
					int highest_zoneidx)
{
	return pt_zone_watermark_ok_safe(z, order, mark, highest_zoneidx);
}


struct pglist_data *ktmm_first_online_pgdat(void)
{
	return pt_first_online_pgdat();
}


struct zone *ktmm_next_zone(struct zone *zone)
{
	return pt_next_zone(zone);
}


struct ktmm_hook vmscan_hooks[] = {
	HOOK("mem_cgroup_iter", ktmm_mem_cgroup_iter, &pt_mem_cgroup_iter),
	HOOK("zone_watermark_ok", ktmm_zone_watermark_ok_safe, &pt_zone_watermark_ok_safe),
	HOOK("first_online_pgdat", ktmm_first_online_pgdat, &pt_first_online_pgdat),
	HOOK("next_zone", ktmm_next_zone, &pt_next_zone),
};


int install_vmscan_hooks()
{
	return install_hooks(vmscan_hooks, ARRAY_SIZE(vmscan_hooks));
}


int uninstall_vmscan_hooks()
{
	return install_hooks(vmscan_hooks, ARRAY_SIZE(vmscan_hooks));
}
