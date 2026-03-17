#include "include/kernel/paging.h"
#include "include/kernel/pmm.h"
#include "../libc/include/string.h"
#include "include/kernel/interrupt.h"
#include "include/kernel/tty.h"
#include "../libc/include/stdio.h"

#define PAGE_SIZE 4096

// Flags
#define PTE_P  0x001
#define PTE_W  0x002
#define PTE_U  0x004
#define PTE_COW 0x200
#define MM_FLAG_SHARED 0x1u

// Simple kernel heap mapping (virtual)
#define KHEAP_BASE 0x40000000u
#define KHEAP_SIZE (4u * 1024u * 1024u) // 4 MiB for now

// Page directory and a small pool of page tables in .bss (identity mapped)
static uint32_t page_directory[1024] __attribute__((aligned(4096)));
#define INIT_PT_COUNT 64
static uint32_t page_tables[INIT_PT_COUNT][1024] __attribute__((aligned(4096)));
static uint32_t* pde_virt_tables[1024];
static int pt_used = 0;

#define DEMAND_REGION_MAX 32
typedef struct demand_region {
    uintptr_t start;
    uintptr_t end;
    uint32_t flags;
    uint8_t used;
} demand_region_t;

static demand_region_t g_demand_regions[DEMAND_REGION_MAX];
static uint32_t g_demand_region_count = 0;
static uint32_t g_demand_fault_count = 0;
static uint32_t g_cow_fault_count = 0;
static uint8_t g_cow_scratch[PAGE_SIZE];
static uint32_t g_mm_root_id = 1;
static uint32_t g_mm_shared_refs = 1;

// Helpers
static inline void load_cr3(uint32_t phys) { __asm__ __volatile__("mov %0, %%cr3" : : "r"(phys) : "memory"); }
static inline void enable_paging(void) {
    uint32_t cr0;
    __asm__ __volatile__("mov %%cr0, %0":"=r"(cr0));
    cr0 |= 0x80000000u; /* PG */
    cr0 |= 0x00010000u; /* WP: honor read-only PTEs in supervisor mode */
    __asm__ __volatile__("mov %0, %%cr0"::"r"(cr0));
}
static inline void invlpg(void* addr) { __asm__ __volatile__("invlpg (%0)" :: "r"(addr) : "memory"); }

static uint32_t* alloc_page_table(void) {
    if (pt_used >= INIT_PT_COUNT) return NULL;
    uint32_t* pt = page_tables[pt_used++];
    memset(pt, 0, 4096);
    return pt;
}

static uint32_t* get_pt(uint32_t dir_idx) {
    if (!(page_directory[dir_idx] & PTE_P)) return NULL;
    return pde_virt_tables[dir_idx];
}

static uint32_t* ensure_pt(uint32_t dir_idx) {
    if (page_directory[dir_idx] & PTE_P) {
        return pde_virt_tables[dir_idx];
    }
    uint32_t* pt = alloc_page_table();
    if (!pt) return NULL;
    /* PDE gets P|W|U so user-mode PTEs beneath it can work. */
    page_directory[dir_idx] = ((uint32_t)pt & ~0xFFFu) | (PTE_P | PTE_W | PTE_U);
    pde_virt_tables[dir_idx] = pt;
    return pt;
}

static int map_page(uintptr_t virt, uintptr_t phys, uint32_t flags) {
    uint32_t pd = (virt >> 22) & 0x3FF;
    uint32_t ti = (virt >> 12) & 0x3FF;
    uint32_t* pt = ensure_pt(pd);
    if (!pt) return -1;
    pt[ti] = (phys & ~0xFFFu) | (flags & 0xFFF);
    invlpg((void*)virt);
    return 0;
}

