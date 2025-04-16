/* tmem_vmscan header.h */
#ifndef KTMM_VMSCAN_HEADER_H
#define KTMM_VMSCAN_HEADER_H

enum node_stat_item_ext {
	NR_ZONE_PROMOTE_ANON,
    NR_ZONE_PROMOTE_ZONE,
    NR_PROMOTE_ANON,
    NR_PROMOTE_FILE,
    NR_DEMOTED,
    NR_PROMOTED,
};

/**
 * tmem_start_available - start tmem daemons on all online nodes
 */
int tmemd_start_available(void);

/**
 * tmemd_stop_all - stop all tmem daemons on all online nodes
 */
void tmemd_stop_all(void);

#endif /* KTMM_VMSCAN_HEADER_H */
