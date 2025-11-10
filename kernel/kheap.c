#include "include/kernel/kheap.h"
#include "include/kernel/paging.h"
#include "include/kernel/idt.h"
#include "../libc/include/string.h"   // memset, memcpy
#include "include/kernel/tty.h"
#include "../libc/include/stdio.h"    // itoa

// Basic boundary-tag allocator with a singly free list.
// Each block: [Header][payload...][Footer]
// Header/Footer store total block size (incl. header+footer). LSB bit = allocated flag.
// Free blocks use payload to store {prev,next} pointers for the free list.

#define ALIGN_UP(x, a)   (((x) + ((a)-1)) & ~((a)-1))
#define ALIGN8(x)        ALIGN_UP((x), 8u)

typedef struct header {
    size_t size_and_flags; // total block size incl. header+footer. bit0 = allocated flag
} header_t;

typedef struct footer {
    size_t size_and_flags; // mirror of header
} footer_t;

typedef struct free_node {
    struct free_node* prev;
    struct free_node* next;
} free_node_t;

static inline size_t blk_size(const header_t* h) { return h->size_and_flags & ~(size_t)1; }
static inline int    blk_alloc(const header_t* h) { return (int)(h->size_and_flags & 1); }
static inline void   set_alloc(header_t* h)       { h->size_and_flags |= 1; }
static inline void   set_free(header_t* h)        { h->size_and_flags &= ~(size_t)1; }

static inline footer_t* blk_footer(header_t* h) {
    return (footer_t*)((uint8_t*)h + blk_size(h) - sizeof(footer_t));
}
static inline header_t* blk_next(header_t* h, uintptr_t heap_end) {
    header_t* n = (header_t*)((uint8_t*)h + blk_size(h));
    if ((uintptr_t)n >= heap_end) return NULL;
    return n;
}
static inline header_t* blk_prev(header_t* h, uintptr_t heap_base) {
    if ((uintptr_t)h <= heap_base) return NULL;
    footer_t* pf = (footer_t*)((uint8_t*)h - sizeof(footer_t));
    if ((uintptr_t)pf < heap_base) return NULL;
    size_t psz = pf->size_and_flags & ~(size_t)1;
    header_t* p = (header_t*)((uint8_t*)h - psz);
    if ((uintptr_t)p < heap_base) return NULL;
    return p;
}
static inline free_node_t* hdr_to_node(header_t* h) {
    return (free_node_t*)((uint8_t*)h + sizeof(header_t));
}
static inline header_t* node_to_hdr(free_node_t* n) {
    return (header_t*)((uint8_t*)n - sizeof(header_t));
}

static uintptr_t g_heap_base = 0;
static uintptr_t g_heap_end  = 0;
static free_node_t* g_free_head = NULL;
static int g_initialized = 0;

// Minimum block size must fit header + footer + free_node payload.
#define MIN_BLOCK_SIZE (ALIGN8(sizeof(header_t) + sizeof(footer_t) + sizeof(free_node_t)))

static void free_list_insert(header_t* h) {
    set_free(h);
    // Write footer mirror
    footer_t* f = blk_footer(h);
    f->size_and_flags = h->size_and_flags;

    free_node_t* node = hdr_to_node(h);
    node->prev = NULL;
    node->next = g_free_head;
    if (g_free_head) g_free_head->prev = node;
    g_free_head = node;
}

static void free_list_remove(header_t* h) {
    free_node_t* node = hdr_to_node(h);
    if (node->prev) node->prev->next = node->next;
    else            g_free_head = node->next;
    if (node->next) node->next->prev = node->prev;
    // not changing header flags here
}

static header_t* find_fit(size_t total) {
    free_node_t* cur = g_free_head;
    while (cur) {
        header_t* h = node_to_hdr(cur);
        if (!blk_alloc(h) && blk_size(h) >= total) {
            return h;
        }
        cur = cur->next;
    }
    return NULL;
}

static header_t* coalesce(header_t* h) {
    // Try merge with next
    header_t* n = blk_next(h, g_heap_end);
    if (n && !blk_alloc(n)) {
        // Remove n from free list, merge
        free_list_remove(n);
        size_t new_sz = blk_size(h) + blk_size(n);
        h->size_and_flags = (new_sz | 0); // keep free
        blk_footer(h)->size_and_flags = h->size_and_flags;
    }

    // Try merge with prev
    header_t* p = blk_prev(h, g_heap_base);
    if (p && !blk_alloc(p)) {
        // h is not in free list yet, but p is; just grow p
        free_list_remove(p);
        size_t new_sz = blk_size(p) + blk_size(h);
        p->size_and_flags = (new_sz | 0);
        blk_footer(p)->size_and_flags = p->size_and_flags;
        // Insert merged block
        free_list_insert(p);
        return p;
    }

    // Insert (or re-insert) this block if not already
    return h;
}

