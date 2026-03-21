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

// Simple kernel heap mapping (virtual)
#define KHEAP_BASE 0x40000000u
#define KHEAP_SIZE (4u * 1024u * 1024u) // 4 MiB for now

/* Active/kernel CR3 tracking for per-process page directories. */
static uint32_t g_kernel_cr3 = 0;
static uint32_t g_current_cr3 = 0;

#define MM_SLOT_MAX 64
typedef struct mm_slot {
    uint32_t cr3;
    uint32_t refs;
    uint8_t  used;
} mm_slot_t;
static mm_slot_t g_mm_slots[MM_SLOT_MAX];

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

// Helpers
static inline void load_cr3(uint32_t phys) { __asm__ __volatile__("mov %0, %%cr3" : : "r"(phys) : "memory"); }
static inline uint32_t read_cr3(void) {
    uint32_t v;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(v));
    return v;
}
static inline void enable_paging(void) {
    uint32_t cr0;
    __asm__ __volatile__("mov %%cr0, %0":"=r"(cr0));
    cr0 |= 0x80000000u; /* PG */
    cr0 |= 0x00010000u; /* WP: honor read-only PTEs in supervisor mode */
    __asm__ __volatile__("mov %0, %%cr0"::"r"(cr0));
}
static inline void invlpg(void* addr) { __asm__ __volatile__("invlpg (%0)" :: "r"(addr) : "memory"); }

static uint32_t* pd_from_phys(uint32_t phys) {
    return (uint32_t*)(uintptr_t)(phys & ~0xFFFu);
}

static int mm_find_slot(uint32_t cr3) {
    for (int i = 0; i < MM_SLOT_MAX; ++i) {
        if (g_mm_slots[i].used && g_mm_slots[i].cr3 == cr3) return i;
    }
    return -1;
}

static int mm_alloc_slot(uint32_t cr3, uint32_t refs) {
    for (int i = 0; i < MM_SLOT_MAX; ++i) {
        if (g_mm_slots[i].used) continue;
        g_mm_slots[i].used = 1;
        g_mm_slots[i].cr3 = cr3;
        g_mm_slots[i].refs = refs;
        return i;
    }
    return -1;
}

static uintptr_t alloc_zeroed_frame(void) {
    uintptr_t pa = phys_alloc_frame();
    if (!pa) return 0;
    memset((void*)pa, 0, PAGE_SIZE);
    return pa;
}

static uint32_t* get_pt_from_pd(uint32_t* pd, uint32_t dir_idx) {
    if (!pd) return NULL;
    if ((pd[dir_idx] & PTE_P) == 0u) return NULL;
    return (uint32_t*)(uintptr_t)(pd[dir_idx] & ~0xFFFu);
}

static uint32_t* ensure_pt_in_pd(uint32_t* pd, uint32_t dir_idx, uint32_t pde_flags) {
    if (!pd) return NULL;
    if (pd[dir_idx] & PTE_P) return (uint32_t*)(uintptr_t)(pd[dir_idx] & ~0xFFFu);

    uintptr_t pt_pa = alloc_zeroed_frame();
    if (!pt_pa) return NULL;
    pd[dir_idx] = (uint32_t)(pt_pa & ~0xFFFu) | (PTE_P | (pde_flags & (PTE_W | PTE_U)));
    return (uint32_t*)(uintptr_t)pt_pa;
}

static int map_page_in_pd(uint32_t* pd, uintptr_t virt, uintptr_t phys, uint32_t flags) {
    uint32_t dir_idx = (virt >> 22) & 0x3FF;
    uint32_t ti = (virt >> 12) & 0x3FF;
    uint32_t* pt = ensure_pt_in_pd(pd, dir_idx, flags);
    if (!pt) return -1;
    pt[ti] = (phys & ~0xFFFu) | (flags & 0xFFF);
    if ((g_current_cr3 & ~0xFFFu) == (((uintptr_t)pd) & ~0xFFFu)) {
        invlpg((void*)virt);
    }
    return 0;
}

static int map_page(uintptr_t virt, uintptr_t phys, uint32_t flags) {
    uint32_t* pd = pd_from_phys(g_current_cr3);
    return map_page_in_pd(pd, virt, phys, flags);
}

