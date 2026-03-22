/*
 * fat32.c — FAT32 filesystem driver
 *
 * Read-only FAT32 filesystem support for disk partitions.
 * Parses boot sector, FAT tables, and directory entries to provide
 * transparent access to files on FAT32-formatted disks via the VFS layer.
 */

#include "include/kernel/fat32.h"
#include "include/kernel/blockdev.h"
#include "include/kernel/kheap.h"
#include "include/kernel/log.h"
#include "include/kernel/mbr.h"
#include "include/kernel/vfs.h"

#include <string.h>
#include <stddef.h>

/* ---- Inline string utilities -------------------------------------------- */

static inline int tolower(int c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

static inline int toupper(int c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    return c;
}

static int fat32_stricmp(const char* a, const char* b) {
    if (!a || !b) return -1;
    while (*a && *b) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        ++a;
        ++b;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

/* ---- Globals ------------------------------------------------------------ */

static fat32_mount_t* g_mounts[FAT32_MAX_MOUNTS];
static uint32_t g_mount_count = 0;
static uint32_t next_inode = 1;

/* ---- Forward declarations ------------------------------------------------ */

static int32_t  fat32_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buf);
static int32_t  fat32_write(vfs_node_t* node, uint32_t offset, uint32_t size, const uint8_t* buf);
static int      fat32_open(vfs_node_t* node, uint32_t flags);
static void     fat32_close(vfs_node_t* node);
static int      fat32_readdir(vfs_node_t* node, uint32_t index, vfs_dirent_t* out);
static vfs_node_t* fat32_finddir(vfs_node_t* node, const char* name);
static int      fat32_create(vfs_node_t* parent, const char* name, uint32_t type);
static int      fat32_unlink(vfs_node_t* parent, const char* name);
static uint32_t fat32_get_next_cluster(const fat32_mount_t* fs, uint32_t cluster);

/* mkfs helpers */
static uint8_t  fat32_pick_sectors_per_cluster(uint32_t total_sectors);
static uint32_t fat32_calc_sectors_per_fat(uint32_t total_sectors, uint16_t reserved_sectors,
                                           uint8_t num_fats, uint8_t sectors_per_cluster,
                                           uint16_t bytes_per_sector);

/* ---- Helper functions --------------------------------------------------- */


/**
 * Convert a cluster number to an absolute LBA
 */
static uint64_t fat32_cluster_to_lba(const fat32_mount_t* fs, uint32_t cluster) {
    if (cluster < 2) return 0;  /* Invalid */
    return (uint64_t)fs->data_start_sector + (uint64_t)(cluster - 2) * (uint64_t)fs->sectors_per_cluster;
}

/**
 * Read a cluster from disk into buffer
 */
static int fat32_read_cluster(fat32_mount_t* fs, uint32_t cluster, uint8_t* buffer) {
    if (!fs || !buffer || cluster < 2) return -1;

    uint64_t lba = fat32_cluster_to_lba(fs, cluster);
    for (uint32_t i = 0; i < fs->sectors_per_cluster; i++) {
        if (blockdev_read(fs->dev_index, lba + i, 1, buffer + (i * fs->bytes_per_sector)) < 0) {
            return -1;
        }
    }
    return 0;
}

/**
 * Write a cluster to disk from buffer
 */
static int fat32_write_cluster(fat32_mount_t* fs, uint32_t cluster, const uint8_t* buffer) {
    if (!fs || !buffer || cluster < 2) return -1;

    uint64_t lba = fat32_cluster_to_lba(fs, cluster);
    for (uint32_t i = 0; i < fs->sectors_per_cluster; i++) {
        if (blockdev_write(fs->dev_index, lba + i, 1, buffer + (i * fs->bytes_per_sector)) < 0) {
            return -1;
        }
    }
    return 0;
}

static int fat32_flush_fat_sector(const fat32_mount_t* fs, uint32_t fat_sector_index) {
    if (!fs || !fs->fat_table) return -1;
    if (fat_sector_index >= fs->sectors_per_fat) return -1;

    const uint8_t* sector_ptr = (const uint8_t*)fs->fat_table + (fat_sector_index * fs->bytes_per_sector);
    for (uint32_t fat = 0; fat < fs->num_fats; ++fat) {
        uint64_t lba = (uint64_t)fs->fat_start_sector + (uint64_t)fat * fs->sectors_per_fat + fat_sector_index;
        if (blockdev_write(fs->dev_index, lba, 1, sector_ptr) < 0) {
            return -1;
        }
    }

    return 0;
}

static int fat32_set_fat_entry(fat32_mount_t* fs, uint32_t cluster, uint32_t value) {
    if (!fs || !fs->fat_table) return -1;
    if (cluster < 2 || cluster >= fs->total_clusters) return -1;

    uint32_t old_raw = fs->fat_table[cluster];
    fs->fat_table[cluster] = (old_raw & 0xF0000000u) | (value & 0x0FFFFFFFu);

    uint32_t fat_sector_index = (cluster * sizeof(uint32_t)) / fs->bytes_per_sector;
    return fat32_flush_fat_sector(fs, fat_sector_index);
}

static uint32_t fat32_find_free_cluster(const fat32_mount_t* fs) {
    if (!fs || !fs->fat_table) return 0;

    for (uint32_t c = 2; c < fs->total_clusters; ++c) {
        if (c == fs->root_cluster) continue; /* Never allocate the root directory cluster. */
        if ((fs->fat_table[c] & 0x0FFFFFFFu) == FAT_ENTRY_FREE) {
            return c;
        }
    }

    return 0;
}

/* Allocate a new cluster and optionally link it from prev_cluster.
 * Write order intentionally prefers leaks over chain corruption on crashes:
 * 1) write/mark new cluster as EOF, 2) then link previous cluster to it. */
static int fat32_alloc_and_link_cluster(fat32_mount_t* fs, uint32_t prev_cluster, uint32_t* out_cluster) {
    if (!fs || !out_cluster) return -1;

    uint32_t c = fat32_find_free_cluster(fs);
    if (!c) return -1;

    if (fat32_set_fat_entry(fs, c, FAT_ENTRY_EOF_END) < 0) return -1;

        uint8_t* zero_buf = (uint8_t*)kcalloc(1, fs->bytes_per_cluster);
    if (!zero_buf) return -1;
    if (fat32_write_cluster(fs, c, zero_buf) < 0) {
        kfree(zero_buf);
        return -1;
    }
    kfree(zero_buf);

    if (prev_cluster >= 2) {
        if (fat32_set_fat_entry(fs, prev_cluster, c) < 0) return -1;
    }

    *out_cluster = c;
    return 0;
}

static int fat32_sync_node_metadata(vfs_node_t* node) {
    if (!node || !(node->type & VFS_FILE)) return -1;

    fat32_node_impl_t* impl = (fat32_node_impl_t*)node->impl;
    if (!impl || !impl->fs || impl->dir_entry_cluster < 2) return -1;

    fat32_mount_t* fs = impl->fs;
    uint8_t* cluster_buf = (uint8_t*)kmalloc(fs->bytes_per_cluster);
    if (!cluster_buf) return -1;
    if (impl->dir_entry_offset + sizeof(fat32_dir_entry_t) > fs->bytes_per_cluster) {
        kfree(cluster_buf);
        return -1;
    }

    if (fat32_read_cluster(fs, impl->dir_entry_cluster, cluster_buf) < 0) {
        kfree(cluster_buf);
        return -1;
    }

    fat32_dir_entry_t* ent = (fat32_dir_entry_t*)(cluster_buf + impl->dir_entry_offset);
    ent->cluster_high = (uint16_t)((impl->cluster >> 16) & 0xFFFFu);
    ent->cluster_low = (uint16_t)(impl->cluster & 0xFFFFu);
    ent->file_size = node->size;
    ent->attr |= FAT_ATTR_ARCHIVE;

    int rc = fat32_write_cluster(fs, impl->dir_entry_cluster, cluster_buf);
    kfree(cluster_buf);
    return rc;
}

static int fat32_release_cluster_chain(fat32_mount_t* fs, uint32_t first_cluster) {
    if (!fs || !fs->fat_table) return -1;
    if (first_cluster < 2) return 0;

    uint32_t current = first_cluster;
    uint32_t guard = 0;

    while (current >= 2 && current < fs->total_clusters && guard++ < FAT32_CLUSTER_CHAIN_MAX) {
        if (current == fs->root_cluster) {
            /* Corruption guard: never free the root directory cluster. */
            return -1;
        }
        uint32_t next = fat32_get_next_cluster(fs, current);
        if (fat32_set_fat_entry(fs, current, FAT_ENTRY_FREE) < 0) return -1;
        if (!next) return 0;
        current = next;
    }

    return 0;
}

/**
 * Get the next cluster in the chain from the FAT table
 */
static uint32_t fat32_get_next_cluster(const fat32_mount_t* fs, uint32_t cluster) {
    if (!fs || !fs->fat_table || cluster < 2) return 0;
    if (cluster >= fs->total_clusters) return 0;

    uint32_t next = fs->fat_table[cluster] & 0x0FFFFFFF;

    /* Check for EOF markers */
    if (next >= FAT_ENTRY_EOF_MARKER) return 0;
    if (next >= FAT_ENTRY_BAD_CLUSTER) return 0;

    return next;
}

static uint8_t fat32_pick_sectors_per_cluster(uint32_t total_sectors) {
    if (total_sectors < 262144u) return 1u;      /* < 128 MiB */
    if (total_sectors < 524288u) return 2u;      /* < 256 MiB */
    if (total_sectors < 1048576u) return 4u;     /* < 512 MiB */
    if (total_sectors < 2097152u) return 8u;     /* < 1 GiB */
    if (total_sectors < 4194304u) return 16u;    /* < 2 GiB */
    return 32u;
}

static uint32_t fat32_calc_sectors_per_fat(uint32_t total_sectors, uint16_t reserved_sectors,
                                           uint8_t num_fats, uint8_t sectors_per_cluster,
                                           uint16_t bytes_per_sector) {
    uint32_t spf = 1;

    for (int i = 0; i < 16; ++i) {
        uint32_t overhead = (uint32_t)reserved_sectors + ((uint32_t)num_fats * spf);
        if (overhead >= total_sectors) return 0;

        uint32_t data_sectors = total_sectors - overhead;
        uint32_t cluster_count = data_sectors / sectors_per_cluster;
        uint32_t fat_bytes = (cluster_count + 2u) * 4u;
        uint32_t next_spf = (fat_bytes + bytes_per_sector - 1u) / bytes_per_sector;
        if (next_spf == 0) return 0;
        if (next_spf == spf) return spf;
        spf = next_spf;
    }

    return spf;
}

/**
 * Format 8.3 DOS filename to normal string
 * Removes padding spaces and adds extension
 */
static void fat32_format_filename(const uint8_t* name, const uint8_t* ext, char* out, size_t out_size) {
    if (!name || !ext || !out) return;

    int pos = 0;
    int max = (int)out_size - 1;

    /* Copy name, skipping trailing spaces */
    for (int i = 0; i < 8 && pos < max; i++) {
        if (name[i] == ' ') break;
        if (name[i] == 0x05) {  /* Special case: 0x05 = 0xE5 */
            out[pos++] = 0xE5;
        } else {
            out[pos++] = tolower(name[i]);
        }
    }

    /* Check if there's an extension */
    int ext_len = 0;
    for (int i = 0; i < 3; i++) {
        if (ext[i] != ' ') ext_len++;
    }

    /* Add extension if present */
    if (ext_len > 0 && pos < max) {
        out[pos++] = '.';
        for (int i = 0; i < 3 && pos < max; i++) {
            if (ext[i] == ' ') break;
            out[pos++] = tolower(ext[i]);
        }
    }

    out[pos] = '\0';
}

/**
 * Allocate and initialize a VFS node for a FAT32 file/directory
 */
static uint16_t fat32_mode_from_attr(uint32_t type, uint8_t attr) {
    if (type & VFS_DIRECTORY) {
        return (uint16_t)(VFS_MODE_IRUSR | VFS_MODE_IWUSR | VFS_MODE_IXUSR |
                          VFS_MODE_IRGRP | VFS_MODE_IWGRP | VFS_MODE_IXGRP |
                          VFS_MODE_IROTH | VFS_MODE_IWOTH | VFS_MODE_IXOTH);
    }

    uint16_t mode = (uint16_t)(VFS_MODE_IRUSR | VFS_MODE_IWUSR |
                               VFS_MODE_IRGRP | VFS_MODE_IWGRP |
                               VFS_MODE_IROTH | VFS_MODE_IWOTH);
    if (attr & FAT_ATTR_READ_ONLY) {
        mode &= (uint16_t)~(VFS_MODE_IWUSR | VFS_MODE_IWGRP | VFS_MODE_IWOTH);
    }
    return mode;
}

static vfs_node_t* fat32_alloc_node(const char* name, uint32_t type, uint32_t size,
                                    uint32_t cluster, uint8_t attr) {
    vfs_node_t* n = (vfs_node_t*)kcalloc(1, sizeof(vfs_node_t));
    if (!n) return NULL;

    strncpy(n->name, name, VFS_NAME_MAX - 1);
    n->name[VFS_NAME_MAX - 1] = '\0';
    n->type = type;
    n->size = size;
    n->inode = next_inode++;
    n->mode = fat32_mode_from_attr(type, attr);

    /* Wire up vtable */
    n->read    = fat32_read;
    n->write   = fat32_write;
    n->open    = fat32_open;
    n->close   = fat32_close;
    n->readdir = fat32_readdir;
    n->finddir = fat32_finddir;
    n->create  = fat32_create;
    n->unlink  = fat32_unlink;

    n->parent   = NULL;
    n->children = NULL;
    n->next     = NULL;

    /* Allocate impl data */
    fat32_node_impl_t* impl = (fat32_node_impl_t*)kcalloc(1, sizeof(fat32_node_impl_t));
    if (!impl) {
        kfree(n);
        return NULL;
    }
    impl->cluster = cluster;
    impl->fs = NULL;  /* Will be set by caller */
    n->impl = impl;

    return n;
}

/* ---- VFS Callback implementations --------------------------------------- */

/**
 * Read from a file
 */
static int32_t fat32_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buf) {
    if (!node || !buf || !(node->type & VFS_FILE)) return -1;

    fat32_node_impl_t* impl = (fat32_node_impl_t*)node->impl;
    if (!impl || !impl->fs) return -1;

    fat32_mount_t* fs = impl->fs;

    if (offset >= node->size) return 0;  /* EOF */

    uint32_t avail = node->size - offset;
    if (size > avail) size = avail;

    uint32_t bytes_read = 0;
    uint32_t current_cluster = impl->cluster;
    uint32_t cluster_offset = offset / fs->bytes_per_cluster;
    uint32_t byte_in_cluster = offset % fs->bytes_per_cluster;

    /* Skip to the starting cluster */
    for (uint32_t i = 0; i < cluster_offset && current_cluster; i++) {
        current_cluster = fat32_get_next_cluster(fs, current_cluster);
    }

    if (!current_cluster && cluster_offset > 0) return 0;  /* Invalid seek */

    /* Read data across cluster boundaries */
    uint8_t* cluster_buf = (uint8_t*)kmalloc(fs->bytes_per_cluster);
    if (!cluster_buf) return -1;

    while (size > 0 && current_cluster) {
        if (fat32_read_cluster(fs, current_cluster, cluster_buf) < 0) {
            kfree(cluster_buf);
            return -1;
        }

        uint32_t to_copy = fs->bytes_per_cluster - byte_in_cluster;
        if (to_copy > size) to_copy = size;

        memcpy(buf + bytes_read, cluster_buf + byte_in_cluster, to_copy);
        bytes_read += to_copy;
        size -= to_copy;

        current_cluster = fat32_get_next_cluster(fs, current_cluster);
        byte_in_cluster = 0;
    }

    kfree(cluster_buf);
    return (int32_t)bytes_read;
}

