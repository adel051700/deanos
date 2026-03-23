#ifndef _KERNEL_MINFS_H
#define _KERNEL_MINFS_H

#include <stdint.h>

int minfs_format(uint32_t dev_index);
int minfs_mount(uint32_t dev_index, const char* mount_name);
void minfs_auto_mount(void);
int minfs_test_mark_dirty(uint32_t dev_index);

#endif

