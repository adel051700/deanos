#include "include/kernel/minfs.h"

#include "include/kernel/blockdev.h"
#include "include/kernel/kheap.h"
#include "include/kernel/log.h"
#include "include/kernel/vfs.h"

#include <stdint.h>
#include <string.h>

#define MINFS_MAGIC             0x3246534Du /* "MFS2" */
#define MINFS_VERSION           2u
#define MINFS_MAX_NODES         128u
#define MINFS_BLOCK_SIZE        512u
#define MINFS_MAX_MOUNTS        4u
#define MINFS_INVALID_PARENT    0xFFFFFFFFu
#define MINFS_BLOCK_NONE        0xFFFFFFFFu
#define MINFS_DIRECT_BLOCKS     (VFS_MAX_FILE / MINFS_BLOCK_SIZE)
#define MINFS_MAX_FILE_BYTES    (MINFS_DIRECT_BLOCKS * MINFS_BLOCK_SIZE)

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t total_nodes;
    uint32_t block_size;
    uint32_t max_file_bytes;
    uint32_t node_table_start;
    uint32_t node_table_sectors;
    uint32_t bitmap_start;
    uint32_t bitmap_sectors;
    uint32_t data_start_sector;
    uint32_t data_block_count;
    uint32_t reserved[117];
} minfs_superblock_disk_t;

typedef struct __attribute__((packed)) {
    uint8_t used;
    uint8_t type;
    uint16_t reserved0;
    uint32_t size;
    uint32_t parent_index;
    uint32_t block_count;
    char name[VFS_NAME_MAX];
    uint32_t direct[MINFS_DIRECT_BLOCKS];
} minfs_disk_node_t;

struct minfs;

typedef struct {
    struct minfs* fs;
    uint32_t index;
} minfs_node_impl_t;

typedef struct minfs {
    uint32_t dev_index;
    uint32_t total_nodes;
    uint32_t block_size;
    uint32_t max_file_bytes;
    uint32_t node_table_start;
    uint32_t node_table_sectors;
    uint32_t bitmap_start;
    uint32_t bitmap_sectors;
    uint32_t bitmap_bytes;
    uint32_t data_start_sector;
    uint32_t data_block_count;
    minfs_disk_node_t* nodes;
    uint8_t* bitmap;
    minfs_node_impl_t* impls;
    vfs_node_t** vnodes;
} minfs_t;

static minfs_t* g_mounts[MINFS_MAX_MOUNTS];
static uint32_t g_mount_count = 0;

static int32_t minfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer);
static int32_t minfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, const uint8_t* buffer);
static int minfs_open(vfs_node_t* node, uint32_t flags);
static void minfs_close(vfs_node_t* node);
static int minfs_readdir(vfs_node_t* node, uint32_t index, vfs_dirent_t* out);
static vfs_node_t* minfs_finddir(vfs_node_t* node, const char* name);
static int minfs_create(vfs_node_t* parent, const char* name, uint32_t type);
static int minfs_unlink(vfs_node_t* parent, const char* name);

static void minfs_disk_node_init(minfs_disk_node_t* node) {
    if (!node) return;
    memset(node, 0, sizeof(*node));
    node->parent_index = MINFS_INVALID_PARENT;
    for (uint32_t i = 0; i < MINFS_DIRECT_BLOCKS; ++i) {
        node->direct[i] = MINFS_BLOCK_NONE;
    }
}

static minfs_t* minfs_find_mount(uint32_t dev_index) {
    for (uint32_t i = 0; i < g_mount_count; ++i) {
        if (g_mounts[i] && g_mounts[i]->dev_index == dev_index) return g_mounts[i];
    }
    return NULL;
}

static void minfs_free_mount(minfs_t* fs) {
    if (!fs) return;
    if (fs->vnodes) {
        for (uint32_t i = 1; i < fs->total_nodes; ++i) {
            if (fs->vnodes[i]) kfree(fs->vnodes[i]);
        }
        kfree(fs->vnodes);
    }
    if (fs->impls) kfree(fs->impls);
    if (fs->bitmap) kfree(fs->bitmap);
    if (fs->nodes) kfree(fs->nodes);
    kfree(fs);
}

