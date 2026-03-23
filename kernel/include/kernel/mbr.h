#ifndef _KERNEL_MBR_H
#define _KERNEL_MBR_H

#include <stdint.h>

typedef struct mbr_partition_info {
    char name[16];
    uint32_t dev_index;
    uint32_t parent_index;
    uint8_t partition_index;
    uint8_t partition_type;
    uint64_t start_lba;
    uint64_t block_count;
} mbr_partition_info_t;

typedef struct mbr_scan_stats {
    uint32_t devices_seen;
    uint32_t devices_scanned;
    uint32_t skipped_partition_devices;
    uint32_t skipped_atapi_devices;
    uint32_t skipped_non_512b_devices;
    uint32_t skipped_too_small_devices;
    uint32_t read_errors;
    uint32_t invalid_mbr_signature;
    uint32_t partitions_found;
    uint32_t partitions_registered;
    uint32_t partitions_reused;
    uint32_t partitions_out_of_bounds;
    uint32_t partition_register_failures;
} mbr_scan_stats_t;

void mbr_initialize(void);
int mbr_scan_all(void);
void mbr_get_scan_stats(mbr_scan_stats_t* out_stats);
uint32_t mbr_partition_count(void);
const mbr_partition_info_t* mbr_partition_get(uint32_t index);
int mbr_partition_parent_index(uint32_t dev_index, uint32_t* out_parent_index);
int mbr_create_single_partition(uint32_t dev_index, uint8_t partition_type);

#endif

