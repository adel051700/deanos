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

void mbr_initialize(void);
int mbr_scan_all(void);
uint32_t mbr_partition_count(void);
const mbr_partition_info_t* mbr_partition_get(uint32_t index);
int mbr_create_single_partition(uint32_t dev_index, uint8_t partition_type);

#endif