static int minfs_read_super(uint32_t dev_index, minfs_superblock_disk_t* super) {
    if (!super) return -1;
    return blockdev_read(dev_index, 0, 1, super);
}

static int minfs_super_is_valid(const minfs_superblock_disk_t* super, const block_device_t* dev) {
    if (!super || !dev) return 0;
    if (super->magic != MINFS_MAGIC) return 0;
    if (super->version != MINFS_VERSION) return 0;
    if (super->block_size != MINFS_BLOCK_SIZE) return 0;
    if (dev->block_size != MINFS_BLOCK_SIZE) return 0;
    if (super->max_file_bytes == 0 || super->max_file_bytes > VFS_MAX_FILE) return 0;
    if (super->max_file_bytes != MINFS_MAX_FILE_BYTES) return 0;
    if (super->total_nodes == 0 || super->total_nodes > MINFS_MAX_NODES) return 0;
    if (super->node_table_start != 1u) return 0;
    if (super->node_table_sectors == 0) return 0;
    if (super->bitmap_start != super->node_table_start + super->node_table_sectors) return 0;
    if (super->bitmap_sectors == 0) return 0;
    if (super->data_start_sector != super->bitmap_start + super->bitmap_sectors) return 0;
    if (super->data_block_count == 0) return 0;
    if ((uint64_t)super->data_start_sector + (uint64_t)super->data_block_count > dev->block_count) return 0;
    return 1;
}

static uint64_t minfs_block_lba(const minfs_t* fs, uint32_t block_index) {
    return (uint64_t)fs->data_start_sector + (uint64_t)block_index;
}

static int minfs_read_data_block(minfs_t* fs, uint32_t block_index, void* buffer) {
    if (!fs || !buffer || block_index >= fs->data_block_count) return -1;
    return blockdev_read(fs->dev_index, minfs_block_lba(fs, block_index), 1, buffer);
}

static int minfs_write_data_block(minfs_t* fs, uint32_t block_index, const void* buffer) {
    if (!fs || !buffer || block_index >= fs->data_block_count) return -1;
    return blockdev_write(fs->dev_index, minfs_block_lba(fs, block_index), 1, buffer);
}

static int minfs_flush_nodes(minfs_t* fs) {
    if (!fs || !fs->nodes) return -1;

    uint32_t bytes = fs->node_table_sectors * MINFS_BLOCK_SIZE;
    uint8_t* buf = (uint8_t*)kcalloc(1, bytes);
    if (!buf) return -1;

    memcpy(buf, fs->nodes, fs->total_nodes * sizeof(minfs_disk_node_t));
    int rc = blockdev_write(fs->dev_index, fs->node_table_start, fs->node_table_sectors, buf);
    kfree(buf);
    return rc;
}

static int minfs_flush_bitmap(minfs_t* fs) {
    if (!fs || !fs->bitmap) return -1;
    return blockdev_write(fs->dev_index, fs->bitmap_start, fs->bitmap_sectors, fs->bitmap);
}

static uint32_t minfs_blocks_for_size(uint32_t size) {
    if (size == 0) return 0;
    return (size + MINFS_BLOCK_SIZE - 1u) / MINFS_BLOCK_SIZE;
}

static int minfs_bitmap_is_set(const minfs_t* fs, uint32_t index) {
    if (!fs || !fs->bitmap || index >= fs->data_block_count) return 0;
    return (fs->bitmap[index / 8u] >> (index % 8u)) & 1u;
}

static void minfs_bitmap_set(minfs_t* fs, uint32_t index) {
    if (!fs || !fs->bitmap || index >= fs->data_block_count) return;
    fs->bitmap[index / 8u] |= (uint8_t)(1u << (index % 8u));
}

static void minfs_bitmap_clear(minfs_t* fs, uint32_t index) {
    if (!fs || !fs->bitmap || index >= fs->data_block_count) return;
    fs->bitmap[index / 8u] &= (uint8_t)~(1u << (index % 8u));
}

static int minfs_alloc_block(minfs_t* fs, uint32_t* out_index) {
    if (!fs || !out_index) return -1;
    for (uint32_t i = 0; i < fs->data_block_count; ++i) {
        if (!minfs_bitmap_is_set(fs, i)) {
            minfs_bitmap_set(fs, i);
            *out_index = i;
            return 0;
        }
    }
    return -1;
}