static int32_t fat32_write(vfs_node_t* node, uint32_t offset, uint32_t size, const uint8_t* buf) {
    if (!node || !buf || !(node->type & VFS_FILE)) return -1;

    fat32_node_impl_t* impl = (fat32_node_impl_t*)node->impl;
    if (!impl || !impl->fs) return -1;

    fat32_mount_t* fs = impl->fs;
    if (size == 0) return 0;
    if (offset > node->size) return -1;  /* No sparse writes yet */
    if (offset + size < offset) return -1;

    uint8_t* cluster_buf = (uint8_t*)kmalloc(fs->bytes_per_cluster);
    if (!cluster_buf) return -1;

    uint32_t cluster_index = offset / fs->bytes_per_cluster;
    uint32_t byte_in_cluster = offset % fs->bytes_per_cluster;
    uint32_t current_cluster = impl->cluster;
    uint32_t prev_cluster = 0;

    if (current_cluster < 2) {
        if (fat32_alloc_and_link_cluster(fs, 0, &current_cluster) < 0) {
            kfree(cluster_buf);
            return -1;
        }
        impl->cluster = current_cluster;
    }

    for (uint32_t i = 0; i < cluster_index; ++i) {
        uint32_t next = fat32_get_next_cluster(fs, current_cluster);
        if (!next) {
            if (fat32_alloc_and_link_cluster(fs, current_cluster, &next) < 0) {
                kfree(cluster_buf);
                return -1;
            }
        }
        prev_cluster = current_cluster;
        current_cluster = next;
    }

    if (current_cluster < 2 && prev_cluster >= 2) {
        if (fat32_alloc_and_link_cluster(fs, prev_cluster, &current_cluster) < 0) {
            kfree(cluster_buf);
            return -1;
        }
    }

    uint32_t bytes_written = 0;
    while (bytes_written < size) {
        if (current_cluster < 2) {
            kfree(cluster_buf);
            return -1;
        }

        uint32_t in_cluster_off = (bytes_written == 0) ? byte_in_cluster : 0;
        uint32_t to_copy = fs->bytes_per_cluster - in_cluster_off;
        if (to_copy > (size - bytes_written)) to_copy = size - bytes_written;

        if (in_cluster_off == 0 && to_copy == fs->bytes_per_cluster) {
            memcpy(cluster_buf, buf + bytes_written, to_copy);
        } else {
            if (fat32_read_cluster(fs, current_cluster, cluster_buf) < 0) {
                memset(cluster_buf, 0, fs->bytes_per_cluster);
            }
            memcpy(cluster_buf + in_cluster_off, buf + bytes_written, to_copy);
        }

        if (fat32_write_cluster(fs, current_cluster, cluster_buf) < 0) {
            kfree(cluster_buf);
            return -1;
        }

        bytes_written += to_copy;
        if (bytes_written >= size) break;

        uint32_t next = fat32_get_next_cluster(fs, current_cluster);
        if (!next) {
            if (fat32_alloc_and_link_cluster(fs, current_cluster, &next) < 0) {
                kfree(cluster_buf);
                return -1;
            }
        }
        current_cluster = next;
    }

    uint32_t old_size = node->size;
    uint32_t end_pos = offset + bytes_written;
    if (end_pos > node->size) {
        node->size = end_pos;
    }

    if (fat32_sync_node_metadata(node) < 0) {
        kfree(cluster_buf);
        node->size = old_size;
        return -1;
    }

    kfree(cluster_buf);
    return (int32_t)bytes_written;
}

