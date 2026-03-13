#ifndef _KERNEL_PAGING_H
#define _KERNEL_PAGING_H

#include <stdint.h>
#include "multiboot.h"

void paging_initialize(struct multiboot_tag_framebuffer* fb_tag);

uintptr_t paging_heap_base(void);
uintptr_t paging_heap_size(void);

/*
 * Map a single 4 KiB page at `vaddr` with user-accessible permissions
 * (PTE_P | PTE_W | PTE_U).  Allocates a fresh physical frame.
 * Skips silently if the page is already mapped.
 * Returns 0 on success, negative on error.
 */
int paging_map_user(uintptr_t vaddr);

#endif