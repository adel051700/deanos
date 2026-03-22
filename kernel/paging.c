#include "include/kernel/paging.h"
#include "include/kernel/pmm.h"
#include "../libc/include/string.h"
#include "include/kernel/interrupt.h"
#include "include/kernel/task.h"
#include "include/kernel/tty.h"
#include "include/kernel/vfs.h"
#include "include/kernel/blockdev.h"
#include "include/kernel/mbr.h"
#include "include/kernel/kheap.h"
#include "../libc/include/stdio.h"

#define PAGE_SIZE 4096

// Flags
#define PTE_P  0x001
#define PTE_W  0x002
#define PTE_U  0x004
#define PTE_A  0x020
#define PTE_COW 0x200
#define PTE_SWAPPED 0x400

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

typedef struct swap_state {
    /* On-disk layout: one slot == one 4 KiB page (N contiguous device blocks). */
    uint8_t enabled;
    uint32_t dev_index;
    uint64_t base_lba;
    uint32_t blocks_per_page;
    uint32_t slot_count;
    uint32_t slots_used;
    uint32_t pageouts;
    uint32_t pageins;
    uint32_t faults;
    uint32_t hand_mm;
    uint32_t hand_page;
    uint16_t* slot_refs;
} swap_state_t;

typedef struct swap_victim {
    uint32_t* pt;
    uint32_t pt_index;
    uint32_t cr3;
    uintptr_t vaddr;
    uintptr_t phys;
    uint32_t pte;
} swap_victim_t;

static swap_state_t g_swap;

static inline int pte_is_swapped(uint32_t pte) {
    return ((pte & PTE_P) == 0u) && ((pte & PTE_SWAPPED) != 0u);
}

static inline uint32_t pte_swap_slot(uint32_t pte) {
    return pte >> 12;
}

static inline uint32_t pte_swap_flags(uint32_t pte) {
    return pte & (PTE_W | PTE_U | PTE_COW);
}

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

static int swap_alloc_slot(uint32_t* out_slot) {
    if (!out_slot || !g_swap.enabled || !g_swap.slot_refs) return -1;
    for (uint32_t i = 0; i < g_swap.slot_count; ++i) {
        if (g_swap.slot_refs[i] != 0) continue;
        g_swap.slot_refs[i] = 1;
        g_swap.slots_used++;
        *out_slot = i;
        return 0;
    }
    return -2;
}

static void swap_slot_ref(uint32_t slot) {
    if (!g_swap.enabled || !g_swap.slot_refs || slot >= g_swap.slot_count) return;
    if (g_swap.slot_refs[slot] == 0) {
        g_swap.slot_refs[slot] = 1;
        g_swap.slots_used++;
        return;
    }
    if (g_swap.slot_refs[slot] != 0xFFFFu) g_swap.slot_refs[slot]++;
}

static void swap_slot_unref(uint32_t slot) {
    if (!g_swap.enabled || !g_swap.slot_refs || slot >= g_swap.slot_count) return;
    if (g_swap.slot_refs[slot] == 0) return;
    if (g_swap.slot_refs[slot] > 1) {
        g_swap.slot_refs[slot]--;
        return;
    }
    g_swap.slot_refs[slot] = 0;
    if (g_swap.slots_used > 0) g_swap.slots_used--;
}

static uintptr_t alloc_frame_for_paging(void);

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

