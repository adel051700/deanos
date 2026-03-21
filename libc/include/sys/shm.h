#ifndef _SYS_SHM_H
#define _SYS_SHM_H 1

#include <kernel/syscall.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int shm_open(int key, uint32_t size, uint32_t flags);
int shm_unlink(int key);

#ifdef __cplusplus
}
#endif

#endif

