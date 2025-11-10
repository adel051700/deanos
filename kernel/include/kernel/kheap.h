#ifndef _KERNEL_KHEAP_H
#define _KERNEL_KHEAP_H

#include <stddef.h>
#include <stdint.h>

/*
 A simple free-list allocator on a fixed, pre-mapped heap region.
 - kmalloc: allocate size bytes (8-byte aligned)
 - kfree: free a previously allocated block
 - kcalloc: allocate and zero-initialize
 - krealloc: resize a block
 - kheap_free_bytes: total bytes currently free in the heap (internal accounting, includes headers/footers)
 - kheap_self_test: basic allocator test, returns 0 on success
*/

void   kheap_initialize(void);
void*  kmalloc(size_t size);
void   kfree(void* ptr);
void*  kcalloc(size_t nmemb, size_t size);
void*  krealloc(void* ptr, size_t new_size);

size_t kheap_free_bytes(void);
int    kheap_self_test(void);

#endif