static int fat32_open(vfs_node_t* node, uint32_t flags) {
    if (!node) return -1;
    if (!(flags & VFS_O_TRUNC)) return 0;
    if (!(node->type & VFS_FILE)) return 0;

    fat32_node_impl_t* impl = (fat32_node_impl_t*)node->impl;
    if (!impl || !impl->fs) return -1;

    uint32_t old_cluster = impl->cluster;
    uint32_t old_size = node->size;

    /* Update directory entry first; on power loss this can leak clusters,
     * but avoids a valid direntry pointing into freed/reused FAT chains. */
    impl->cluster = 0;
    node->size = 0;
    if (fat32_sync_node_metadata(node) < 0) {
        impl->cluster = old_cluster;
        node->size = old_size;
        return -1;
    }

    if (fat32_release_cluster_chain(impl->fs, old_cluster) < 0) {
        return -1;
    }

    return 0;
}

/**
 * Close a file (no-op for read-only)
 */
static void fat32_close(vfs_node_t* node) {
    (void)node;
}

/**
 * Read directory entries
 */
static int fat32_readdir(vfs_node_t* node, uint32_t index, vfs_dirent_t* out) {
    if (!node || !out || !(node->type & VFS_DIRECTORY)) return -1;

    fat32_node_impl_t* impl = (fat32_node_impl_t*)node->impl;
    if (!impl || !impl->fs) return -1;

    fat32_mount_t* fs = impl->fs;

    uint32_t entry_index = 0;
    uint32_t current_cluster = impl->cluster;
    uint8_t* cluster_buf = (uint8_t*)kmalloc(fs->bytes_per_cluster);
    if (!cluster_buf) return -1;

    while (current_cluster) {
        if (fat32_read_cluster(fs, current_cluster, cluster_buf) < 0) {
            kfree(cluster_buf);
            return -1;
        }

        uint32_t entries_per_cluster = fs->bytes_per_cluster / sizeof(fat32_dir_entry_t);

        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            fat32_dir_entry_t* ent = (fat32_dir_entry_t*)(cluster_buf + i * sizeof(fat32_dir_entry_t));

            /* End of directory */
            if (ent->name[0] == 0x00) {
                kfree(cluster_buf);
                return -1;
            }

            /* Skip deleted entries and LFN entries */
            if (ent->name[0] == 0xE5) continue;
            if ((ent->attr & FAT_ATTR_LFN) == FAT_ATTR_LFN) continue;

            /* Found the entry we're looking for */
            if (entry_index == index) {
                char filename[VFS_NAME_MAX];
                fat32_format_filename(ent->name, ent->ext, filename, sizeof(filename));

                strncpy(out->name, filename, VFS_NAME_MAX - 1);
                out->name[VFS_NAME_MAX - 1] = '\0';

                uint32_t cluster = ((uint32_t)ent->cluster_high << 16) | ent->cluster_low;
                out->inode = cluster;
                out->type = (ent->attr & FAT_ATTR_DIRECTORY) ? VFS_DIRECTORY : VFS_FILE;

                kfree(cluster_buf);
                return 0;  /* Success */
            }

            entry_index++;
        }

        current_cluster = fat32_get_next_cluster(fs, current_cluster);
    }

    kfree(cluster_buf);
    return -1;  /* Index out of range */
}

