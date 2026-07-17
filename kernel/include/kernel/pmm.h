#ifndef _KERNEL_PMM_H
#define _KERNEL_PMM_H

#include <stdint.h>
#include "multiboot.h"

/* Frame size (4KiB) */
#define PMM_FRAME_SIZE 4096

/* Initialize the physical memory manager using the multiboot2 memory map tag */
void pmm_initialize(struct multiboot_tag_mmap* mmap_tag);

/* Allocate/free a single 4KiB frame; returns 0 on failure */
uintptr_t phys_alloc_frame(void);
void      phys_free_frame(uintptr_t phys_addr);

/* Frame refcount helpers (used by VM/COW hooks). */
void     pmm_frame_ref(uintptr_t phys_addr);
void     pmm_frame_unref(uintptr_t phys_addr);
uint16_t pmm_frame_refcount(uintptr_t phys_addr);

/* Stats */
uint32_t  pmm_total_frames(void);
uint32_t  pmm_free_frames(void);

#endif