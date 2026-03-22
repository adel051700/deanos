#ifndef _KERNEL_FAT32_H
#define _KERNEL_FAT32_H

#include <stdint.h>
#include <stddef.h>
#include "vfs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- FAT32 Constants --------------------------------------------------- */

#define FAT32_MAGIC_BYTE_510    0x55
#define FAT32_MAGIC_BYTE_511    0xAA

#define FAT32_PARTITION_TYPE_FAT32_LBA    0x0C
#define FAT32_PARTITION_TYPE_FAT32_CHS    0x0B

#define FAT_ENTRY_FREE          0x00000000
#define FAT_ENTRY_RESERVED      0x00000001
#define FAT_ENTRY_BAD_CLUSTER   0x0FFFFFF7
#define FAT_ENTRY_EOF_MARKER    0x0FFFFFF8
#define FAT_ENTRY_EOF_END       0x0FFFFFFF

#define FAT32_MAX_MOUNTS        4
#define FAT32_CLUSTER_CHAIN_MAX 100000

/* FAT32 Directory entry attributes */
#define FAT_ATTR_READ_ONLY      0x01
#define FAT_ATTR_HIDDEN         0x02
#define FAT_ATTR_SYSTEM         0x04
#define FAT_ATTR_VOLUME_ID      0x08
#define FAT_ATTR_DIRECTORY      0x10
#define FAT_ATTR_ARCHIVE        0x20
#define FAT_ATTR_LFN            (FAT_ATTR_READ_ONLY | FAT_ATTR_HIDDEN | FAT_ATTR_SYSTEM | FAT_ATTR_VOLUME_ID)

/* ---- FAT32 Boot Sector (BPB - BIOS Parameter Block) -------------------- */

typedef struct __attribute__((packed)) {
    uint8_t  jmp[3];              /* 0x00: Jump instruction */
    uint8_t  oem[8];              /* 0x03: OEM identifier */
    uint16_t bytes_per_sector;    /* 0x0B: Bytes per sector (usually 512) */
    uint8_t  sectors_per_cluster; /* 0x0D: Sectors per cluster */
    uint16_t reserved_sectors;    /* 0x0E: Reserved sectors (usually 1) */
    uint8_t  num_fats;            /* 0x10: Number of FAT tables (usually 2) */
    uint16_t root_entries;        /* 0x11: Root directory entries (0 for FAT32) */
    uint16_t total_sectors_16;    /* 0x13: Total sectors 16-bit (0 for FAT32) */
    uint8_t  media_type;          /* 0x15: Media descriptor */
    uint16_t sectors_per_fat_16;  /* 0x16: Sectors per FAT (0 for FAT32) */
    uint16_t sectors_per_track;   /* 0x18: Sectors per track (CHS) */
    uint16_t num_heads;           /* 0x1A: Number of heads (CHS) */
    uint32_t hidden_sectors;      /* 0x1C: Hidden sectors before partition */
    uint32_t total_sectors_32;    /* 0x20: Total sectors 32-bit */

    /* FAT32 specific */
    uint32_t sectors_per_fat_32;  /* 0x24: Sectors per FAT (FAT32) */
    uint16_t ext_flags;           /* 0x28: Extension flags */
    uint16_t fs_version;          /* 0x2A: Filesystem version (should be 0) */
    uint32_t root_cluster;        /* 0x2C: Root directory cluster number */
    uint16_t fsinfo_sector;       /* 0x30: FSInfo sector number (usually 1) */
    uint16_t backup_boot_sector;  /* 0x32: Backup boot sector (usually 6) */
    uint8_t  reserved[12];        /* 0x34: Reserved */
    uint8_t  drive_num;           /* 0x40: Drive number */
    uint8_t  reserved1;           /* 0x41: Reserved */
    uint8_t  boot_sig;            /* 0x42: Boot signature (0x29) */
    uint32_t vol_serial;          /* 0x43: Volume serial number */
    uint8_t  vol_label[11];       /* 0x47: Volume label */
    uint8_t  fs_type[8];          /* 0x52: Filesystem type string ("FAT32   ") */

    uint8_t  code[420];           /* 0x5A: Boot code */
    uint8_t  signature[2];        /* 0x1FE: Boot signature (0x55AA) */
} fat32_boot_sector_t;

_Static_assert(sizeof(fat32_boot_sector_t) == 512, "FAT32 boot sector must be 512 bytes");

/* ---- FAT32 Directory Entry (8.3 format) --------------------------------- */

typedef struct __attribute__((packed)) {
    uint8_t  name[8];             /* 0x00: Filename (8 bytes, space-padded) */
    uint8_t  ext[3];              /* 0x08: Extension (3 bytes, space-padded) */
    uint8_t  attr;                /* 0x0B: File attributes */
    uint8_t  nt_reserved;         /* 0x0C: NT case info (ignored) */
    uint8_t  create_time_ms;      /* 0x0D: Create time (10ms units) */
    uint16_t create_time;         /* 0x0E: Create time (hh:mm:ss in 2-second units) */
    uint16_t create_date;         /* 0x10: Create date (yyyy:mm:dd) */
    uint16_t access_date;         /* 0x12: Last access date */
    uint16_t cluster_high;        /* 0x14: High word of cluster number */
    uint16_t write_time;          /* 0x16: Write time */
    uint16_t write_date;          /* 0x18: Write date */
    uint16_t cluster_low;         /* 0x1A: Low word of cluster number */
    uint32_t file_size;           /* 0x1C: File size in bytes */
} fat32_dir_entry_t;

_Static_assert(sizeof(fat32_dir_entry_t) == 32, "FAT32 directory entry must be 32 bytes");

/* ---- FAT32 Mount Context ------------------------------------------------- */

typedef struct fat32_mount {
    uint32_t dev_index;           /* Block device index */
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t bytes_per_cluster;
    uint32_t num_fats;
    uint32_t root_cluster;
    uint32_t fat_start_sector;
    uint32_t sectors_per_fat;
    uint32_t data_start_sector;
    uint32_t total_clusters;

    uint32_t* fat_table;          /* FAT table cached in memory */
    vfs_node_t* root_node;        /* VFS root node */
} fat32_mount_t;

typedef struct {
    fat32_mount_t* fs;
    uint32_t cluster;             /* Current cluster for this node */
    uint32_t dir_entry_cluster;   /* Parent directory cluster containing this entry */
    uint32_t dir_entry_offset;    /* Byte offset of 32-byte entry inside dir_entry_cluster */
} fat32_node_impl_t;

/* ---- FAT32 API ---------------------------------------------------------- */

/**
 * Mount a FAT32 filesystem from a block device at a given LBA offset
 * @param dev_index Block device index
 * @param partition_lba LBA where the partition starts
 * @param mount_path VFS path to mount at (e.g., "/mnt/disk")
 * @return 0 on success, <0 on error
 */
int fat32_mount(uint32_t dev_index, uint64_t partition_lba, const char* mount_path);

/**
 * Format a block device (typically a partition device) as FAT32.
 * @param dev_index Block device index
 * @return 0 on success, <0 on error
 */
int fat32_format(uint32_t dev_index);

/**
 * Scan a block device for FAT32 partitions and auto-mount them
 * @param dev_index Block device index
 * @return Number of FAT32 partitions mounted, <0 on error
 */
int fat32_auto_mount(uint32_t dev_index);

#ifdef __cplusplus
}
#endif

#endif /* _KERNEL_FAT32_H */

