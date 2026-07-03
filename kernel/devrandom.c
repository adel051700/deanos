/*
 * devrandom.c — /dev/random and /dev/urandom character devices.
 *
 * Uses the VFS vtable-override pattern: create an ordinary ramfs file node,
 * then replace its read/write pointers with ours. impl encodes the mode:
 *   0 -> urandom (never blocks)
 *   1 -> random  (blocks until the pool is seeded once, then identical)
 */

#include "include/kernel/random.h"
#include "include/kernel/vfs.h"
#include "include/kernel/task.h"
#include <stddef.h>
#include <stdint.h>

static int32_t devrandom_read(vfs_node_t* node, uint32_t offset,
                              uint32_t size, uint8_t* buf) {
    (void)offset;
    if (!buf || size == 0u) return 0;
    int blocking = (int)(uintptr_t)node->impl;
    if (blocking) {
        while (!random_is_seeded()) task_sleep_ms(10);
    }
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

static void make_dev(vfs_node_t* dev, const char* name, int blocking) {
    if (vfs_create(dev, name, VFS_FILE) < 0) return;
    vfs_node_t* n = vfs_finddir(dev, name);
    if (!n) return;
    n->read  = devrandom_read;
    n->write = devrandom_write;
    n->impl  = (void*)(uintptr_t)blocking;
    n->size  = 0;
}

void devrandom_initialize(void) {
    vfs_node_t* root = vfs_get_root();
    if (!root) return;
    vfs_create(root, "dev", VFS_DIRECTORY);
    vfs_node_t* dev = vfs_finddir(root, "dev");
    if (!dev) return;
    make_dev(dev, "urandom", 0);
    make_dev(dev, "random", 1);
}