static void minfs_free_block(minfs_t* fs, uint32_t index) {
    if (!fs || index >= fs->data_block_count) return;
    minfs_bitmap_clear(fs, index);
}

static int minfs_release_file_blocks(minfs_t* fs, minfs_disk_node_t* node) {
    if (!fs || !node) return -1;
    for (uint32_t i = 0; i < MINFS_DIRECT_BLOCKS; ++i) {
        if (node->direct[i] != MINFS_BLOCK_NONE) {
            minfs_free_block(fs, node->direct[i]);
            node->direct[i] = MINFS_BLOCK_NONE;
        }
    }
    node->block_count = 0;
    node->size = 0;
    return 0;
}

static int minfs_ensure_file_blocks(minfs_t* fs, minfs_disk_node_t* node, uint32_t needed_blocks) {
    if (!fs || !node) return -1;
    if (needed_blocks > MINFS_DIRECT_BLOCKS) return -1;
    if (needed_blocks <= node->block_count) return 0;

    uint32_t allocated[MINFS_DIRECT_BLOCKS];
    uint32_t alloc_count = 0;

    for (uint32_t i = node->block_count; i < needed_blocks; ++i) {
        uint32_t blk = 0;
        if (minfs_alloc_block(fs, &blk) < 0) {
            for (uint32_t j = 0; j < alloc_count; ++j) {
                minfs_free_block(fs, allocated[j]);
                node->direct[node->block_count + j] = MINFS_BLOCK_NONE;
            }
            return -1;
        }
        node->direct[i] = blk;
        allocated[alloc_count++] = blk;
    }

    node->block_count = needed_blocks;
    return 0;
}

static uint32_t minfs_count_children(minfs_t* fs, uint32_t parent_index) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < fs->total_nodes; ++i) {
        if (!fs->nodes[i].used) continue;
        if (fs->nodes[i].parent_index == parent_index) count++;
    }
    return count;
}

static void minfs_wire_vnode(minfs_t* fs, uint32_t index, vfs_node_t* node, int keep_name) {
    if (!fs || !node || index >= fs->total_nodes) return;

    if (!keep_name) {
        strncpy(node->name, fs->nodes[index].name, VFS_NAME_MAX - 1);
        node->name[VFS_NAME_MAX - 1] = '\0';
    }
    node->type = fs->nodes[index].type;
    node->size = fs->nodes[index].size;
    node->inode = (fs->dev_index << 16) | index;
    node->read = minfs_read;
    node->write = minfs_write;
    node->open = minfs_open;
    node->close = minfs_close;
    node->readdir = minfs_readdir;
    node->finddir = minfs_finddir;
    node->create = minfs_create;
    node->unlink = minfs_unlink;
    node->impl = &fs->impls[index];
    node->children = NULL;
    node->next = NULL;
}

static int minfs_refresh_directory_sizes(minfs_t* fs) {
    if (!fs) return -1;
    for (uint32_t i = 0; i < fs->total_nodes; ++i) {
        if (!fs->nodes[i].used) continue;
        if (!(fs->nodes[i].type & VFS_DIRECTORY)) continue;
        uint32_t count = minfs_count_children(fs, i);
        fs->nodes[i].size = count;
        if (fs->vnodes[i]) fs->vnodes[i]->size = count;
    }
    return 0;
}

static vfs_node_t* minfs_alloc_vnode(minfs_t* fs, uint32_t index) {
    if (!fs || index >= fs->total_nodes) return NULL;
    if (fs->vnodes[index]) return fs->vnodes[index];

    vfs_node_t* node = (vfs_node_t*)kcalloc(1, sizeof(vfs_node_t));
    if (!node) return NULL;
    fs->vnodes[index] = node;
    minfs_wire_vnode(fs, index, node, 0);
    return node;
}

static int minfs_find_free_index(minfs_t* fs) {
    if (!fs) return -1;
    for (uint32_t i = 1; i < fs->total_nodes; ++i) {
        if (!fs->nodes[i].used) return (int)i;
    }
    return -1;
}

