#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H 1

#include <kernel/syscall.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROT_READ   MMAP_PROT_READ
#define PROT_WRITE  MMAP_PROT_WRITE
#define PROT_EXEC   MMAP_PROT_EXEC

#define MAP_SHARED    MMAP_MAP_SHARED
#define MAP_PRIVATE   MMAP_MAP_PRIVATE
#define MAP_FIXED     MMAP_MAP_FIXED
#define MAP_ANONYMOUS MMAP_MAP_ANONYMOUS
#define MAP_SHM       MMAP_MAP_SHM

#define MAP_FAILED ((void*)(-1))

void* mmap(void* addr, size_t length, int prot, int flags, int fd, unsigned offset);
int munmap(void* addr, size_t length);

#ifdef __cplusplus
}
#endif

#endif