static header_t* coalesce_neighbors(header_t* h) {
    // Merge with next if free
    header_t* n = blk_next(h, g_heap_end);
    if (n && !blk_alloc(n)) {
        free_list_remove(n);                    // remove neighbor from free list
        size_t new_sz = blk_size(h) + blk_size(n);
        h->size_and_flags = new_sz & ~(size_t)1; // keep free flag cleared
        blk_footer(h)->size_and_flags = h->size_and_flags;
    }

    // Merge with previous if free
    header_t* p = blk_prev(h, g_heap_base);
    if (p && !blk_alloc(p)) {
        free_list_remove(p);                    // remove previous from free list
        size_t new_sz = blk_size(p) + blk_size(h);
        p->size_and_flags = new_sz & ~(size_t)1;
        blk_footer(p)->size_and_flags = p->size_and_flags;
        h = p;                                   // merged block starts at previous
    }

    return h;
}

void kheap_initialize(void) {
    if (g_initialized) return;

    // Get mapped region from paging
    g_heap_base = paging_heap_base();
    g_heap_end  = g_heap_base + paging_heap_size();

    // Create one big free block
    header_t* h = (header_t*)g_heap_base;
    size_t total = ALIGN8(g_heap_end - g_heap_base);
    h->size_and_flags = (total & ~(size_t)1);
    footer_t* f = blk_footer(h);
    f->size_and_flags = h->size_and_flags;

    g_free_head = NULL;
    free_list_insert(h);

    g_initialized = 1;
}

void* kmalloc(size_t size) {
    if (size == 0) return NULL;
    if (!g_initialized) kheap_initialize();

    // 8-byte align payload
    size_t need = ALIGN8(size);
    size_t total = need + sizeof(header_t) + sizeof(footer_t);
    if (total < MIN_BLOCK_SIZE) total = MIN_BLOCK_SIZE;

    interrupts_disable(); // simple critical section

    header_t* h = find_fit(total);
    if (!h) {
        interrupts_enable();
        return NULL; // out of heap memory
    }

    free_list_remove(h);

    size_t cur_sz = blk_size(h);
    size_t rem = (cur_sz > total) ? (cur_sz - total) : 0;

    if (rem >= MIN_BLOCK_SIZE) {
        // Split: allocated block at front, remainder as a free block
        header_t* alloc_h = h;
        alloc_h->size_and_flags = (total | 1);
        footer_t* alloc_f = blk_footer(alloc_h);
        alloc_f->size_and_flags = alloc_h->size_and_flags;

        header_t* rem_h = (header_t*)((uint8_t*)alloc_h + total);
        rem_h->size_and_flags = (rem & ~(size_t)1);
        footer_t* rem_f = blk_footer(rem_h);
        rem_f->size_and_flags = rem_h->size_and_flags;

        free_list_insert(rem_h);
        h = alloc_h;
    } else {
        // Use whole block
        h->size_and_flags |= 1; // mark allocated
        blk_footer(h)->size_and_flags = h->size_and_flags;
    }

    interrupts_enable();

    return (void*)((uint8_t*)h + sizeof(header_t));
}

void kfree(void* ptr) {
    if (!ptr) return;
    if (!g_initialized) return;

    header_t* h = (header_t*)((uint8_t*)ptr - sizeof(header_t));

    interrupts_disable();

    // Mark this block free and write its footer
    set_free(h);
    blk_footer(h)->size_and_flags = h->size_and_flags;

    // Merge with adjacent free blocks before (re)inserting
    h = coalesce_neighbors(h);

    // Insert the final merged block exactly once
    free_list_insert(h);

    interrupts_enable();
}

void* kcalloc(size_t nmemb, size_t size) {
    // very simple overflow guard
    if (nmemb && size > ((size_t)-1) / nmemb) return NULL;
    size_t total = nmemb * size;
    void* p = kmalloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void* krealloc(void* ptr, size_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) { kfree(ptr); return NULL; }

    header_t* h = (header_t*)((uint8_t*)ptr - sizeof(header_t));
    size_t old_payload = blk_size(h) - sizeof(header_t) - sizeof(footer_t);
    if (new_size <= old_payload) {
        // Shrink in place (optional split could be done; keep simple)
        return ptr;
    }

    // Grow: allocate new, copy, free old
    void* np = kmalloc(new_size);
    if (!np) return NULL;
    memcpy(np, ptr, old_payload);
    kfree(ptr);
    return np;
}

size_t kheap_free_bytes(void) {
    size_t sum = 0;
    free_node_t* cur = g_free_head;
    while (cur) {
        header_t* h = node_to_hdr(cur);
        sum += blk_size(h);
        cur = cur->next;
    }
    return sum;
}

int kheap_self_test(void) {
    if (!g_initialized) kheap_initialize();

    size_t before = kheap_free_bytes();

    // Allocate a few blocks
    void* a = kmalloc(32);
    void* b = kmalloc(64);
    void* c = kmalloc(1024);
    if (!a || !b || !c) return -1;

    // Touch memory
    memset(a, 0xAA, 32);
    memset(b, 0xBB, 64);
    memset(c, 0xCC, 1024);

    // Realloc grow/shrink
    c = krealloc(c, 2048);
    if (!c) return -2;
    c = krealloc(c, 256);
    if (!c) return -3;

    // Free in different order
    kfree(b);
    kfree(a);
    kfree(c);

    size_t after = kheap_free_bytes();
    // Allow small bookkeeping differences? Here we expect exact match.
    return (after == before) ? 0 : -4;
}