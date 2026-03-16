#include <stdint.h>
#include <stddef.h>
#include "include/kernel/pmm.h"
#include "../libc/include/string.h"  // memset

/* Symbols from linker */
extern char _kernel_start;
extern char _kernel_end;

/* Bitmap representation: 1 bit per frame. 1 = used, 0 = free */
static uint32_t* g_bitmap = NULL;
static uint16_t* g_refcounts = NULL;
static uint32_t  g_bitmap_bits = 0;      // number of frames tracked
static uint32_t  g_free_frames = 0;

#define BIT_SET(arr, i)   ((arr)[(i)>>5] |=  (1u << ((i)&31)))
#define BIT_CLR(arr, i)   ((arr)[(i)>>5] &= ~(1u << ((i)&31)))
#define BIT_TST(arr, i)   (((arr)[(i)>>5] >> ((i)&31)) & 1u)

#define ALIGN_UP(x,a)     (((x) + ((a)-1)) & ~((a)-1))
#define MIN(a,b)          ((a) < (b) ? (a) : (b))
#define MAX(a,b)          ((a) > (b) ? (a) : (b))

static inline uint32_t addr_to_frame(uint64_t addr) {
    return (uint32_t)(addr >> 12); // 4KiB frames
}

static inline uintptr_t frame_to_addr(uint32_t frame) {
    return ((uintptr_t)frame) << 12;
}

static inline int frame_valid(uint32_t frame) {
    return (frame < g_bitmap_bits) && (g_bitmap != NULL);
}

/* Reserve [start,end) in frame granularity (set bits to 1 if in range) */
static void bitmap_reserve_range(uintptr_t start, uintptr_t end) {
    if (end <= start) return;
    uint32_t first = addr_to_frame(ALIGN_UP(start, PMM_FRAME_SIZE));
    uint32_t last  = addr_to_frame(end); // end is exclusive
    if (last > g_bitmap_bits) last = g_bitmap_bits;

    for (uint32_t f = first; f < last; ++f) {
        if (!BIT_TST(g_bitmap, f)) {
            BIT_SET(g_bitmap, f);
            if (g_free_frames) g_free_frames--;
        }
    }
}

/* Free [start,end) frames (clear bits to 0) */
static void bitmap_free_range(uintptr_t start, uintptr_t end) {
    if (end <= start) return;
    uint32_t first = addr_to_frame(ALIGN_UP(start, PMM_FRAME_SIZE));
    uint32_t last  = addr_to_frame(end); // end is exclusive
    if (last > g_bitmap_bits) last = g_bitmap_bits;

    for (uint32_t f = first; f < last; ++f) {
        if (BIT_TST(g_bitmap, f)) {
            BIT_CLR(g_bitmap, f);
            if (g_refcounts) g_refcounts[f] = 0;
            g_free_frames++;
        }
    }
}

/* Try to find a run of 'count' zero bits with alignment in frames */
static int find_free_run(uint32_t count, uint32_t align_frames, uint32_t* out_first) {
    if (count == 0 || g_bitmap_bits == 0) return 0;
    if (align_frames == 0) align_frames = 1;
    uint32_t i = 0;
    while (i + count <= g_bitmap_bits) {
        if (align_frames > 1) {
            uint32_t aligned = (i + align_frames - 1) & ~(align_frames - 1);
            if (aligned != i) {
                i = aligned;
                if (i + count > g_bitmap_bits) break;
            }
        }
        if (BIT_TST(g_bitmap, i)) { i++; continue; }
        uint32_t j = 0;
        for (; j < count; ++j) {
            if (BIT_TST(g_bitmap, i + j)) break;
        }
        if (j == count) {
            *out_first = i;
            return 1;
        }
        i += j + 1;
    }
    return 0;
}

uint32_t pmm_total_frames(void) { return g_bitmap_bits; }
uint32_t pmm_free_frames(void)  { return g_free_frames; }

/* Early bump allocator: place bitmap right after kernel.
   We only need it during init to carve the bitmap. */
static uintptr_t boot_alloc_ptr = 0;
static uintptr_t g_reserved_end = 0;  // <- add

static void boot_alloc_init(void) {
    uintptr_t after_kernel = (uintptr_t)&_kernel_end;
    boot_alloc_ptr = ALIGN_UP(after_kernel, PMM_FRAME_SIZE);
}

static void* boot_alloc(uint32_t bytes, uint32_t align) {
    if (align < 4) align = 4;
    boot_alloc_ptr = ALIGN_UP(boot_alloc_ptr, align);
    void* p = (void*)boot_alloc_ptr;
    boot_alloc_ptr += ALIGN_UP(bytes, 4);
    return p;
}

