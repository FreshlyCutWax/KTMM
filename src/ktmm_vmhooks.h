#ifndef KTMM_VMHOOKS_HEADER_H
#define KTMM_VMHOOKS_HEADER_H


#include <linux/mmzone.h>
#include <linux/memcontrol.h>

#include "ktmm_hook.h"


/******************* MODULE PROTOTYPES HERE *****************************/
struct mem_cgroup *ktmm_mem_cgroup_iter(struct mem_cgroup *root,
				struct mem_cgroup *prev,
				struct mem_cgroup_reclaim_cookie *reclaim);


bool ktmm_zone_watermark_ok_safe(struct zone *z,
					unsigned int order,
					unsigned long mark,
					int highest_zoneidx);


struct pglist_data *ktmm_first_online_pgdat(void);


struct zone *ktmm_next_zone(struct zone *zone);


int install_vmscan_hooks(void);


int uninstall_vmscan_hooks(void);


#endif /* KTMM_VMHOOKS_HEADER_H */
