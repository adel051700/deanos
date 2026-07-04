#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

/* Minimal userspace allocator: a bump allocator over mmap'd anonymous
 * arenas. free() is a deliberate no-op (documented limitation) — the
 * programs this runtime targets (short-lived coreutils-style /bin
 * commands) run once and exit, so per-allocation reclamation isn't
 * worth the complexity yet. realloc() still copies the correct old size
 * via a per-allocation header, so it's safe even though free() doesn't
 * reclaim. */

#define USER_HEAP_CHUNK (64u * 1024u)

typedef struct {
    size_t size;
} alloc_header_t;

static uint8_t* heap_base = NULL;
static uint32_t heap_offset = 0;
static uint32_t heap_capacity = 0;

static int grow_heap(uint32_t min_extra) {
    uint32_t chunk = USER_HEAP_CHUNK;
    while (chunk < min_extra) chunk *= 2u;
    void* p = mmap(NULL, chunk, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0u);
    if (!p || p == MAP_FAILED) return -1;
    heap_base = (uint8_t*)p;
    heap_offset = 0;
    heap_capacity = chunk;
    return 0;
}

void* malloc(size_t size) {
    size_t total = size + sizeof(alloc_header_t);
    total = (total + 15u) & ~(size_t)15u; /* 16-byte align */
    if (!heap_base || (uint64_t)heap_offset + total > heap_capacity) {
        if (grow_heap((uint32_t)total) < 0) return NULL;
    }
    alloc_header_t* hdr = (alloc_header_t*)(heap_base + heap_offset);
    hdr->size = size;
    heap_offset += (uint32_t)total;
    return (void*)(hdr + 1);
}

void free(void* ptr) {
    (void)ptr;
}

void* calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void* p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void* realloc(void* ptr, size_t size) {
    if (!ptr) return malloc(size);
    alloc_header_t* old_hdr = (alloc_header_t*)ptr - 1;
    void* p = malloc(size);
    if (p) {
        size_t copy_len = old_hdr->size < size ? old_hdr->size : size;
        memcpy(p, ptr, copy_len);
    }
    return p;
}

void abort(void) {
    _exit(1);
}

void exit(int status) {
    _exit(status);
}