static vfs_node_t* minfs_find_child_node(minfs_t* fs, uint32_t parent_index, const char* name, uint32_t* out_index) {
    if (!fs || !name) return NULL;
    for (uint32_t i = 0; i < fs->total_nodes; ++i) {
        if (!fs->nodes[i].used) continue;
        if (fs->nodes[i].parent_index != parent_index) continue;
        if (strcmp(fs->nodes[i].name, name) != 0) continue;
        if (out_index) *out_index = i;
        return minfs_alloc_vnode(fs, i);
    }
    return NULL;
}

static int minfs_ensure_mount_dir(vfs_node_t* parent, const char* name, vfs_node_t** out) {
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

static int32_t minfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    if (!node || !buffer) return -1;
    minfs_node_impl_t* impl = (minfs_node_impl_t*)node->impl;
    if (!impl || !impl->fs) return -1;
    if (!(node->type & VFS_FILE)) return -1;
    if (offset >= node->size) return 0;

    minfs_t* fs = impl->fs;
    uint32_t avail = node->size - offset;
    if (size > avail) size = avail;

    uint8_t blockbuf[MINFS_BLOCK_SIZE];
    uint32_t done = 0;
    while (done < size) {
        uint32_t file_off = offset + done;
        uint32_t block_idx = file_off / MINFS_BLOCK_SIZE;
        uint32_t block_off = file_off % MINFS_BLOCK_SIZE;
        uint32_t chunk = MINFS_BLOCK_SIZE - block_off;
        if (chunk > (size - done)) chunk = size - done;

        if (block_idx >= fs->nodes[impl->index].block_count || fs->nodes[impl->index].direct[block_idx] == MINFS_BLOCK_NONE) {
            memset(buffer + done, 0, chunk);
        } else {
            if (minfs_read_data_block(fs, fs->nodes[impl->index].direct[block_idx], blockbuf) < 0) return -1;
            memcpy(buffer + done, blockbuf + block_off, chunk);
        }
        done += chunk;
    }

    return (int32_t)done;
}

static int32_t minfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, const uint8_t* buffer) {
    if (!node || !buffer) return -1;
    minfs_node_impl_t* impl = (minfs_node_impl_t*)node->impl;
    if (!impl || !impl->fs) return -1;
    if (!(node->type & VFS_FILE)) return -1;

    minfs_t* fs = impl->fs;
    minfs_disk_node_t* dnode = &fs->nodes[impl->index];
    if (offset >= fs->max_file_bytes) return -1;
    if (offset + size > fs->max_file_bytes) {
        size = fs->max_file_bytes - offset;
    }

    uint32_t old_size = dnode->size;
    uint32_t old_blocks = dnode->block_count;
    uint32_t end = offset + size;
    uint32_t needed_blocks = minfs_blocks_for_size(end);
    int bitmap_dirty = 0;

    if (needed_blocks > dnode->block_count) {
        if (minfs_ensure_file_blocks(fs, dnode, needed_blocks) < 0) return -1;
        bitmap_dirty = 1;
    }

    uint8_t blockbuf[MINFS_BLOCK_SIZE];
    uint32_t done = 0;
    while (done < size) {
        uint32_t file_off = offset + done;
        uint32_t block_idx = file_off / MINFS_BLOCK_SIZE;
        uint32_t block_off = file_off % MINFS_BLOCK_SIZE;
        uint32_t chunk = MINFS_BLOCK_SIZE - block_off;
        if (chunk > (size - done)) chunk = size - done;

        uint32_t data_block = dnode->direct[block_idx];
        if (data_block == MINFS_BLOCK_NONE) return -1;

        if (block_off == 0 && chunk == MINFS_BLOCK_SIZE) {
            if (minfs_write_data_block(fs, data_block, buffer + done) < 0) return -1;
        } else {
            if (block_idx < old_blocks) {
                if (minfs_read_data_block(fs, data_block, blockbuf) < 0) return -1;
            } else {
                memset(blockbuf, 0, sizeof(blockbuf));
            }
            memcpy(blockbuf + block_off, buffer + done, chunk);
            if (minfs_write_data_block(fs, data_block, blockbuf) < 0) return -1;
        }

        done += chunk;
    }

    if (end > dnode->size) {
        dnode->size = end;
        node->size = end;
    }

    if (bitmap_dirty && minfs_flush_bitmap(fs) < 0) return -1;
    if (dnode->size != old_size || dnode->block_count != old_blocks) {
        if (minfs_flush_nodes(fs) < 0) return -1;
    }

    return (int32_t)done;
}

