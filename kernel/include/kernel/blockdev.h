#ifndef _KERNEL_BLOCKDEV_H
#define _KERNEL_BLOCKDEV_H

#include <stdint.h>

typedef int (*blockdev_read_fn)(void* ctx, uint64_t lba, uint32_t count, void* buffer);
typedef int (*blockdev_write_fn)(void* ctx, uint64_t lba, uint32_t count, const void* buffer);

typedef struct block_device {
    uint32_t id;
    char name[16];
    uint32_t block_size;
    uint64_t block_count;
    uint32_t flags;
    void* ctx;
    blockdev_read_fn read;
    blockdev_write_fn write;
} block_device_t;

#define BLOCKDEV_FLAG_READONLY 0x1u
#define BLOCKDEV_FLAG_ATAPI    0x2u
#define BLOCKDEV_FLAG_PARTITION 0x4u

void blockdev_initialize(void);
int blockdev_register(const block_device_t* dev);
uint32_t blockdev_count(void);
const block_device_t* blockdev_get(uint32_t index);
const block_device_t* blockdev_find_by_name(const char* name);
int blockdev_read(uint32_t index, uint64_t lba, uint32_t count, void* buffer);
int blockdev_write(uint32_t index, uint64_t lba, uint32_t count, const void* buffer);

#endif

