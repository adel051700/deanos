#include <kernel/kheap.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>

void* malloc(size_t size) {
    return kmalloc(size);
}

void free(void* ptr) {
    kfree(ptr);
}

void* calloc(size_t nmemb, size_t size) {
    return kcalloc(nmemb, size);
}

void* realloc(void* ptr, size_t size) {
    return krealloc(ptr, size);
}

void abort(void) {
    _exit(1);
}

void exit(int status) {
    _exit(status);
}