static int minfs_open(vfs_node_t* node, uint32_t flags) {
    if (!node) return -1;
    if (!(flags & VFS_O_TRUNC)) return 0;
    if (!(node->type & VFS_FILE)) return 0;

    minfs_node_impl_t* impl = (minfs_node_impl_t*)node->impl;
    if (!impl || !impl->fs) return -1;

    if (minfs_release_file_blocks(impl->fs, &impl->fs->nodes[impl->index]) < 0) return -1;
    node->size = 0;
    if (minfs_flush_bitmap(impl->fs) < 0) return -1;
    if (minfs_flush_nodes(impl->fs) < 0) return -1;
    return 0;
}

static void minfs_close(vfs_node_t* node) {
    (void)node;
}

static int minfs_readdir(vfs_node_t* node, uint32_t index, vfs_dirent_t* out) {
    if (!node || !out) return -1;
    minfs_node_impl_t* impl = (minfs_node_impl_t*)node->impl;
    if (!impl || !impl->fs) return -1;
    if (!(node->type & VFS_DIRECTORY)) return -1;

    uint32_t seen = 0;
    for (uint32_t i = 0; i < impl->fs->total_nodes; ++i) {
        if (!impl->fs->nodes[i].used) continue;
        if (impl->fs->nodes[i].parent_index != impl->index) continue;
        if (seen == index) {
            strncpy(out->name, impl->fs->nodes[i].name, VFS_NAME_MAX - 1);
            out->name[VFS_NAME_MAX - 1] = '\0';
            out->inode = (impl->fs->dev_index << 16) | i;
            out->type = impl->fs->nodes[i].type;
            return 0;
        }
        seen++;
    }

    return -1;
}

static vfs_node_t* minfs_finddir(vfs_node_t* node, const char* name) {
    if (!node || !name) return NULL;
    minfs_node_impl_t* impl = (minfs_node_impl_t*)node->impl;
    if (!impl || !impl->fs) return NULL;
    if (!(node->type & VFS_DIRECTORY)) return NULL;
    return minfs_find_child_node(impl->fs, impl->index, name, NULL);
}

static int minfs_create(vfs_node_t* parent, const char* name, uint32_t type) {
    if (!parent || !name) return -1;
    minfs_node_impl_t* impl = (minfs_node_impl_t*)parent->impl;
    if (!impl || !impl->fs) return -1;
    if (!(parent->type & VFS_DIRECTORY)) return -1;
    if (type != VFS_FILE && type != VFS_DIRECTORY) return -1;
    if (*name == '\0' || strlen(name) >= VFS_NAME_MAX) return -1;
    if (minfs_find_child_node(impl->fs, impl->index, name, NULL)) return -1;

    int free_index = minfs_find_free_index(impl->fs);
    if (free_index < 0) return -1;

    minfs_disk_node_t* d = &impl->fs->nodes[(uint32_t)free_index];
    minfs_disk_node_init(d);
    d->used = 1;
    d->type = (uint8_t)type;
    d->parent_index = impl->index;
    strncpy(d->name, name, sizeof(d->name) - 1);
    d->name[sizeof(d->name) - 1] = '\0';

    if (!minfs_alloc_vnode(impl->fs, (uint32_t)free_index)) {
        minfs_disk_node_init(d);
        return -1;
    }

    minfs_refresh_directory_sizes(impl->fs);
    return minfs_flush_nodes(impl->fs);
}