static int unmap_page_current(uintptr_t virt) {
    uint32_t* pd = pd_from_phys(g_current_cr3);
    if (!pd) return -1;

    uint32_t dir_idx = (virt >> 22) & 0x3FFu;
    uint32_t pt_idx = (virt >> 12) & 0x3FFu;
    uint32_t* pt = get_pt_from_pd(pd, dir_idx);
    if (!pt) return 0;

    uint32_t pte = pt[pt_idx];
    if ((pte & PTE_P) == 0u) {
        if (pte_is_swapped(pte)) {
            pt[pt_idx] = 0;
            invlpg((void*)(virt & ~0xFFFu));
            swap_slot_unref(pte_swap_slot(pte));
            return 1;
        }
        return 0;
    }
    if ((pte & PTE_U) == 0u) return 0;

    uintptr_t pa = (uintptr_t)(pte & ~0xFFFu);
    pt[pt_idx] = 0;
    invlpg((void*)(virt & ~0xFFFu));
    pmm_frame_unref(pa);
    return 1;
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
            if ((pte & PTE_P) != 0u) {
                uintptr_t pa = (uintptr_t)(pte & ~0xFFFu);
                pmm_frame_unref(pa);
            } else if (pte_is_swapped(pte)) {
                swap_slot_unref(pte_swap_slot(pte));
            }
        }

        phys_free_frame((uintptr_t)(pde & ~0xFFFu));
    }

    phys_free_frame((uintptr_t)(cr3_phys & ~0xFFFu));
}

static int find_swap_victim(int second_chance, swap_victim_t* out) {
    /* Eviction policy: global second-chance clock over user-present PTEs. */
    if (!out) return -1;

    const uint32_t pages_per_pd = 1024u * 1024u;
    uint32_t start_mm = g_swap.hand_mm;
    uint32_t start_page = g_swap.hand_page;

    for (uint32_t mm_step = 0; mm_step < MM_SLOT_MAX; ++mm_step) {
        uint32_t mm_index = (start_mm + mm_step) % MM_SLOT_MAX;
        mm_slot_t* slot = &g_mm_slots[mm_index];
        if (!slot->used) continue;
        if ((slot->cr3 & ~0xFFFu) == g_kernel_cr3) continue;

        uint32_t* pd = pd_from_phys(slot->cr3);
        if (!pd) continue;

        uint32_t page_start = (mm_step == 0) ? start_page : 0u;
        for (uint32_t page_step = 0; page_step < pages_per_pd; ++page_step) {
            uint32_t page_index = (page_start + page_step) % pages_per_pd;
            uint32_t dir = (page_index >> 10) & 0x3FFu;
            uint32_t ti = page_index & 0x3FFu;
            uint32_t pde = pd[dir];
            if ((pde & PTE_P) == 0u) continue;
            if ((pde & PTE_U) == 0u) continue;

            uint32_t* pt = (uint32_t*)(uintptr_t)(pde & ~0xFFFu);
            uint32_t pte = pt[ti];
            if ((pte & PTE_P) == 0u) continue;
            if ((pte & PTE_U) == 0u) continue;
            if (pte & PTE_COW) continue;

            uintptr_t pa = (uintptr_t)(pte & ~0xFFFu);
            if (pmm_frame_refcount(pa) != 1u) continue;

            uintptr_t va = ((uintptr_t)dir << 22) | ((uintptr_t)ti << 12);
            if (second_chance && (pte & PTE_A)) {
                pt[ti] = pte & ~PTE_A;
                if ((slot->cr3 & ~0xFFFu) == g_current_cr3) invlpg((void*)va);
                continue;
            }

            out->pt = pt;
            out->pt_index = ti;
            out->cr3 = slot->cr3 & ~0xFFFu;
            out->vaddr = va;
            out->phys = pa;
            out->pte = pte;

            g_swap.hand_mm = mm_index;
            g_swap.hand_page = (page_index + 1u) % pages_per_pd;
            return 0;
        }
    }

    return -2;
}

