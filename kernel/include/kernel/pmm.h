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

/* Allocate N contiguous frames. align_frames must be a power of two (in frames), e.g. 1,2,4...
   Returns 0 on failure. */
uintptr_t phys_alloc_contiguous(uint32_t count, uint32_t align_frames);

/* Stats */
uint32_t  pmm_total_frames(void);
uint32_t  pmm_free_frames(void);
int pmm_self_test(uint32_t frames_to_test);

uintptr_t pmm_reserved_region_end(void);
#endif