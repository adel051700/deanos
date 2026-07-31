/*
 * devinstall.c — /dev/installkernel, /dev/installgrubcore,
 * /dev/installgrubcfg: read-only views onto the build-time-embedded
 * install payload (see
 * docs/superpowers/specs/2026-07-31-disk-install-design.md). Same VFS
 * vtable-override pattern as kernel/devrandom.c, but backed by a fixed
 * byte range instead of a live data source, so reads are offset-bounded
 * rather than an infinite stream.
 *
 * Each blob's start/size lives in file-scope statics closed over by its
 * own read function (not node->impl) — same reasoning as devrandom.c's
 * make_dev(): ramfs_unlink() unconditionally casts node->impl to
 * ramfs_file_data_t* and frees it, so these nodes must leave impl NULL.
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

typedef int32_t (*devinstall_read_fn)(vfs_node_t*, uint32_t, uint32_t, uint8_t*);

static int32_t devinstall_read_range(const uint8_t* start, uint32_t blob_size,
                                      uint32_t offset, uint32_t size, uint8_t* buf) {
    if (!buf) return -1;
    if (offset >= blob_size) return 0;
    uint32_t remaining = blob_size - offset;
    uint32_t chunk = (size < remaining) ? size : remaining;
    memcpy(buf, start + offset, chunk);
    return (int32_t)chunk;
}

static int32_t devinstall_read_kernel(vfs_node_t* node, uint32_t offset,
                                       uint32_t size, uint8_t* buf) {
    (void)node;
    uint32_t blob_size = (uint32_t)(_binary_build_deanos_payload_bin_end -
                                     _binary_build_deanos_payload_bin_start);
    return devinstall_read_range(_binary_build_deanos_payload_bin_start,
                                  blob_size, offset, size, buf);
}

static int32_t devinstall_read_grubcore(vfs_node_t* node, uint32_t offset,
                                         uint32_t size, uint8_t* buf) {
    (void)node;
    uint32_t blob_size = (uint32_t)(_binary_build_install_grub_combined_img_end -
                                     _binary_build_install_grub_combined_img_start);
    return devinstall_read_range(_binary_build_install_grub_combined_img_start,
                                  blob_size, offset, size, buf);
}

static int32_t devinstall_read_grubcfg(vfs_node_t* node, uint32_t offset,
                                        uint32_t size, uint8_t* buf) {
    (void)node;
    uint32_t blob_size = (uint32_t)(_binary_grub_install_cfg_end -
                                     _binary_grub_install_cfg_start);
    return devinstall_read_range(_binary_grub_install_cfg_start,
                                  blob_size, offset, size, buf);
}

static int32_t devinstall_write(vfs_node_t* node, uint32_t offset,
                                 uint32_t size, const uint8_t* buf) {
    (void)node; (void)offset; (void)size; (void)buf;
    return -1;
}

static void make_blob_dev(vfs_node_t* dev, const char* name,
                           devinstall_read_fn read_fn,
                           const uint8_t* start, const uint8_t* end) {
    /* Skip creating a file for 0-sized blobs (e.g., in pass-1 payload build). */
    if (end == start) return;
    if (vfs_create(dev, name, VFS_FILE) < 0) return;
    vfs_node_t* n = vfs_finddir(dev, name);
    if (!n) return;
    /* Discard the ramfs-allocated file buffer: these nodes are blob
     * shims, not backed by ramfs storage (same reasoning as
     * devrandom.c's make_dev). */
    if (n->impl) {
        kfree(n->impl);
    }
    n->impl  = NULL;
    n->read  = read_fn;
    n->write = devinstall_write;
    n->size  = (uint32_t)(end - start);
}

void devinstall_initialize(void) {
    vfs_node_t* root = vfs_get_root();
    if (!root) return;
    vfs_create(root, "dev", VFS_DIRECTORY);
    vfs_node_t* dev = vfs_finddir(root, "dev");
    if (!dev) return;
    make_blob_dev(dev, "installkernel", devinstall_read_kernel,
                  _binary_build_deanos_payload_bin_start,
                  _binary_build_deanos_payload_bin_end);
    make_blob_dev(dev, "installgrubcore", devinstall_read_grubcore,
                  _binary_build_install_grub_combined_img_start,
                  _binary_build_install_grub_combined_img_end);
    make_blob_dev(dev, "installgrubcfg", devinstall_read_grubcfg,
                  _binary_grub_install_cfg_start,
                  _binary_grub_install_cfg_end);
}
