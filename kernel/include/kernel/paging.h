#ifndef _KERNEL_PAGING_H
#define _KERNEL_PAGING_H

#include <stdint.h>
#include "multiboot.h"

void paging_initialize(struct multiboot_tag_framebuffer* fb_tag);

uintptr_t paging_heap_base(void);
uintptr_t paging_heap_size(void);

#endif