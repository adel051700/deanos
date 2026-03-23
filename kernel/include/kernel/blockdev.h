#ifndef _KERNEL_BLOCKDEV_H
#define _KERNEL_BLOCKDEV_H

#include <stdint.h>

typedef int (*blockdev_read_fn)(void* ctx, uint64_t lba, uint32_t count, void* buffer);
typedef int (*blockdev_write_fn)(void* ctx, uint64_t lba, uint32_t count, const void* buffer);

typedef enum {
    BLOCKDEV_REQ_READ = 1,
    BLOCKDEV_REQ_WRITE = 2,
    BLOCKDEV_REQ_FLUSH = 3
} blockdev_request_op_t;

struct blockdev_request;
typedef void (*blockdev_completion_cb)(const struct blockdev_request* req, int status, void* user_data);

typedef struct blockdev_request {
    uint8_t op;
    uint8_t _pad[3];
    uint32_t dev_index;
    uint64_t lba;
    uint32_t count;
    void* buffer;
    blockdev_completion_cb completion;
    void* user_data;
} blockdev_request_t;

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

typedef struct blockdev_cache_stats {
    uint32_t entries;
    uint32_t hits;
    uint32_t misses;
    uint32_t evictions;
    uint32_t writebacks;
    uint32_t async_submitted;
    uint32_t async_completed;
    uint32_t async_failed;
    uint32_t async_pending;
} blockdev_cache_stats_t;

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
int blockdev_submit_async(const blockdev_request_t* req);
void blockdev_pump(uint32_t budget);
int blockdev_flush(uint32_t index);
int blockdev_flush_all(void);
void blockdev_cache_stats(blockdev_cache_stats_t* out);

#endif