void pmm_initialize(struct multiboot_tag_mmap* mmap_tag) {
    if (!mmap_tag) { return; }

    if (mmap_tag->entry_size < sizeof(struct multiboot_mmap_entry) ||
        mmap_tag->entry_size > 128) {
        return;
    }

    uint8_t* cur = (uint8_t*)mmap_tag + sizeof(*mmap_tag);
    uint8_t* end = (uint8_t*)mmap_tag + mmap_tag->size;

    // Replace first pass that finds highest_addr:
    uint64_t highest_addr = 0;
    while (cur + sizeof(struct multiboot_mmap_entry) <= end) {
        const struct multiboot_mmap_entry* e = (const struct multiboot_mmap_entry*)cur;
        if (e->type == MB_MMAP_TYPE_AVAILABLE && e->len) {
            uint64_t region_end = e->addr + e->len;
            if (region_end > highest_addr) highest_addr = region_end;
        }
        cur += mmap_tag->entry_size;
    }

    // After computing highest_addr:
    if (highest_addr == 0) {
        return;
    }
    if (highest_addr > 0x8200000ULL)   // ~130 MiB heuristic from your map
        highest_addr = 0x8200000ULL;

    g_bitmap_bits = (uint32_t)((highest_addr + (PMM_FRAME_SIZE - 1)) / PMM_FRAME_SIZE);

    boot_alloc_init();
    uint32_t bitmap_words = (g_bitmap_bits + 31) / 32;
    uint32_t bitmap_bytes = bitmap_words * sizeof(uint32_t);

    g_bitmap = (uint32_t*)boot_alloc(bitmap_bytes, PMM_FRAME_SIZE);
    if (!g_bitmap) { return; }

    g_refcounts = (uint16_t*)boot_alloc(g_bitmap_bits * sizeof(uint16_t), 4);
    if (!g_refcounts) { return; }

    memset(g_bitmap, 0xFF, bitmap_bytes);
    memset(g_refcounts, 0, g_bitmap_bits * sizeof(uint16_t));
    g_free_frames = 0;

    uintptr_t reserved_end = boot_alloc_ptr;
    g_reserved_end = reserved_end;     // <- remember end of boot-reserved region

    // Pass 2 free usable
    cur = (uint8_t*)mmap_tag + sizeof(*mmap_tag);
    end = (uint8_t*)mmap_tag + mmap_tag->size;

    while (cur + sizeof(struct multiboot_mmap_entry) <= end) {
        const struct multiboot_mmap_entry* e = (const struct multiboot_mmap_entry*)cur;
        if (e->type == MB_MMAP_TYPE_AVAILABLE && e->len) {
            uintptr_t s = (uintptr_t)e->addr;
            uintptr_t t = (uintptr_t)(e->addr + e->len);
            if (t > reserved_end) {
                uintptr_t fs = (s < reserved_end) ? reserved_end : s;
                if (fs < t) {
                    bitmap_free_range(fs, t);
                }
            }
        }
        cur += mmap_tag->entry_size;
    }

    // Reserve frame 0 if freed
    if (g_bitmap_bits && !BIT_TST(g_bitmap, 0)) {
        BIT_SET(g_bitmap, 0);
        if (g_free_frames) g_free_frames--;
    }

    /* Mark currently used frames with refcount=1, free frames stay at 0. */
    for (uint32_t i = 0; i < g_bitmap_bits; ++i) {
        if (BIT_TST(g_bitmap, i)) g_refcounts[i] = 1;
    }
}

uintptr_t phys_alloc_frame(void) {
    // Scan for first zero bit
    for (uint32_t i = 0; i < g_bitmap_bits; ++i) {
        if (!BIT_TST(g_bitmap, i)) {
            BIT_SET(g_bitmap, i);
            if (g_refcounts) g_refcounts[i] = 1;
            if (g_free_frames) g_free_frames--;
            return frame_to_addr(i);
        }
    }
    return 0;
}

void phys_free_frame(uintptr_t phys_addr) {
    uint32_t f = addr_to_frame(phys_addr);
    if (f >= g_bitmap_bits) return;
    if (BIT_TST(g_bitmap, f)) {
        BIT_CLR(g_bitmap, f);
        if (g_refcounts) g_refcounts[f] = 0;
        g_free_frames++;
    }
}

uintptr_t phys_alloc_contiguous(uint32_t count, uint32_t align_frames) {
    if (count == 0) return 0;
    if (align_frames == 0) align_frames = 1;
    // align_frames must be power-of-two
    if (align_frames & (align_frames - 1)) return 0;

    uint32_t first;
    if (!find_free_run(count, align_frames, &first)) return 0;

    for (uint32_t j = 0; j < count; ++j) {
        BIT_SET(g_bitmap, first + j);
        if (g_refcounts) g_refcounts[first + j] = 1;
    }
    if (g_free_frames >= count) g_free_frames -= count;
    return frame_to_addr(first);
}

int pmm_self_test(uint32_t frames_to_test) {
    if (!g_bitmap) return -10;
    if (frames_to_test == 0) return -1;
    if (frames_to_test > 512) frames_to_test = 512;

    uint32_t before = pmm_free_frames();
    uintptr_t list[512];
    uint32_t got = 0;

    for (; got < frames_to_test; ++got) {
        uintptr_t f = phys_alloc_frame();
        if (!f) break;
        list[got] = f;
        // uniqueness cheap check vs earlier ones
        for (uint32_t k = 0; k < got; ++k)
            if (list[k] == f) return -3;
    }
    if (got == 0) return -2;

    for (uint32_t i = 0; i < got; ++i) phys_free_frame(list[i]);

    if (pmm_free_frames() != before) return -4;
    return 0;
}

uintptr_t pmm_reserved_region_end(void) {
    return g_reserved_end;
}

void pmm_frame_ref(uintptr_t phys_addr) {
    uint32_t f = addr_to_frame(phys_addr);
    if (!frame_valid(f)) return;
    if (!BIT_TST(g_bitmap, f)) return;
    if (g_refcounts[f] != 0xFFFFu) g_refcounts[f]++;
}

void pmm_frame_unref(uintptr_t phys_addr) {
    uint32_t f = addr_to_frame(phys_addr);
    if (!frame_valid(f)) return;
    if (!BIT_TST(g_bitmap, f)) return;

    if (g_refcounts[f] > 1) {
        g_refcounts[f]--;
        return;
    }
    phys_free_frame(phys_addr);
}

uint16_t pmm_frame_refcount(uintptr_t phys_addr) {
    uint32_t f = addr_to_frame(phys_addr);
    if (!frame_valid(f) || !BIT_TST(g_bitmap, f)) return 0;
    return g_refcounts[f];
}