/**
 * Find a child node by name
 */
static vfs_node_t* fat32_finddir(vfs_node_t* node, const char* name) {
    if (!node || !name || !(node->type & VFS_DIRECTORY)) return NULL;

    fat32_node_impl_t* impl = (fat32_node_impl_t*)node->impl;
    if (!impl || !impl->fs) return NULL;

    fat32_mount_t* fs = impl->fs;

    uint32_t current_cluster = impl->cluster;
    uint8_t* cluster_buf = (uint8_t*)kmalloc(fs->bytes_per_cluster);
    if (!cluster_buf) return NULL;

    while (current_cluster) {
        if (fat32_read_cluster(fs, current_cluster, cluster_buf) < 0) {
            kfree(cluster_buf);
            return NULL;
        }

        uint32_t entries_per_cluster = fs->bytes_per_cluster / sizeof(fat32_dir_entry_t);

        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            fat32_dir_entry_t* ent = (fat32_dir_entry_t*)(cluster_buf + i * sizeof(fat32_dir_entry_t));

            /* End of directory */
            if (ent->name[0] == 0x00) {
                kfree(cluster_buf);
                return NULL;
            }

            /* Skip deleted entries and LFN entries */
            if (ent->name[0] == 0xE5) continue;
            if ((ent->attr & FAT_ATTR_LFN) == FAT_ATTR_LFN) continue;

            char filename[VFS_NAME_MAX];
            fat32_format_filename(ent->name, ent->ext, filename, sizeof(filename));

            if (fat32_stricmp(filename, name) == 0) {
                uint32_t cluster = ((uint32_t)ent->cluster_high << 16) | ent->cluster_low;
                uint32_t type = (ent->attr & FAT_ATTR_DIRECTORY) ? VFS_DIRECTORY : VFS_FILE;

                vfs_node_t* child = fat32_alloc_node(filename, type, ent->file_size, cluster, ent->attr);
                if (!child) {
                    kfree(cluster_buf);
                    return NULL;
                }

                fat32_node_impl_t* child_impl = (fat32_node_impl_t*)child->impl;
                child_impl->fs = fs;
                child_impl->dir_entry_cluster = current_cluster;
                child_impl->dir_entry_offset = i * sizeof(fat32_dir_entry_t);
                child->parent = node;

                kfree(cluster_buf);
                return child;
            }
        }

        current_cluster = fat32_get_next_cluster(fs, current_cluster);
    }

    kfree(cluster_buf);
    return NULL;
}