static void identity_map_range(uint32_t* pd, uintptr_t start, uintptr_t size, uint32_t flags) {
    uintptr_t s = start & ~(PAGE_SIZE - 1);
    uintptr_t e = (start + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (uintptr_t a = s; a < e; a += PAGE_SIZE) {
        map_page_in_pd(pd, a, a, flags);
    }
}

static void map_region_new_frames(uint32_t* pd, uintptr_t vbase, uintptr_t size, uint32_t flags) {
    uintptr_t s = vbase & ~(PAGE_SIZE - 1);
    uintptr_t e = (vbase + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (uintptr_t va = s; va < e; va += PAGE_SIZE) {
        uintptr_t pa = phys_alloc_frame();
        if (!pa) {
            // silent failure; could add fallback or panic later
            return;
        }
        map_page_in_pd(pd, va, pa, flags);
    }
}

static void clone_kernel_mappings(uint32_t* dst_pd) {
    uint32_t* src_pd = pd_from_phys(g_kernel_cr3);
    for (uint32_t i = 0; i < 1024; ++i) {
        uint32_t pde = src_pd[i];
        if ((pde & PTE_P) == 0u) continue;
        if (pde & PTE_U) continue;
        dst_pd[i] = pde;
    }
}

static void destroy_mm(uint32_t cr3_phys) {
    if (!cr3_phys) return;
    uint32_t* pd = pd_from_phys(cr3_phys);
    if (!pd) return;

    for (uint32_t i = 0; i < 1024; ++i) {
        uint32_t pde = pd[i];
        if ((pde & PTE_P) == 0u) continue;
        if ((pde & PTE_U) == 0u) continue;

        uint32_t* pt = (uint32_t*)(uintptr_t)(pde & ~0xFFFu);
        for (uint32_t j = 0; j < 1024; ++j) {
            uint32_t pte = pt[j];
            if ((pte & PTE_P) == 0u) continue;
            uintptr_t pa = (uintptr_t)(pte & ~0xFFFu);
            pmm_frame_unref(pa);
        }

        phys_free_frame((uintptr_t)(pde & ~0xFFFu));
    }

    phys_free_frame((uintptr_t)(cr3_phys & ~0xFFFu));
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
    uint32_t* cur_pd = pd_from_phys(g_current_cr3);
    uint32_t* pt = get_pt_from_pd(cur_pd, pd);
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

    uint32_t* pd = pd_from_phys(g_current_cr3);
    uint32_t* pt = ensure_pt_in_pd(pd, pd_idx, PTE_W | PTE_U);
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
    uint32_t* cur_pd = pd_from_phys(g_current_cr3);
    uint32_t* pt = get_pt_from_pd(cur_pd, pd);
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

uint32_t paging_current_cr3(void) {
    if (g_current_cr3 == 0) g_current_cr3 = read_cr3();
    return g_current_cr3;
}

void paging_switch_mm(uint32_t cr3_phys) {
    if (!cr3_phys) return;
    cr3_phys &= ~0xFFFu;
    if (cr3_phys == g_current_cr3) return;
    g_current_cr3 = cr3_phys;
    load_cr3(g_current_cr3);
}

int paging_create_mm(uint32_t* out_cr3_phys) {
    if (!out_cr3_phys) return -1;
    if (!g_kernel_cr3) return -2;

    uintptr_t pd_pa = alloc_zeroed_frame();
    if (!pd_pa) return -3;

    uint32_t* pd = (uint32_t*)(uintptr_t)pd_pa;
    clone_kernel_mappings(pd);

    if (mm_alloc_slot((uint32_t)pd_pa, 1) < 0) {
        phys_free_frame(pd_pa);
        return -4;
    }

    *out_cr3_phys = (uint32_t)pd_pa;
    return 0;
}

void paging_retain_mm(uint32_t cr3_phys) {
    if (!cr3_phys) return;
    cr3_phys &= ~0xFFFu;
    int slot = mm_find_slot(cr3_phys);
    if (slot >= 0) {
        g_mm_slots[slot].refs++;
        return;
    }
    (void)mm_alloc_slot(cr3_phys, 1);
}

void paging_release_mm(uint32_t cr3_phys) {
    if (!cr3_phys) return;
    cr3_phys &= ~0xFFFu;
    int slot = mm_find_slot(cr3_phys);
    if (slot < 0) return;

    if (g_mm_slots[slot].refs > 1) {
        g_mm_slots[slot].refs--;
        return;
    }

    g_mm_slots[slot].used = 0;
    g_mm_slots[slot].refs = 0;
    g_mm_slots[slot].cr3 = 0;

    if (cr3_phys == g_kernel_cr3) return;
    destroy_mm(cr3_phys);
}

uint32_t paging_mm_refcount(uint32_t cr3_phys) {
    if (!cr3_phys) return 0;
    cr3_phys &= ~0xFFFu;
    int slot = mm_find_slot(cr3_phys);
    if (slot < 0) return 0;
    return g_mm_slots[slot].refs;
}

int paging_fork_current_cow(uint32_t* out_child_cr3_phys) {
    if (!out_child_cr3_phys) return -1;
    uint32_t parent_cr3 = paging_current_cr3() & ~0xFFFu;
    uint32_t* parent_pd = pd_from_phys(parent_cr3);
    if (!parent_pd) return -2;

    uint32_t child_cr3 = 0;
    int mk = paging_create_mm(&child_cr3);
    if (mk < 0) return mk;
    uint32_t* child_pd = pd_from_phys(child_cr3);

    for (uint32_t i = 0; i < 1024; ++i) {
        uint32_t pde = parent_pd[i];
        if ((pde & PTE_P) == 0u) continue;
        if ((pde & PTE_U) == 0u) continue;

        uintptr_t child_pt_pa = alloc_zeroed_frame();
        if (!child_pt_pa) {
            paging_release_mm(child_cr3);
            return -3;
        }

        uint32_t* parent_pt = (uint32_t*)(uintptr_t)(pde & ~0xFFFu);
        uint32_t* child_pt = (uint32_t*)(uintptr_t)child_pt_pa;
        child_pd[i] = ((uint32_t)child_pt_pa & ~0xFFFu) | (pde & 0xFFFu);

        for (uint32_t j = 0; j < 1024; ++j) {
            uint32_t pte = parent_pt[j];
            if ((pte & PTE_P) == 0u) continue;
            if ((pte & PTE_U) == 0u) continue;

            if (pte & (PTE_W | PTE_COW)) {
                pte = (pte | PTE_COW) & ~PTE_W;
                parent_pt[j] = pte;
            }

            child_pt[j] = pte;
            pmm_frame_ref((uintptr_t)(pte & ~0xFFFu));
        }
    }

    /* Parent PTE write-bit changes must be visible immediately. */
    load_cr3(parent_cr3);
    g_current_cr3 = parent_cr3;

    *out_child_cr3_phys = child_cr3;
    return 0;
}

void paging_initialize(struct multiboot_tag_framebuffer* fb_tag) {
    memset(g_mm_slots, 0, sizeof(g_mm_slots));
    memset(g_demand_regions, 0, sizeof(g_demand_regions));
    g_demand_region_count = 0;
    g_demand_fault_count = 0;
    g_cow_fault_count = 0;

    uintptr_t pd_pa = alloc_zeroed_frame();
    if (!pd_pa) {
        terminal_writestring("paging init failed: no frame for page directory\n");
        while (1) { __asm__ __volatile__("hlt"); }
    }
    uint32_t* pd = (uint32_t*)(uintptr_t)pd_pa;

    // Leave page 0 unmapped to catch NULL derefs; identity-map tracked RAM so
    // kernel can directly access page tables/frames via physical addresses.
    uintptr_t phys_span = (uintptr_t)pmm_total_frames() * PAGE_SIZE;
    if (phys_span < 0x1000u) phys_span = 0x1000u;
    identity_map_range(pd, 0x1000, phys_span - 0x1000u, PTE_P | PTE_W);

    // Identity map linear framebuffer if present
    if (fb_tag) {
        uintptr_t fb_addr = (uintptr_t)fb_tag->framebuffer_addr;
        uintptr_t fb_size = (uintptr_t)fb_tag->framebuffer_pitch * fb_tag->framebuffer_height;
        identity_map_range(pd, fb_addr, fb_size, PTE_P | PTE_W);
    }

    // Map a kernel heap region at KHEAP_BASE as supervisor-only.
    map_region_new_frames(pd, KHEAP_BASE, KHEAP_SIZE, PTE_P | PTE_W);

    g_kernel_cr3 = (uint32_t)(pd_pa & ~0xFFFu);
    g_current_cr3 = g_kernel_cr3;
    (void)mm_alloc_slot(g_kernel_cr3, 1);

    register_interrupt_handler(14, page_fault_handler);
    load_cr3(g_kernel_cr3);
    enable_paging();
}