static int minfs_unlink(vfs_node_t* parent, const char* name) {
    if (!parent || !name) return -1;
    minfs_node_impl_t* impl = (minfs_node_impl_t*)parent->impl;
    if (!impl || !impl->fs) return -1;
    if (!(parent->type & VFS_DIRECTORY)) return -1;

    uint32_t idx = 0;
    vfs_node_t* child = minfs_find_child_node(impl->fs, impl->index, name, &idx);
    if (!child) return -1;
    if (idx == 0) return -1;
    if ((child->type & VFS_DIRECTORY) && minfs_count_children(impl->fs, idx) != 0) return -1;

    if (impl->fs->nodes[idx].type & VFS_FILE) {
        if (minfs_release_file_blocks(impl->fs, &impl->fs->nodes[idx]) < 0) return -1;
        if (minfs_flush_bitmap(impl->fs) < 0) return -1;
    }

    minfs_disk_node_init(&impl->fs->nodes[idx]);
    if (impl->fs->vnodes[idx]) {
        kfree(impl->fs->vnodes[idx]);
        impl->fs->vnodes[idx] = NULL;
    }

    minfs_refresh_directory_sizes(impl->fs);
    return minfs_flush_nodes(impl->fs);
}

int minfs_format(uint32_t dev_index) {
    const block_device_t* dev = blockdev_get(dev_index);
    if (!dev) return -1;
    if (minfs_find_mount(dev_index)) return -2;
    if (dev->flags & (BLOCKDEV_FLAG_READONLY | BLOCKDEV_FLAG_ATAPI)) return -3;
    if (dev->block_size != MINFS_BLOCK_SIZE) return -4;

    uint32_t node_table_sectors = (uint32_t)(((MINFS_MAX_NODES * sizeof(minfs_disk_node_t)) + MINFS_BLOCK_SIZE - 1u) / MINFS_BLOCK_SIZE);
    uint32_t bitmap_sectors = 1u;
    uint32_t data_start = 0;
    uint32_t data_blocks = 0;

    while (1) {
        data_start = 1u + node_table_sectors + bitmap_sectors;
        if (dev->block_count <= data_start) return -5;
        data_blocks = (uint32_t)(dev->block_count - data_start);
        uint32_t needed_bitmap = (data_blocks + (MINFS_BLOCK_SIZE * 8u) - 1u) / (MINFS_BLOCK_SIZE * 8u);
        if (needed_bitmap == bitmap_sectors) break;
        bitmap_sectors = needed_bitmap;
    }

    minfs_superblock_disk_t super;
    memset(&super, 0, sizeof(super));
    super.magic = MINFS_MAGIC;
    super.version = MINFS_VERSION;
    super.total_nodes = MINFS_MAX_NODES;
    super.block_size = MINFS_BLOCK_SIZE;
    super.max_file_bytes = MINFS_MAX_FILE_BYTES;
    super.node_table_start = 1u;
    super.node_table_sectors = node_table_sectors;
    super.bitmap_start = super.node_table_start + super.node_table_sectors;
    super.bitmap_sectors = bitmap_sectors;
    super.data_start_sector = data_start;
    super.data_block_count = data_blocks;

    if (blockdev_write(dev_index, 0, 1, &super) < 0) return -6;

    uint32_t table_bytes = node_table_sectors * MINFS_BLOCK_SIZE;
    uint8_t* table = (uint8_t*)kcalloc(1, table_bytes);
    if (!table) return -7;

    minfs_disk_node_t* nodes = (minfs_disk_node_t*)table;
    for (uint32_t i = 0; i < MINFS_MAX_NODES; ++i) minfs_disk_node_init(&nodes[i]);
    nodes[0].used = 1;
    nodes[0].type = VFS_DIRECTORY;
    nodes[0].parent_index = MINFS_INVALID_PARENT;
    strcpy(nodes[0].name, "/");
    nodes[0].size = 0;

    int rc = blockdev_write(dev_index, super.node_table_start, super.node_table_sectors, table);
    kfree(table);
    if (rc < 0) return -8;

    uint32_t bitmap_bytes = bitmap_sectors * MINFS_BLOCK_SIZE;
    uint8_t* bitmap = (uint8_t*)kcalloc(1, bitmap_bytes);
    if (!bitmap) return -9;
    rc = blockdev_write(dev_index, super.bitmap_start, super.bitmap_sectors, bitmap);
    kfree(bitmap);
    if (rc < 0) return -10;

    return 0;
}