static int fat32_create(vfs_node_t* parent, const char* name, uint32_t type) {
    if (!parent || !name || !(parent->type & VFS_DIRECTORY)) return -1;
    if (type != VFS_FILE) return -1; /* Minimal implementation: regular files only */

    fat32_node_impl_t* pimpl = (fat32_node_impl_t*)parent->impl;
    if (!pimpl || !pimpl->fs || pimpl->cluster < 2) return -1;

    /* Convert "name.ext" to strict 8.3 upper-case name. */
    uint8_t short_name[8];
    uint8_t short_ext[3];
    memset(short_name, ' ', sizeof(short_name));
    memset(short_ext, ' ', sizeof(short_ext));

    const char* dot = strrchr(name, '.');
    size_t base_len = dot ? (size_t)(dot - name) : strlen(name);
    size_t ext_len = dot ? strlen(dot + 1) : 0;
    if (base_len == 0 || base_len > 8 || ext_len > 3) return -1;
    for (size_t i = 0; i < base_len; ++i) {
        char c = name[i];
        if (c == ' ' || c == '/' || c == '\\') return -1;
        short_name[i] = (uint8_t)toupper((unsigned char)c);
    }
    for (size_t i = 0; i < ext_len; ++i) {
        char c = dot[1 + i];
        if (c == ' ' || c == '/' || c == '\\') return -1;
        short_ext[i] = (uint8_t)toupper((unsigned char)c);
    }

    fat32_mount_t* fs = pimpl->fs;
    uint8_t* cluster_buf = (uint8_t*)kmalloc(fs->bytes_per_cluster);
    if (!cluster_buf) return -1;

    uint32_t current_cluster = pimpl->cluster;
    uint32_t prev_cluster = 0;

    while (current_cluster >= 2) {
        if (fat32_read_cluster(fs, current_cluster, cluster_buf) < 0) {
            kfree(cluster_buf);
            return -1;
        }

        uint32_t entries = fs->bytes_per_cluster / sizeof(fat32_dir_entry_t);
        uint32_t free_index = entries;

        for (uint32_t i = 0; i < entries; ++i) {
            fat32_dir_entry_t* ent = (fat32_dir_entry_t*)(cluster_buf + i * sizeof(fat32_dir_entry_t));

            if (ent->name[0] == 0x00 || ent->name[0] == 0xE5) {
                if (free_index == entries) free_index = i;
                if (ent->name[0] == 0x00) break;
                continue;
            }

            if ((ent->attr & FAT_ATTR_LFN) == FAT_ATTR_LFN) continue;
            if (memcmp(ent->name, short_name, 8) == 0 && memcmp(ent->ext, short_ext, 3) == 0) {
                kfree(cluster_buf);
                return -1; /* Already exists */
            }
        }

        if (free_index < entries) {
            fat32_dir_entry_t* new_ent = (fat32_dir_entry_t*)(cluster_buf + free_index * sizeof(fat32_dir_entry_t));
            memset(new_ent, 0, sizeof(*new_ent));
            memcpy(new_ent->name, short_name, sizeof(short_name));
            memcpy(new_ent->ext, short_ext, sizeof(short_ext));
            new_ent->attr = FAT_ATTR_ARCHIVE;
            new_ent->cluster_high = 0;
            new_ent->cluster_low = 0;
            new_ent->file_size = 0;

            int rc = fat32_write_cluster(fs, current_cluster, cluster_buf);
            kfree(cluster_buf);
            return rc;
        }

        prev_cluster = current_cluster;
        current_cluster = fat32_get_next_cluster(fs, current_cluster);
    }

    uint32_t new_cluster = 0;
    if (fat32_alloc_and_link_cluster(fs, prev_cluster, &new_cluster) < 0) {
        kfree(cluster_buf);
        return -1;
    }
    memset(cluster_buf, 0, fs->bytes_per_cluster);
    fat32_dir_entry_t* new_ent = (fat32_dir_entry_t*)cluster_buf;
    memcpy(new_ent->name, short_name, sizeof(short_name));
    memcpy(new_ent->ext, short_ext, sizeof(short_ext));
    new_ent->attr = FAT_ATTR_ARCHIVE;
    new_ent->cluster_high = 0;
    new_ent->cluster_low = 0;
    new_ent->file_size = 0;
    int rc = fat32_write_cluster(fs, new_cluster, cluster_buf);
    kfree(cluster_buf);
    return rc;
}

