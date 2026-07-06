#ifndef _SYS_BLK_H
#define _SYS_BLK_H 1
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

struct blk_info {
    uint32_t id;
    char name[16];
    uint32_t block_size;
    uint64_t block_count;
    uint32_t flags;
};

struct blk_cache_stats {
    uint32_t entries;
    uint32_t hits;
    uint32_t misses;
    uint32_t evictions;
    uint32_t writebacks;
    uint32_t async_submitted;
    uint32_t async_completed;
    uint32_t async_failed;
    uint32_t async_pending;
};

#define BLOCKDEV_FLAG_READONLY  0x1u
#define BLOCKDEV_FLAG_ATAPI     0x2u
#define BLOCKDEV_FLAG_PARTITION 0x4u

int blk_info(unsigned index, struct blk_info* out);
int blk_read(unsigned dev, unsigned lba, void* buf);
int blk_write(unsigned dev, unsigned lba, const void* buf);
int blk_cache_stats(struct blk_cache_stats* out);
int blk_flush(int dev);

#ifdef __cplusplus
}
#endif
#endif /* _SYS_BLK_H */
