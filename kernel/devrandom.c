/*
 * devrandom.c — /dev/random and /dev/urandom character devices.
 *
 * Uses the VFS vtable-override pattern: create an ordinary ramfs file node,
 * then replace its read/write pointers with ours. The ramfs allocator stows
 * a heap-allocated ramfs_file_data_t in node->impl for every VFS_FILE it
 * creates; since these nodes are pure device shims (no backing buffer), we
 * free that orphaned allocation and null impl out rather than repurposing it
 * to carry a mode tag. That keeps ramfs_unlink() (which dereferences impl as
 * a ramfs_file_data_t*) safe if /dev/random or /dev/urandom is ever removed.
 */

#include "include/kernel/random.h"
#include "include/kernel/vfs.h"
#include "include/kernel/task.h"
#include "include/kernel/kheap.h"
#include <stddef.h>
#include <stdint.h>

typedef int32_t (*devrandom_read_fn)(vfs_node_t*, uint32_t, uint32_t, uint8_t*);

static int32_t devrandom_read_urandom(vfs_node_t* node, uint32_t offset,
                                      uint32_t size, uint8_t* buf) {
    (void)node; (void)offset;
    if (!buf || size == 0u) return 0;
    random_bytes(buf, size);
    return (int32_t)size;   /* character device: never EOF */
}

static int32_t devrandom_read_random(vfs_node_t* node, uint32_t offset,
                                     uint32_t size, uint8_t* buf) {
    (void)node; (void)offset;
    if (!buf || size == 0u) return 0;
    while (!random_is_seeded()) task_sleep_ms(10);
    random_bytes(buf, size);
    return (int32_t)size;   /* character device: never EOF */
}

static int32_t devrandom_write(vfs_node_t* node, uint32_t offset,
                               uint32_t size, const uint8_t* buf) {
    (void)node; (void)offset;
    if (!buf) return -1;
    random_add_entropy(buf, size, 0u);   /* adds mixing material, not estimate */
    return (int32_t)size;
}

static void make_dev(vfs_node_t* dev, const char* name, devrandom_read_fn read_fn) {
    if (vfs_create(dev, name, VFS_FILE) < 0) return;
    vfs_node_t* n = vfs_finddir(dev, name);
    if (!n) return;
    n->read  = read_fn;
    n->write = devrandom_write;
    /* Discard the ramfs-allocated file buffer: these nodes are device shims,
     * not backed by ramfs storage, and a stray impl pointer would confuse
     * ramfs_unlink() if the node is ever removed. */
    if (n->impl) {
        kfree(n->impl);
    }
    n->impl  = NULL;
    n->size  = 0;
}

void devrandom_initialize(void) {
    vfs_node_t* root = vfs_get_root();
    if (!root) return;
    vfs_create(root, "dev", VFS_DIRECTORY);
    vfs_node_t* dev = vfs_finddir(root, "dev");
    if (!dev) return;
    make_dev(dev, "urandom", devrandom_read_urandom);
    make_dev(dev, "random", devrandom_read_random);
}