static int fat32_unlink(vfs_node_t* parent, const char* name) {
    if (!parent || !name || !(parent->type & VFS_DIRECTORY)) return -1;

    fat32_node_impl_t* pimpl = (fat32_node_impl_t*)parent->impl;
    if (!pimpl || !pimpl->fs || pimpl->cluster < 2) return -1;

    fat32_mount_t* fs = pimpl->fs;
    uint8_t* cluster_buf = (uint8_t*)kmalloc(fs->bytes_per_cluster);
    if (!cluster_buf) return -1;

    uint32_t current_cluster = pimpl->cluster;
    while (current_cluster >= 2) {
        if (fat32_read_cluster(fs, current_cluster, cluster_buf) < 0) {
            kfree(cluster_buf);
            return -1;
        }

        uint32_t entries = fs->bytes_per_cluster / sizeof(fat32_dir_entry_t);
        for (uint32_t i = 0; i < entries; ++i) {
            fat32_dir_entry_t* ent = (fat32_dir_entry_t*)(cluster_buf + i * sizeof(fat32_dir_entry_t));

            if (ent->name[0] == 0x00) {
                kfree(cluster_buf);
                return -1;
            }
            if (ent->name[0] == 0xE5) continue;
            if ((ent->attr & FAT_ATTR_LFN) == FAT_ATTR_LFN) continue;

            char filename[VFS_NAME_MAX];
            fat32_format_filename(ent->name, ent->ext, filename, sizeof(filename));
            if (fat32_stricmp(filename, name) != 0) continue;

            if (ent->attr & FAT_ATTR_DIRECTORY) {
                kfree(cluster_buf);
                return -1; /* Directory removal is not supported here. */
            }

            uint32_t first_cluster = ((uint32_t)ent->cluster_high << 16) | ent->cluster_low;

            ent->name[0] = 0xE5; /* Tombstone entry */
            ent->cluster_high = 0;
            ent->cluster_low = 0;
            ent->file_size = 0;

            if (fat32_write_cluster(fs, current_cluster, cluster_buf) < 0) {
                kfree(cluster_buf);
                return -1;
            }

            kfree(cluster_buf);
            if (first_cluster >= 2) {
                return fat32_release_cluster_chain(fs, first_cluster);
            }
            return 0;
        }

        current_cluster = fat32_get_next_cluster(fs, current_cluster);
    }

    kfree(cluster_buf);
    return -1;
}

/* ---- Filesystem initialization ------------------------------------------ */

/**
 * Load the FAT table into memory
 */
static int fat32_load_fat(fat32_mount_t* fs, const fat32_boot_sector_t* boot) {
    if (!fs || !boot) return -1;

    fs->sectors_per_fat = boot->sectors_per_fat_32;
    if (fs->sectors_per_fat == 0) return -2;

    uint32_t fat_entries = (fs->sectors_per_fat * fs->bytes_per_sector) / sizeof(uint32_t);
    fs->total_clusters = fat_entries;

    fs->fat_table = (uint32_t*)kmalloc(fs->sectors_per_fat * fs->bytes_per_sector);
    if (!fs->fat_table) return -3;

    /* Read FAT table from disk */
    for (uint32_t i = 0; i < fs->sectors_per_fat; i++) {
        uint8_t* sector = (uint8_t*)fs->fat_table + (i * fs->bytes_per_sector);
        if (blockdev_read(fs->dev_index, fs->fat_start_sector + i, 1, sector) < 0) {
            kfree(fs->fat_table);
            fs->fat_table = NULL;
            return -4;
        }
    }

    /* Safety: root cluster must never be FREE; recover to EOF if needed. */
    if (fs->root_cluster >= 2 && fs->root_cluster < fs->total_clusters) {
        if ((fs->fat_table[fs->root_cluster] & 0x0FFFFFFFu) == FAT_ENTRY_FREE) {
            if (fat32_set_fat_entry(fs, fs->root_cluster, FAT_ENTRY_EOF_END) < 0) {
                kfree(fs->fat_table);
                fs->fat_table = NULL;
                return -5;
            }
        }
    }

    return 0;
}

/**
 * Create root directory VFS node
 */
static vfs_node_t* fat32_create_root(fat32_mount_t* fs) {
    if (!fs) return NULL;

    vfs_node_t* root = fat32_alloc_node("", VFS_DIRECTORY, 0, fs->root_cluster, FAT_ATTR_DIRECTORY);
    if (!root) return NULL;

    ((fat32_node_impl_t*)root->impl)->fs = fs;
    return root;
}

/**
 * Parse and validate FAT32 boot sector
 */
static int fat32_parse_boot_sector(fat32_mount_t* fs, const fat32_boot_sector_t* boot) {
    if (!fs || !boot) return -1;

    /* Validate signatures */
    if (boot->signature[0] != FAT32_MAGIC_BYTE_510 || boot->signature[1] != FAT32_MAGIC_BYTE_511) {
        return -2;  /* Invalid boot sector signature */
    }

    /* Check FAT32 specific fields */
    if (boot->root_entries != 0) {
        return -3;  /* FAT32 should have 0 root entries */
    }

    if (boot->sectors_per_fat_16 != 0) {
        return -4;  /* FAT32 should have 0 for 16-bit sectors per FAT */
    }

    if (boot->total_sectors_16 != 0) {
        return -5;  /* FAT32 should use 32-bit total sectors */
    }

    /* Store parameters */
    fs->bytes_per_sector = boot->bytes_per_sector;
    if (fs->bytes_per_sector == 0 || fs->bytes_per_sector > 4096) {
        return -6;  /* Invalid sector size */
    }

    fs->sectors_per_cluster = boot->sectors_per_cluster;
    if (fs->sectors_per_cluster == 0 || fs->sectors_per_cluster > 128) {
        return -7;  /* Invalid cluster size */
    }

    fs->bytes_per_cluster = fs->bytes_per_sector * fs->sectors_per_cluster;
    if (fs->bytes_per_cluster > 32768) {
        return -8;  /* Cluster too large */
    }

    fs->num_fats = boot->num_fats;
    if (fs->num_fats == 0 || fs->num_fats > 2) {
        return -9;  /* Invalid FAT count */
    }

    fs->fat_start_sector = boot->reserved_sectors;
    fs->root_cluster = boot->root_cluster;

    if (fs->root_cluster < 2) {
        return -10;  /* Invalid root cluster */
    }

    fs->data_start_sector = fs->fat_start_sector + (fs->num_fats * boot->sectors_per_fat_32);

    return 0;
}