int minfs_mount(uint32_t dev_index, const char* mount_name) {
    if (minfs_find_mount(dev_index)) return 0;
    if (g_mount_count >= MINFS_MAX_MOUNTS) return -1;

    const block_device_t* dev = blockdev_get(dev_index);
    if (!dev) return -2;

    minfs_superblock_disk_t super;
    if (minfs_read_super(dev_index, &super) < 0) return -3;
    if (!minfs_super_is_valid(&super, dev)) return -4;

    vfs_node_t* root = vfs_get_root();
    if (!root) return -5;

    vfs_node_t* mnt = NULL;
    if (minfs_ensure_mount_dir(root, "mnt", &mnt) < 0) return -6;

    const char* mp_name = mount_name;
    if (!mp_name || *mp_name == '\0') mp_name = dev->name;

    vfs_node_t* mountpoint = NULL;
    if (minfs_ensure_mount_dir(mnt, mp_name, &mountpoint) < 0) return -7;

    minfs_t* fs = (minfs_t*)kcalloc(1, sizeof(minfs_t));
    if (!fs) return -8;

    fs->dev_index = dev_index;
    fs->total_nodes = super.total_nodes;
    fs->block_size = super.block_size;
    fs->max_file_bytes = super.max_file_bytes;
    fs->node_table_start = super.node_table_start;
    fs->node_table_sectors = super.node_table_sectors;
    fs->bitmap_start = super.bitmap_start;
    fs->bitmap_sectors = super.bitmap_sectors;
    fs->bitmap_bytes = fs->bitmap_sectors * MINFS_BLOCK_SIZE;
    fs->data_start_sector = super.data_start_sector;
    fs->data_block_count = super.data_block_count;
    fs->nodes = (minfs_disk_node_t*)kcalloc(fs->total_nodes, sizeof(minfs_disk_node_t));
    fs->bitmap = (uint8_t*)kcalloc(1, fs->bitmap_bytes);
    fs->impls = (minfs_node_impl_t*)kcalloc(fs->total_nodes, sizeof(minfs_node_impl_t));
    fs->vnodes = (vfs_node_t**)kcalloc(fs->total_nodes, sizeof(vfs_node_t*));
    if (!fs->nodes || !fs->bitmap || !fs->impls || !fs->vnodes) {
        minfs_free_mount(fs);
        return -9;
    }

    uint32_t table_bytes = fs->node_table_sectors * MINFS_BLOCK_SIZE;
    uint8_t* table = (uint8_t*)kcalloc(1, table_bytes);
    if (!table) {
        minfs_free_mount(fs);
        return -10;
    }
    if (blockdev_read(dev_index, fs->node_table_start, fs->node_table_sectors, table) < 0) {
        kfree(table);
        minfs_free_mount(fs);
        return -11;
    }
    memcpy(fs->nodes, table, fs->total_nodes * sizeof(minfs_disk_node_t));
    kfree(table);

    if (blockdev_read(dev_index, fs->bitmap_start, fs->bitmap_sectors, fs->bitmap) < 0) {
        minfs_free_mount(fs);
        return -12;
    }

    if (!fs->nodes[0].used || !(fs->nodes[0].type & VFS_DIRECTORY)) {
        minfs_free_mount(fs);
        return -13;
    }

    for (uint32_t i = 0; i < fs->total_nodes; ++i) {
        fs->impls[i].fs = fs;
        fs->impls[i].index = i;
    }

    fs->vnodes[0] = mountpoint;
    minfs_wire_vnode(fs, 0, mountpoint, 1);

    for (uint32_t i = 1; i < fs->total_nodes; ++i) {
        if (!fs->nodes[i].used) continue;
        if (!minfs_alloc_vnode(fs, i)) {
            minfs_free_mount(fs);
            return -14;
        }
    }

    minfs_refresh_directory_sizes(fs);
    g_mounts[g_mount_count++] = fs;
    klog("minfs: mounted bitmap-backed filesystem");
    return 0;
}

void minfs_auto_mount(void) {
    uint32_t n = blockdev_count();
    for (uint32_t i = 0; i < n; ++i) {
        const block_device_t* d = blockdev_get(i);
        if (!d) continue;
        if (!(d->flags & BLOCKDEV_FLAG_PARTITION)) continue;
        if (minfs_find_mount(i)) continue;

        minfs_superblock_disk_t super;
        if (minfs_read_super(i, &super) < 0) continue;
        if (!minfs_super_is_valid(&super, d)) continue;
        minfs_mount(i, d->name);
    }
}