static void identity_map_range(uintptr_t start, uintptr_t size, uint32_t flags) {
    uintptr_t s = start & ~(PAGE_SIZE - 1);
    uintptr_t e = (start + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (uintptr_t a = s; a < e; a += PAGE_SIZE) {
        map_page(a, a, flags);
    }
}

static void map_region_new_frames(uintptr_t vbase, uintptr_t size, uint32_t flags) {
    uintptr_t s = vbase & ~(PAGE_SIZE - 1);
    uintptr_t e = (vbase + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (uintptr_t va = s; va < e; va += PAGE_SIZE) {
        uintptr_t pa = phys_alloc_frame();
        if (!pa) {
            // silent failure; could add fallback or panic later
            return;
        }
        map_page(va, pa, flags);
    }
}

static demand_region_t* find_demand_region(uintptr_t addr) {
    for (uint32_t i = 0; i < DEMAND_REGION_MAX; ++i) {
        if (!g_demand_regions[i].used) continue;
        if (addr >= g_demand_regions[i].start && addr < g_demand_regions[i].end)
            return &g_demand_regions[i];
    }
    return NULL;
}

static int handle_demand_fault(uintptr_t fault_addr, uint32_t err_code) {
    if (err_code & 0x1u) return 0; /* present/protection fault, not demand */
    demand_region_t* region = find_demand_region(fault_addr);
    if (!region) return 0;

    uintptr_t page = fault_addr & ~0xFFFu;
    uintptr_t pa = phys_alloc_frame();
    if (!pa) return -1;

    uint32_t map_flags = PTE_P | (region->flags & (PTE_W | PTE_U));
    if (map_page(page, pa, map_flags) < 0) {
        phys_free_frame(pa);
        return -1;
    }
    memset((void*)page, 0, PAGE_SIZE);
    g_demand_fault_count++;
    return 1;
}

static int handle_cow_fault(uintptr_t fault_addr, uint32_t err_code) {
    if ((err_code & 0x1u) == 0u) return 0;  /* not-present faults are not COW */
    if ((err_code & 0x2u) == 0u) return 0;  /* COW triggers on write faults */

    uintptr_t page = fault_addr & ~0xFFFu;
    uint32_t pd = (page >> 22) & 0x3FF;
    uint32_t ti = (page >> 12) & 0x3FF;
    uint32_t* pt = get_pt(pd);
    if (!pt) return 0;

    uint32_t pte = pt[ti];
    if ((pte & PTE_P) == 0u) return 0;
    if ((pte & PTE_COW) == 0u) return 0;

    uintptr_t old_pa = (uintptr_t)(pte & ~0xFFFu);
    uint16_t refs = pmm_frame_refcount(old_pa);

    if (refs <= 1) {
        pt[ti] = (pte | PTE_W) & ~PTE_COW;
        invlpg((void*)page);
        g_cow_fault_count++;
        return 1;
    }

    memcpy(g_cow_scratch, (const void*)page, PAGE_SIZE);

    uintptr_t new_pa = phys_alloc_frame();
    if (!new_pa) return -1;

    uint32_t new_flags = (pte & PTE_U) | PTE_P | PTE_W;
    if (map_page(page, new_pa, new_flags) < 0) {
        phys_free_frame(new_pa);
        return -1;
    }

    memcpy((void*)page, g_cow_scratch, PAGE_SIZE);
    pmm_frame_unref(old_pa);
    g_cow_fault_count++;
    return 1;
}

static void page_fault_handler(struct registers* r) {
    uint32_t fault_addr;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(fault_addr));
    if (handle_demand_fault(fault_addr, r->err_code) > 0) return;
    if (handle_cow_fault(fault_addr, r->err_code) > 0) return;

    char buf[16];
    terminal_writestring("\nPAGE FAULT at 0x");
    itoa(fault_addr, buf, 16);
    terminal_writestring(buf);
    terminal_writestring("  EIP=0x");
    itoa(r->eip, buf, 16);
    terminal_writestring(buf);
    terminal_writestring("\nSystem halted.\n");
    while (1) { __asm__ __volatile__("hlt"); }
}

// Exported heap info
uintptr_t paging_heap_base(void) { return (uintptr_t)KHEAP_BASE; }
uintptr_t paging_heap_size(void) { return (uintptr_t)KHEAP_SIZE; }

int paging_map_user(uintptr_t vaddr) {
    uint32_t pd_idx = (vaddr >> 22) & 0x3FF;
    uint32_t pt_idx = (vaddr >> 12) & 0x3FF;

    uint32_t* pt = ensure_pt(pd_idx);
    if (!pt) return -1;

    /* Already mapped → nothing to do. */
    if (pt[pt_idx] & PTE_P) return 0;

    uintptr_t pa = phys_alloc_frame();
    if (!pa) return -2;

    pt[pt_idx] = (pa & ~0xFFFu) | (PTE_P | PTE_W | PTE_U);
    invlpg((void*)(vaddr & ~0xFFFu));

    /* Zero the freshly mapped page. */
    memset((void*)(vaddr & ~0xFFFu), 0, PAGE_SIZE);
    return 0;
}

int paging_register_demand_region(uintptr_t start, uintptr_t size, uint32_t flags) {
    if (size == 0) return -1;
    uintptr_t s = start & ~0xFFFu;
    uintptr_t e = (start + size + PAGE_SIZE - 1) & ~0xFFFu;
    if (e <= s) return -2;

    for (uint32_t i = 0; i < DEMAND_REGION_MAX; ++i) {
        if (!g_demand_regions[i].used) continue;
        uintptr_t rs = g_demand_regions[i].start;
        uintptr_t re = g_demand_regions[i].end;
        if (!(e <= rs || s >= re)) return -3;
    }

    for (uint32_t i = 0; i < DEMAND_REGION_MAX; ++i) {
        if (g_demand_regions[i].used) continue;
        g_demand_regions[i].start = s;
        g_demand_regions[i].end = e;
        g_demand_regions[i].flags = flags;
        g_demand_regions[i].used = 1;
        g_demand_region_count++;
        return 0;
    }

    return -4;
}

int paging_mark_cow(uintptr_t vaddr) {
    uintptr_t page = vaddr & ~0xFFFu;
    uint32_t pd = (page >> 22) & 0x3FF;
    uint32_t ti = (page >> 12) & 0x3FF;
    uint32_t* pt = get_pt(pd);
    if (!pt) return -1;
    if ((pt[ti] & PTE_P) == 0u) return -2;

    pt[ti] = (pt[ti] | PTE_COW) & ~PTE_W;
    invlpg((void*)page);
    return 0;
}

void paging_get_stats(paging_stats_t* out) {
    if (!out) return;
    out->demand_regions = g_demand_region_count;
    out->demand_faults = g_demand_fault_count;
    out->cow_faults = g_cow_fault_count;
}

int paging_clone_current_mm_metadata(uint32_t* out_mm_id, uint32_t* out_mm_flags) {
    if (!out_mm_id || !out_mm_flags) return -1;
    g_mm_shared_refs++;
    *out_mm_id = g_mm_root_id;
    *out_mm_flags = MM_FLAG_SHARED;
    return 0;
}

void paging_release_mm_metadata(uint32_t mm_id, uint32_t mm_flags) {
    if (mm_id != g_mm_root_id) return;
    if ((mm_flags & MM_FLAG_SHARED) == 0u) return;
    if (g_mm_shared_refs > 1) g_mm_shared_refs--;
}

void paging_initialize(struct multiboot_tag_framebuffer* fb_tag) {
    memset(page_directory, 0, sizeof(page_directory));
    memset(pde_virt_tables, 0, sizeof(pde_virt_tables));
    memset(g_demand_regions, 0, sizeof(g_demand_regions));
    g_demand_region_count = 0;
    g_demand_fault_count = 0;
    g_cow_fault_count = 0;
    g_mm_root_id = 1;
    g_mm_shared_refs = 1;

    // Leave page 0 unmapped to catch NULL derefs; start identity map at 0x1000
    uintptr_t reserved_end = pmm_reserved_region_end();
    if (reserved_end < 0x1000) reserved_end = 0x1000;

    // Identity map kernel + boot reserved region
    // PTE_U is set so ring-3 test tasks can execute kernel-linked user code.
    // A real OS would map user code into separate address space.
    identity_map_range(0x1000, reserved_end - 0x1000, PTE_P | PTE_W | PTE_U);

    // Identity map linear framebuffer if present
    if (fb_tag) {
        uintptr_t fb_addr = (uintptr_t)fb_tag->framebuffer_addr;
        uintptr_t fb_size = (uintptr_t)fb_tag->framebuffer_pitch * fb_tag->framebuffer_height;
        identity_map_range(fb_addr, fb_size, PTE_P | PTE_W);
    }

    // Map a kernel heap region at KHEAP_BASE (user-accessible for user stacks)
    map_region_new_frames(KHEAP_BASE, KHEAP_SIZE, PTE_P | PTE_W | PTE_U);
    register_interrupt_handler(14, page_fault_handler);
    load_cr3((uint32_t)page_directory);
    enable_paging();
}