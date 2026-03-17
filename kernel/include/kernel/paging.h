#ifndef _KERNEL_PAGING_H
#define _KERNEL_PAGING_H

#include <stdint.h>
#include "multiboot.h"

/* Public mapping flags used by demand-region registration. */
#define PAGING_FLAG_WRITE 0x002u
#define PAGING_FLAG_USER  0x004u

typedef struct paging_stats {
	uint32_t demand_regions;
	uint32_t demand_faults;
	uint32_t cow_faults;
} paging_stats_t;

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

/* Register a lazily populated region. Pages are allocated on first fault. */
int paging_register_demand_region(uintptr_t start, uintptr_t size, uint32_t flags);

/* Mark an already-mapped user page as copy-on-write (hook for future fork). */
int paging_mark_cow(uintptr_t vaddr);

/* Query VM hook counters for diagnostics. */
void paging_get_stats(paging_stats_t* out);

/* Fork groundwork: clone/release current shared mm metadata (no deep page copy). */
int paging_clone_current_mm_metadata(uint32_t* out_mm_id, uint32_t* out_mm_flags);
void paging_release_mm_metadata(uint32_t mm_id, uint32_t mm_flags);

#endif