static int swap_page_out_one(void) {
    if (!g_swap.enabled) return -1;

    swap_victim_t victim;
    if (find_swap_victim(1, &victim) < 0 && find_swap_victim(0, &victim) < 0) {
        return -2;
    }

    uint32_t slot = 0;
    if (swap_alloc_slot(&slot) < 0) return -3;

    uint64_t lba = g_swap.base_lba + ((uint64_t)slot * (uint64_t)g_swap.blocks_per_page);
    if (blockdev_write(g_swap.dev_index, lba, g_swap.blocks_per_page, (const void*)victim.phys) < 0) {
        swap_slot_unref(slot);
        return -4;
    }

    victim.pt[victim.pt_index] = (slot << 12) | PTE_SWAPPED | pte_swap_flags(victim.pte);
    if (victim.cr3 == g_current_cr3) invlpg((void*)victim.vaddr);
    pmm_frame_unref(victim.phys);
    g_swap.pageouts++;
    return 0;
}

static uintptr_t alloc_frame_for_paging(void) {
    uintptr_t pa = phys_alloc_frame();
    if (pa) return pa;
    if (!g_swap.enabled) return 0;
    if (swap_page_out_one() < 0) return 0;
    pa = phys_alloc_frame();
    return pa;
}

static int handle_swap_fault(uintptr_t fault_addr, uint32_t err_code) {
    if ((err_code & 0x1u) != 0u) return 0;
    if (!g_swap.enabled) return 0;

    uintptr_t page = fault_addr & ~0xFFFu;
    uint32_t pd = (page >> 22) & 0x3FFu;
    uint32_t ti = (page >> 12) & 0x3FFu;
    uint32_t* cur_pd = pd_from_phys(g_current_cr3);
    uint32_t* pt = get_pt_from_pd(cur_pd, pd);
    if (!pt) return 0;

    uint32_t pte = pt[ti];
    if (!pte_is_swapped(pte)) return 0;
    uint32_t slot = pte_swap_slot(pte);
    if (slot >= g_swap.slot_count) return -1;

    uintptr_t pa = alloc_frame_for_paging();
    if (!pa) return -1;

    if (map_page(page, pa, PTE_P | PTE_W | PTE_U) < 0) {
        phys_free_frame(pa);
        return -1;
    }

    uint64_t lba = g_swap.base_lba + ((uint64_t)slot * (uint64_t)g_swap.blocks_per_page);
    if (blockdev_read(g_swap.dev_index, lba, g_swap.blocks_per_page, (void*)page) < 0) {
        unmap_page_current(page);
        return -1;
    }

    pt[ti] = (uint32_t)(pa & ~0xFFFu) | PTE_P | pte_swap_flags(pte);
    invlpg((void*)page);
    swap_slot_unref(slot);
    g_swap.pageins++;
    g_swap.faults++;
    return 1;
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
    uintptr_t pa = alloc_frame_for_paging();
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

static int handle_elf_lazy_fault(uintptr_t fault_addr, uint32_t err_code) {
    if (err_code & 0x1u) return 0; /* present/protection fault */

    task_t* cur = task_current();
    if (!cur || !cur->elf_backing || cur->elf_backing_size == 0) return 0;

    uintptr_t page = fault_addr & ~0xFFFu;
    int matched = 0;
    int writable = 0;
    for (uint32_t i = 0; i < cur->elf_region_count && i < TASK_ELF_LAZY_MAX; ++i) {
        const task_elf_lazy_region_t* reg = &cur->elf_regions[i];
        if (!reg->in_use) continue;
        if (page < reg->start || page >= reg->end) continue;
        matched = 1;
        if (reg->flags & PAGING_FLAG_WRITE) writable = 1;
    }
    if (!matched) return 0;

    uintptr_t pa = alloc_frame_for_paging();
    if (!pa) return -1;

    if (map_page(page, pa, PTE_P | PTE_W | PTE_U) < 0) {
        phys_free_frame(pa);
        return -1;
    }

    memset((void*)page, 0, PAGE_SIZE);

    for (uint32_t i = 0; i < cur->elf_region_count && i < TASK_ELF_LAZY_MAX; ++i) {
        const task_elf_lazy_region_t* reg = &cur->elf_regions[i];
        if (!reg->in_use) continue;
        if (page < reg->start || page >= reg->end) continue;

        uintptr_t copy_start = page;
        if (copy_start < reg->file_start) copy_start = reg->file_start;
        uintptr_t copy_end = page + PAGE_SIZE;
        if (copy_end > reg->file_end) copy_end = reg->file_end;

        if (copy_end > copy_start) {
            uint32_t copy_len = (uint32_t)(copy_end - copy_start);
            uint32_t file_off = reg->file_offset + (uint32_t)(copy_start - reg->file_start);
            if (file_off + copy_len > cur->elf_backing_size) return -1;
            memcpy((void*)copy_start, cur->elf_backing + file_off, copy_len);
        }
    }

    if (!writable) {
        uint32_t pd = (page >> 22) & 0x3FFu;
        uint32_t ti = (page >> 12) & 0x3FFu;
        uint32_t* cur_pd = pd_from_phys(g_current_cr3);
        uint32_t* pt = get_pt_from_pd(cur_pd, pd);
        if (!pt) return -1;
        pt[ti] &= ~PTE_W;
        invlpg((void*)page);
    }

    g_demand_fault_count++;
    return 1;
}

static int handle_mmap_fault(uintptr_t fault_addr, uint32_t err_code) {
    if (err_code & 0x1u) return 0; /* protection faults are not lazy-map faults */

    task_t* cur = task_current();
    if (!cur) return 0;

    uintptr_t page = fault_addr & ~0xFFFu;
    const task_mmap_region_t* reg = NULL;
    for (uint32_t i = 0; i < TASK_MMAP_MAX; ++i) {
        const task_mmap_region_t* cand = &cur->mmap_regions[i];
        if (!cand->in_use) continue;
        if (page < cand->start || page >= cand->end) continue;
        reg = cand;
        break;
    }
    if (!reg) return 0;

    uintptr_t pa = 0;
    int shared_frame = 0;

    if (reg->backing == TASK_MMAP_BACKING_SHM) {
        uintptr_t page_off = page - reg->start;
        uint32_t shm_page = (reg->file_offset + (uint32_t)page_off) >> 12;
        if (task_shm_get_frame(reg->shm_id, shm_page, &pa, NULL) < 0) return -1;
        pmm_frame_ref(pa);
        shared_frame = 1;
    } else {
        pa = alloc_frame_for_paging();
        if (!pa) return -1;
    }

    uint32_t map_flags = PTE_P | PTE_U | PTE_W;
    if (map_page(page, pa, map_flags) < 0) {
        if (shared_frame) {
            pmm_frame_unref(pa);
        } else {
            phys_free_frame(pa);
        }
        return -1;
    }

    if (!shared_frame) {
        memset((void*)page, 0, PAGE_SIZE);
    }

    if (reg->backing == TASK_MMAP_BACKING_FILE && reg->file_node) {
        uint32_t page_off = (uint32_t)(page - reg->start);
        uint32_t file_off = reg->file_offset + page_off;
        uint32_t max_in_region = (uint32_t)(reg->end - page);
        uint32_t to_read = PAGE_SIZE;
        if (max_in_region < to_read) to_read = max_in_region;

        if (file_off < reg->file_node->size) {
            uint32_t file_avail = reg->file_node->size - file_off;
            if (file_avail < to_read) to_read = file_avail;
        } else {
            to_read = 0;
        }

        if (to_read > 0) {
            int32_t nread = vfs_read(reg->file_node, file_off, to_read, (uint8_t*)page);
            if (nread < 0) return -1;
        }
    }

    if ((reg->prot & MMAP_PROT_WRITE) == 0u) {
        uint32_t pd = (page >> 22) & 0x3FFu;
        uint32_t ti = (page >> 12) & 0x3FFu;
        uint32_t* cur_pd = pd_from_phys(g_current_cr3);
        uint32_t* pt = get_pt_from_pd(cur_pd, pd);
        if (!pt) return -1;
        pt[ti] &= ~PTE_W;
        invlpg((void*)page);
    }

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

    uintptr_t new_pa = alloc_frame_for_paging();
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
    if (handle_swap_fault(fault_addr, r->err_code) > 0) return;
    if (handle_elf_lazy_fault(fault_addr, r->err_code) > 0) return;
    if (handle_mmap_fault(fault_addr, r->err_code) > 0) return;
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

    uintptr_t pa = alloc_frame_for_paging();
    if (!pa) return -2;

    pt[pt_idx] = (pa & ~0xFFFu) | (PTE_P | PTE_W | PTE_U);
    invlpg((void*)(vaddr & ~0xFFFu));

    /* Zero the freshly mapped page. */
    memset((void*)(vaddr & ~0xFFFu), 0, PAGE_SIZE);
    return 0;
}

int paging_unmap_user_range(uintptr_t start, uintptr_t size) {
    if (size == 0) return -1;
    uintptr_t s = start & ~0xFFFu;
    uintptr_t e = (start + size + PAGE_SIZE - 1) & ~0xFFFu;
    if (e <= s) return -1;

    int changed = 0;
    for (uintptr_t va = s; va < e; va += PAGE_SIZE) {
        int rc = unmap_page_current(va);
        if (rc < 0) return rc;
        if (rc > 0) changed = 1;
    }
    return changed;
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
    out->swap_slots_total = g_swap.slot_count;
    out->swap_slots_used = g_swap.slots_used;
    out->swap_pageouts = g_swap.pageouts;
    out->swap_pageins = g_swap.pageins;
    out->swap_faults = g_swap.faults;
    out->swap_enabled = g_swap.enabled;
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
            if ((pte & PTE_P) == 0u) {
                if (pte_is_swapped(pte)) {
                    child_pt[j] = pte;
                    swap_slot_ref(pte_swap_slot(pte));
                }
                continue;
            }
            if ((pte & PTE_U) == 0u) continue;

            uintptr_t vaddr = ((uintptr_t)i << 22) | ((uintptr_t)j << 12);
            int shared = task_is_shared_page(vaddr);

            if (!shared && (pte & (PTE_W | PTE_COW))) {
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
    memset(&g_swap, 0, sizeof(g_swap));

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

int paging_swap_initialize(void) {
    memset(&g_swap, 0, sizeof(g_swap));

    uint32_t part_count = mbr_partition_count();
    const mbr_partition_info_t* part = NULL;
    for (uint32_t i = 0; i < part_count; ++i) {
        const mbr_partition_info_t* info = mbr_partition_get(i);
        if (!info) continue;
        if (info->partition_type != 0x82u) continue;
        part = info;
        break;
    }
    if (!part) return -1;

    const block_device_t* dev = blockdev_get(part->dev_index);
    if (!dev) return -2;
    if (dev->flags & BLOCKDEV_FLAG_READONLY) return -3;
    if (dev->block_size == 0 || (PAGE_SIZE % dev->block_size) != 0u) return -4;

    uint32_t blocks_per_page = PAGE_SIZE / dev->block_size;
    if (blocks_per_page == 0) return -5;

    uint64_t max_slots_u64 = dev->block_count / (uint64_t)blocks_per_page;
    if (max_slots_u64 == 0 || max_slots_u64 > 0xFFFFFFFFu) return -6;

    uint32_t slot_count = (uint32_t)max_slots_u64;
    uint16_t* slot_refs = (uint16_t*)kcalloc(slot_count, sizeof(uint16_t));
    if (!slot_refs) return -7;

    g_swap.enabled = 1;
    g_swap.dev_index = dev->id;
    g_swap.base_lba = 0;
    g_swap.blocks_per_page = blocks_per_page;
    g_swap.slot_count = slot_count;
    g_swap.slot_refs = slot_refs;
    g_swap.hand_mm = 0;
    g_swap.hand_page = 0;
    return 0;
}
