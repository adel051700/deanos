#include "include/kernel/mbr.h"

#include "include/kernel/blockdev.h"
#include "include/kernel/kheap.h"
#include "include/kernel/log.h"

#include <stdint.h>
#include <string.h>

#define MBR_PARTITION_TABLE_OFFSET 446u
#define MBR_SIGNATURE_OFFSET       510u
#define MBR_MAX_PARTITIONS         4u
#define MBR_TRACK_ALIGN_LBA        2048u
#define MBR_INFO_MAX               16u

typedef struct __attribute__((packed)) {
    uint8_t status;
    uint8_t chs_first[3];
    uint8_t type;
    uint8_t chs_last[3];
    uint32_t lba_start;
    uint32_t lba_count;
} mbr_partition_entry_t;

typedef struct {
    uint32_t parent_index;
    uint64_t start_lba;
    uint64_t block_count;
} mbr_partition_ctx_t;

static mbr_partition_info_t g_parts[MBR_INFO_MAX];
static uint32_t g_part_count = 0;

static int mbr_partition_read(void* ctx, uint64_t lba, uint32_t count, void* buffer) {
    mbr_partition_ctx_t* p = (mbr_partition_ctx_t*)ctx;
    if (!p) return -1;
    return blockdev_read(p->parent_index, p->start_lba + lba, count, buffer);
}

static int mbr_partition_write(void* ctx, uint64_t lba, uint32_t count, const void* buffer) {
    mbr_partition_ctx_t* p = (mbr_partition_ctx_t*)ctx;
    if (!p) return -1;
    return blockdev_write(p->parent_index, p->start_lba + lba, count, buffer);
}

static int mbr_is_partition_device(const block_device_t* d) {
    return d && (d->flags & BLOCKDEV_FLAG_PARTITION);
}

static void mbr_record_partition(const char* name,
                                 uint32_t dev_index,
                                 uint32_t parent_index,
                                 uint8_t part_index,
                                 uint8_t part_type,
                                 uint64_t start_lba,
                                 uint64_t block_count) {
    if (g_part_count >= MBR_INFO_MAX) return;

    mbr_partition_info_t* info = &g_parts[g_part_count++];
    memset(info, 0, sizeof(*info));
    strncpy(info->name, name, sizeof(info->name) - 1);
    info->dev_index = dev_index;
    info->parent_index = parent_index;
    info->partition_index = part_index;
    info->partition_type = part_type;
    info->start_lba = start_lba;
    info->block_count = block_count;
}

static int mbr_scan_device(uint32_t dev_index) {
    const block_device_t* dev = blockdev_get(dev_index);
    if (!dev) return 0;
    if (mbr_is_partition_device(dev)) return 0;
    if (dev->flags & BLOCKDEV_FLAG_ATAPI) return 0;
    if (dev->block_size != 512u) return 0;
    if (dev->block_count < 2u) return 0;

    uint8_t sector[512];
    if (blockdev_read(dev_index, 0, 1, sector) < 0) return 0;
    if (sector[MBR_SIGNATURE_OFFSET] != 0x55 || sector[MBR_SIGNATURE_OFFSET + 1] != 0xAA) return 0;

    int found = 0;
    for (uint32_t i = 0; i < MBR_MAX_PARTITIONS; ++i) {
        mbr_partition_entry_t ent;
        memcpy(&ent, sector + MBR_PARTITION_TABLE_OFFSET + (i * sizeof(mbr_partition_entry_t)), sizeof(ent));
        if (ent.type == 0 || ent.lba_count == 0) continue;
        if ((uint64_t)ent.lba_start + (uint64_t)ent.lba_count > dev->block_count) continue;

        char pname[16];
        memset(pname, 0, sizeof(pname));
        strncpy(pname, dev->name, sizeof(pname) - 3);
        size_t nlen = strlen(pname);
        if (nlen < sizeof(pname) - 3) {
            pname[nlen++] = 'p';
            pname[nlen++] = (char)('1' + i);
            pname[nlen] = '\0';
        } else {
            continue;
        }

        const block_device_t* existing = blockdev_find_by_name(pname);
        uint32_t part_dev_index;
        if (existing) {
            part_dev_index = existing->id;
        } else {
            mbr_partition_ctx_t* ctx = (mbr_partition_ctx_t*)kcalloc(1, sizeof(mbr_partition_ctx_t));
            if (!ctx) continue;
            ctx->parent_index = dev_index;
            ctx->start_lba = ent.lba_start;
            ctx->block_count = ent.lba_count;

            block_device_t part = {0};
            strncpy(part.name, pname, sizeof(part.name) - 1);
            part.block_size = dev->block_size;
            part.block_count = ent.lba_count;
            part.flags = (dev->flags & BLOCKDEV_FLAG_READONLY) | BLOCKDEV_FLAG_PARTITION;
            part.ctx = ctx;
            part.read = mbr_partition_read;
            part.write = (part.flags & BLOCKDEV_FLAG_READONLY) ? 0 : mbr_partition_write;

            int rc = blockdev_register(&part);
            if (rc < 0) {
                kfree(ctx);
                continue;
            }
            part_dev_index = (uint32_t)rc;
            klog("mbr: registered partition");
        }

        mbr_record_partition(pname, part_dev_index, dev_index, (uint8_t)(i + 1u), ent.type, ent.lba_start, ent.lba_count);
        found++;
    }

    return found;
}

void mbr_initialize(void) {
    g_part_count = 0;
}

int mbr_scan_all(void) {
    g_part_count = 0;
    uint32_t snapshot = blockdev_count();
    int found = 0;

    for (uint32_t i = 0; i < snapshot; ++i) {
        found += mbr_scan_device(i);
    }

    return found;
}

uint32_t mbr_partition_count(void) {
    return g_part_count;
}

const mbr_partition_info_t* mbr_partition_get(uint32_t index) {
    if (index >= g_part_count) return NULL;
    return &g_parts[index];
}

int mbr_create_single_partition(uint32_t dev_index, uint8_t partition_type) {
    const block_device_t* dev = blockdev_get(dev_index);
    if (!dev) return -1;
    if (mbr_is_partition_device(dev)) return -2;
    if (dev->flags & (BLOCKDEV_FLAG_READONLY | BLOCKDEV_FLAG_ATAPI)) return -3;
    if (dev->block_size != 512u) return -4;
    if (dev->block_count <= MBR_TRACK_ALIGN_LBA + 1u) return -5;
    if (dev->block_count > 0xFFFFFFFFULL) return -6;

    uint8_t sector[512];
    memset(sector, 0, sizeof(sector));

    mbr_partition_entry_t ent;
    memset(&ent, 0, sizeof(ent));
    ent.status = 0x00;
    ent.chs_first[0] = 0x00;
    ent.chs_first[1] = 0x02;
    ent.chs_first[2] = 0x00;
    ent.type = partition_type ? partition_type : 0x83u;
    ent.chs_last[0] = 0xFE;
    ent.chs_last[1] = 0xFF;
    ent.chs_last[2] = 0xFF;
    ent.lba_start = MBR_TRACK_ALIGN_LBA;
    ent.lba_count = (uint32_t)(dev->block_count - (uint64_t)MBR_TRACK_ALIGN_LBA);

    memcpy(sector + MBR_PARTITION_TABLE_OFFSET, &ent, sizeof(ent));
    sector[MBR_SIGNATURE_OFFSET] = 0x55;
    sector[MBR_SIGNATURE_OFFSET + 1] = 0xAA;

    return blockdev_write(dev_index, 0, 1, sector);
}

