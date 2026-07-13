#ifndef _SYS_DISK_H
#define _SYS_DISK_H 1
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define DISK_CTL_RESCAN      0u
#define DISK_CTL_INIT        1u
#define DISK_CTL_INIT_FAT32  2u
#define DISK_CTL_MKFS_MINFS  3u
#define DISK_CTL_MKFS_FAT32  4u
#define DISK_CTL_MOUNT_MINFS 5u
#define DISK_CTL_MOUNT_FAT32 6u
#define DISK_CTL_MARKDIRTY   7u

struct disk_part_info {
    char     name[16];
    char     parent[16];
    uint32_t type;
    uint32_t start_lba;
    uint32_t block_count;
};

int disk_part_info(unsigned index, struct disk_part_info* out);
int disk_ctl(unsigned op, const char* name);

#ifdef __cplusplus
}
#endif
#endif /* _SYS_DISK_H */
