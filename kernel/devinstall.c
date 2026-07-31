/*
 * devinstall.c — /dev/installkernel, /dev/installgrubcore,
 * /dev/installgrubcfg: read-only views onto the build-time-embedded
 * install payload (see
 * docs/superpowers/specs/2026-07-31-disk-install-design.md). Same VFS
 * vtable-override pattern as kernel/devrandom.c, but backed by a fixed
 * byte range instead of a live data source, so reads are offset-bounded
 * rather than an infinite stream.
 */

#include "include/kernel/devinstall.h"
#include "include/kernel/vfs.h"
#include "include/kernel/kheap.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern const uint8_t _binary_build_deanos_payload_bin_start[] __attribute__((weak));
extern const uint8_t _binary_build_deanos_payload_bin_end[] __attribute__((weak));
extern const uint8_t _binary_build_install_grub_combined_img_start[] __attribute__((weak));
extern const uint8_t _binary_build_install_grub_combined_img_end[] __attribute__((weak));
extern const uint8_t _binary_grub_install_cfg_start[] __attribute__((weak));
extern const uint8_t _binary_grub_install_cfg_end[] __attribute__((weak));

typedef struct {
    const uint8_t* data;
    uint32_t size;
} devinstall_blob_t;

static devinstall_blob_t g_blobs[3];

static int32_t devinstall_read(vfs_node_t* node, uint32_t offset,
                                uint32_t size, uint8_t* buf) {
    const devinstall_blob_t* blob = (const devinstall_blob_t*)node->impl;
    if (!blob || !buf) return -1;
    if (offset >= blob->size) return 0;
    uint32_t remaining = blob->size - offset;
    uint32_t chunk = (size < remaining) ? size : remaining;
    memcpy(buf, blob->data + offset, chunk);
    return (int32_t)chunk;
}

static int32_t devinstall_write(vfs_node_t* node, uint32_t offset,
                                 uint32_t size, const uint8_t* buf) {
    (void)node; (void)offset; (void)size; (void)buf;
    return -1;
}

static void make_blob_dev(vfs_node_t* dev, const char* name,
                           devinstall_blob_t* slot,
                           const uint8_t* start, const uint8_t* end) {
    /* Skip creating a file for 0-sized blobs (e.g., in pass-1 payload build). */
    if (end == start) return;
    if (vfs_create(dev, name, VFS_FILE) < 0) return;
    vfs_node_t* n = vfs_finddir(dev, name);
    if (!n) return;
    slot->data = start;
    slot->size = (uint32_t)(end - start);
    /* Discard the ramfs-allocated file buffer: these nodes are blob
     * shims, not backed by ramfs storage (same reasoning as
     * devrandom.c's make_dev). */
    if (n->impl) {
        kfree(n->impl);
    }
    n->impl  = slot;
    n->read  = devinstall_read;
    n->write = devinstall_write;
    n->size  = slot->size;
}

void devinstall_initialize(void) {
    vfs_node_t* root = vfs_get_root();
    if (!root) return;
    vfs_create(root, "dev", VFS_DIRECTORY);
    vfs_node_t* dev = vfs_finddir(root, "dev");
    if (!dev) return;
    make_blob_dev(dev, "installkernel", &g_blobs[0],
                  _binary_build_deanos_payload_bin_start,
                  _binary_build_deanos_payload_bin_end);
    make_blob_dev(dev, "installgrubcore", &g_blobs[1],
                  _binary_build_install_grub_combined_img_start,
                  _binary_build_install_grub_combined_img_end);
    make_blob_dev(dev, "installgrubcfg", &g_blobs[2],
                  _binary_grub_install_cfg_start,
                  _binary_grub_install_cfg_end);
}