/* ---- Public API --------------------------------------------------------- */

int fat32_format(uint32_t dev_index) {
    const block_device_t* dev = blockdev_get(dev_index);
    if (!dev) return -1;
    if (dev->flags & (BLOCKDEV_FLAG_READONLY | BLOCKDEV_FLAG_ATAPI)) return -2;
    if (dev->block_size != 512u) return -3;
    if (dev->block_count < 65536u) return -4; /* Keep a practical minimum volume size. */
    if (dev->block_count > 0xFFFFFFFFULL) return -5;

    uint32_t total_sectors = (uint32_t)dev->block_count;
    uint16_t bytes_per_sector = 512u;
    uint16_t reserved_sectors = 32u;
    uint8_t num_fats = 2u;
    uint8_t sectors_per_cluster = fat32_pick_sectors_per_cluster(total_sectors);
    uint32_t sectors_per_fat = fat32_calc_sectors_per_fat(total_sectors, reserved_sectors, num_fats,
                                                          sectors_per_cluster, bytes_per_sector);
    if (sectors_per_fat == 0) return -6;

    uint32_t fat_start = reserved_sectors;
    uint32_t data_start = fat_start + (num_fats * sectors_per_fat);
    if (data_start >= total_sectors) return -7;

    uint32_t data_sectors = total_sectors - data_start;
    uint32_t cluster_count = data_sectors / sectors_per_cluster;
    if (cluster_count < 65525u) return -8; /* Enforce FAT32-size cluster range. */

    fat32_boot_sector_t boot;
    memset(&boot, 0, sizeof(boot));
    boot.jmp[0] = 0xEB;
    boot.jmp[1] = 0x58;
    boot.jmp[2] = 0x90;
    memcpy(boot.oem, "DEANOS  ", 8);
    boot.bytes_per_sector = bytes_per_sector;
    boot.sectors_per_cluster = sectors_per_cluster;
    boot.reserved_sectors = reserved_sectors;
    boot.num_fats = num_fats;
    boot.root_entries = 0;
    boot.total_sectors_16 = 0;
    boot.media_type = 0xF8;
    boot.sectors_per_fat_16 = 0;
    boot.sectors_per_track = 63;
    boot.num_heads = 255;
    boot.hidden_sectors = 0;
    boot.total_sectors_32 = total_sectors;
    boot.sectors_per_fat_32 = sectors_per_fat;
    boot.ext_flags = 0;
    boot.fs_version = 0;
    boot.root_cluster = 2;
    boot.fsinfo_sector = 1;
    boot.backup_boot_sector = 6;
    boot.drive_num = 0x80;
    boot.boot_sig = 0x29;
    boot.vol_serial = 0x20260322u;
    memcpy(boot.vol_label, "DEANOS FAT ", 11);
    memcpy(boot.fs_type, "FAT32   ", 8);
    boot.signature[0] = FAT32_MAGIC_BYTE_510;
    boot.signature[1] = FAT32_MAGIC_BYTE_511;

    if (blockdev_write(dev_index, 0, 1, &boot) < 0) return -9;
    if (boot.backup_boot_sector < reserved_sectors) {
        if (blockdev_write(dev_index, boot.backup_boot_sector, 1, &boot) < 0) return -10;
    }

    uint8_t fsinfo[512];
    memset(fsinfo, 0, sizeof(fsinfo));
    fsinfo[0] = 0x52; fsinfo[1] = 0x52; fsinfo[2] = 0x61; fsinfo[3] = 0x41;          /* RRaA */
    fsinfo[484] = 0x72; fsinfo[485] = 0x72; fsinfo[486] = 0x41; fsinfo[487] = 0x61;  /* rrAa */
    fsinfo[488] = 0xFF; fsinfo[489] = 0xFF; fsinfo[490] = 0xFF; fsinfo[491] = 0xFF;  /* free count unknown */
    fsinfo[492] = 0xFF; fsinfo[493] = 0xFF; fsinfo[494] = 0xFF; fsinfo[495] = 0xFF;  /* next free unknown */
    fsinfo[510] = FAT32_MAGIC_BYTE_510;
    fsinfo[511] = FAT32_MAGIC_BYTE_511;

    if (boot.fsinfo_sector < reserved_sectors) {
        if (blockdev_write(dev_index, boot.fsinfo_sector, 1, fsinfo) < 0) return -11;
    }
    if ((uint32_t)boot.backup_boot_sector + 1u < reserved_sectors) {
        if (blockdev_write(dev_index, (uint32_t)boot.backup_boot_sector + 1u, 1, fsinfo) < 0) return -12;
    }

    uint8_t zero_sector[512];
    memset(zero_sector, 0, sizeof(zero_sector));

    for (uint32_t fat = 0; fat < num_fats; ++fat) {
        uint32_t base = fat_start + (fat * sectors_per_fat);
        for (uint32_t s = 0; s < sectors_per_fat; ++s) {
            if (blockdev_write(dev_index, base + s, 1, zero_sector) < 0) return -13;
        }
    }

    uint8_t fat_sector[512];
    memset(fat_sector, 0, sizeof(fat_sector));
    fat_sector[0] = 0xF8;
    fat_sector[1] = 0xFF;
    fat_sector[2] = 0xFF;
    fat_sector[3] = 0x0F;
    fat_sector[4] = 0xFF;
    fat_sector[5] = 0xFF;
    fat_sector[6] = 0xFF;
    fat_sector[7] = 0x0F;
    fat_sector[8] = 0xFF;
    fat_sector[9] = 0xFF;
    fat_sector[10] = 0xFF;
    fat_sector[11] = 0x0F;

    for (uint32_t fat = 0; fat < num_fats; ++fat) {
        uint32_t base = fat_start + (fat * sectors_per_fat);
        if (blockdev_write(dev_index, base, 1, fat_sector) < 0) return -14;
    }

    uint32_t root_lba = data_start;
    for (uint32_t i = 0; i < sectors_per_cluster; ++i) {
        if (blockdev_write(dev_index, root_lba + i, 1, zero_sector) < 0) return -15;
    }

    return 0;
}

