#ifndef _KERNEL_RAMFS_H
#define _KERNEL_RAMFS_H

#include "vfs.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ramfs — In-memory filesystem backed by the kernel heap.
 *
 * Call ramfs_initialize() after vfs_initialize() to create the root "/"
 * directory and mount it as the VFS root.
 */

void ramfs_initialize(void);

#ifdef __cplusplus
}
#endif

#endif /* _KERNEL_RAMFS_H */

