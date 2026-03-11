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

// Simple kernel heap mapping (virtual)
#define KHEAP_BASE 0x40000000u
#define KHEAP_SIZE (4u * 1024u * 1024u) // 4 MiB for now

// Page directory and a small pool of page tables in .bss (identity mapped)
static uint32_t page_directory[1024] __attribute__((aligned(4096)));
#define INIT_PT_COUNT 64
static uint32_t page_tables[INIT_PT_COUNT][1024] __attribute__((aligned(4096)));
static uint32_t* pde_virt_tables[1024];
static int pt_used = 0;

// Helpers
static inline void load_cr3(uint32_t phys) { __asm__ __volatile__("mov %0, %%cr3" : : "r"(phys) : "memory"); }
static inline void enable_paging(void) {
    uint32_t cr0;
    __asm__ __volatile__("mov %%cr0, %0":"=r"(cr0));
    cr0 |= 0x80000000u; // PG
    __asm__ __volatile__("mov %0, %%cr0"::"r"(cr0));
}
static inline void invlpg(void* addr) { __asm__ __volatile__("invlpg (%0)" :: "r"(addr) : "memory"); }

static uint32_t* alloc_page_table(void) {
    if (pt_used >= INIT_PT_COUNT) return NULL;
    uint32_t* pt = page_tables[pt_used++];
    memset(pt, 0, 4096);
    return pt;
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

static void page_fault_handler(struct registers* r) {
    uint32_t fault_addr;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(fault_addr));
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

void paging_initialize(struct multiboot_tag_framebuffer* fb_tag) {
    memset(page_directory, 0, sizeof(page_directory));
    memset(pde_virt_tables, 0, sizeof(pde_virt_tables));

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