/**
 * Ensure a mount directory exists, creating it if necessary
 */
static int fat32_ensure_mount_dir(vfs_node_t* parent, const char* name, vfs_node_t** out) {
    if (!parent || !name || !out) return -1;

    vfs_node_t* node = vfs_finddir(parent, name);
    if (!node) {
        if (vfs_create(parent, name, VFS_DIRECTORY) < 0) return -1;
        node = vfs_finddir(parent, name);
    }
    if (!node || !(node->type & VFS_DIRECTORY)) return -1;

    *out = node;
    return 0;
}

/**
 * Mount a FAT32 filesystem
 */
int fat32_mount(uint32_t dev_index, uint64_t partition_lba, const char* mount_path) {
    const block_device_t* dev = blockdev_get(dev_index);
    if (!dev) {
        klog("FAT32: Invalid device index");
        return -1;
    }

    if (g_mount_count >= FAT32_MAX_MOUNTS) {
        klog("FAT32: Max mounts reached");
        return -2;
    }

    /* Allocate mount structure */
    fat32_mount_t* fs = (fat32_mount_t*)kcalloc(1, sizeof(fat32_mount_t));
    if (!fs) {
        klog("FAT32: Failed to allocate mount structure");
        return -3;
    }

    fs->dev_index = dev_index;

    /* Read boot sector */
    fat32_boot_sector_t boot;
    memset(&boot, 0, sizeof(boot));

    if (blockdev_read(dev_index, partition_lba, 1, &boot) < 0) {
        klog("FAT32: Failed to read boot sector");
        kfree(fs);
        return -4;
    }

    /* Parse and validate boot sector */
    if (fat32_parse_boot_sector(fs, &boot) < 0) {
        klog("FAT32: Invalid boot sector");
        kfree(fs);
        return -5;
    }

    /* Adjust sector offsets for partition offset */
    fs->fat_start_sector += partition_lba;
    fs->data_start_sector += partition_lba;

    /* Load FAT table */
    if (fat32_load_fat(fs, &boot) < 0) {
        klog("FAT32: Failed to load FAT table");
        kfree(fs);
        return -6;
    }

    /* Create root VFS node */
    fs->root_node = fat32_create_root(fs);
    if (!fs->root_node) {
        klog("FAT32: Failed to create root node");
        kfree(fs->fat_table);
        kfree(fs);
        return -7;
    }

    /* Ensure mount directory hierarchy exists */
    vfs_node_t* root = vfs_get_root();
    if (!root) {
        klog("FAT32: Failed to get VFS root");
        kfree(fs->root_node);
        kfree(fs->fat_table);
        kfree(fs);
        return -8;
    }

    vfs_node_t* mnt = NULL;
    if (fat32_ensure_mount_dir(root, "mnt", &mnt) < 0) {
        klog("FAT32: Failed to create /mnt directory");
        kfree(fs->root_node);
        kfree(fs->fat_table);
        kfree(fs);
        return -9;
    }

    char parent_path[VFS_PATH_MAX];
    char mount_name[VFS_NAME_MAX];
    if (vfs_split_path(mount_path, parent_path, sizeof(parent_path),
                       mount_name, sizeof(mount_name)) < 0) {
        klog("FAT32: Invalid mount path");
        kfree(fs->root_node);
        kfree(fs->fat_table);
        kfree(fs);
        return -10;
    }

    vfs_node_t* parent = NULL;
    if (strcmp(parent_path, "/mnt") == 0) {
        parent = mnt;
    } else {
        parent = vfs_namei(parent_path);
    }

    if (!parent || !(parent->type & VFS_DIRECTORY)) {
        klog("FAT32: Mount path parent does not exist");
        kfree(fs->root_node);
        kfree(fs->fat_table);
        kfree(fs);
        return -11;
    }

    vfs_node_t* mountpoint = NULL;
    if (fat32_ensure_mount_dir(parent, mount_name, &mountpoint) < 0) {
        klog("FAT32: Failed to create mountpoint directory");
        kfree(fs->root_node);
        kfree(fs->fat_table);
        kfree(fs);
        return -12;
    }
    (void)mountpoint;

    /* Mount in VFS */
    if (vfs_mount(mount_path, fs->root_node) < 0) {
        klog("FAT32: Failed to mount");
        kfree(fs->root_node);
        kfree(fs->fat_table);
        kfree(fs);
        return -13;
    }

    /* Register mount */
    g_mounts[g_mount_count] = fs;
    g_mount_count++;

    klog("FAT32: Mounted successfully");

    return 0;
}

/**
 * Auto-mount FAT32 partitions (stub for now)
 */
int fat32_auto_mount(uint32_t dev_index) {
    uint32_t mounted = 0;

    uint32_t total_parts = mbr_partition_count();
    for (uint32_t i = 0; i < total_parts; ++i) {
        const mbr_partition_info_t* p = mbr_partition_get(i);
        if (!p) continue;
        if (p->parent_index != dev_index) continue;
        if (p->partition_type != FAT32_PARTITION_TYPE_FAT32_CHS &&
            p->partition_type != FAT32_PARTITION_TYPE_FAT32_LBA) {
            continue;
        }

        char mount_path[VFS_PATH_MAX];
        strcpy(mount_path, "/mnt/");
        strncat(mount_path, p->name, VFS_PATH_MAX - strlen(mount_path) - 1);

        if (fat32_mount(p->dev_index, 0, mount_path) == 0) {
            mounted++;
        }
    }

    return (int)mounted;
}

