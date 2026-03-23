#include "include/kernel/blockdev.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define BLOCKDEV_MAX 16
#define BLOCK_CACHE_ENTRIES 128u
#define BLOCK_CACHE_BLOCK_SIZE 512u

typedef struct {
    uint8_t valid;
    uint8_t dirty;
    uint8_t ref;
    uint8_t _pad;
    uint32_t dev_index;
    uint64_t lba;
    uint8_t data[BLOCK_CACHE_BLOCK_SIZE];
} block_cache_entry_t;

static block_device_t g_devs[BLOCKDEV_MAX];
static uint32_t g_count = 0;
static block_cache_entry_t g_cache[BLOCK_CACHE_ENTRIES];
static uint32_t g_cache_hand = 0;
static blockdev_cache_stats_t g_cache_stats = {0};

static int blockdev_should_cache(const block_device_t* d) {
    if (!d || !d->read) return 0;
    if (d->flags & BLOCKDEV_FLAG_PARTITION) return 0;
    return d->block_size == BLOCK_CACHE_BLOCK_SIZE;
}

static int block_cache_find(uint32_t dev_index, uint64_t lba) {
    for (uint32_t i = 0; i < BLOCK_CACHE_ENTRIES; ++i) {
        if (g_cache[i].valid && g_cache[i].dev_index == dev_index && g_cache[i].lba == lba) {
            return (int)i;
        }
    }
    return -1;
}

static int block_cache_writeback_entry(uint32_t slot) {
    block_cache_entry_t* e = &g_cache[slot];
    if (!e->valid || !e->dirty) return 0;
    if (e->dev_index >= g_count) return -1;

    const block_device_t* d = &g_devs[e->dev_index];
    if (!d->write) return -1;
    if (d->flags & BLOCKDEV_FLAG_READONLY) return -1;

    int rc = d->write(d->ctx, e->lba, 1, e->data);
    if (rc < 0) return rc;

    e->dirty = 0;
    g_cache_stats.writebacks++;
    return 0;
}

static int block_cache_get_slot(void) {
    for (uint32_t i = 0; i < BLOCK_CACHE_ENTRIES; ++i) {
        if (!g_cache[i].valid) return (int)i;
    }

    for (;;) {
        block_cache_entry_t* e = &g_cache[g_cache_hand];
        if (!e->ref) {
            int victim = (int)g_cache_hand;
            g_cache_hand = (g_cache_hand + 1u) % BLOCK_CACHE_ENTRIES;
            g_cache_stats.evictions++;
            return victim;
        }
        e->ref = 0;
        g_cache_hand = (g_cache_hand + 1u) % BLOCK_CACHE_ENTRIES;
    }
}

static int block_cache_load(uint32_t slot, uint32_t dev_index, uint64_t lba) {
    block_cache_entry_t* e = &g_cache[slot];
    const block_device_t* d = &g_devs[dev_index];
    int rc = d->read(d->ctx, lba, 1, e->data);
    if (rc < 0) return rc;

    e->valid = 1;
    e->dirty = 0;
    e->ref = 1;
    e->dev_index = dev_index;
    e->lba = lba;
    return 0;
}

void blockdev_initialize(void) {
    g_count = 0;
    memset(g_cache, 0, sizeof(g_cache));
    g_cache_hand = 0;
    memset(&g_cache_stats, 0, sizeof(g_cache_stats));
    g_cache_stats.entries = BLOCK_CACHE_ENTRIES;
}

int blockdev_register(const block_device_t* dev) {
    if (!dev || !dev->read) return -1;
    if (g_count >= BLOCKDEV_MAX) return -2;

    for (uint32_t i = 0; i < g_count; ++i) {
        if (strcmp(g_devs[i].name, dev->name) == 0) return -3;
    }

    g_devs[g_count] = *dev;
    g_devs[g_count].id = g_count;
    g_count++;
    return (int)(g_count - 1);
}

uint32_t blockdev_count(void) {
    return g_count;
}

const block_device_t* blockdev_get(uint32_t index) {
    if (index >= g_count) return NULL;
    return &g_devs[index];
}

const block_device_t* blockdev_find_by_name(const char* name) {
    if (!name || *name == '\0') return NULL;

    for (uint32_t i = 0; i < g_count; ++i) {
        if (strcmp(g_devs[i].name, name) == 0) return &g_devs[i];
    }

    return NULL;
}

int blockdev_read(uint32_t index, uint64_t lba, uint32_t count, void* buffer) {
    if (index >= g_count || !buffer || count == 0) return -1;

    const block_device_t* d = &g_devs[index];
    if (!d->read) return -2;
    if (lba >= d->block_count) return -3;
    if ((d->block_count - lba) < (uint64_t)count) return -4;

    if (!blockdev_should_cache(d)) {
        return d->read(d->ctx, lba, count, buffer);
    }

    uint8_t* dst = (uint8_t*)buffer;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t cur_lba = lba + i;
        int slot = block_cache_find(index, cur_lba);
        if (slot >= 0) {
            g_cache_stats.hits++;
            g_cache[slot].ref = 1;
            memcpy(dst + (i * BLOCK_CACHE_BLOCK_SIZE), g_cache[slot].data, BLOCK_CACHE_BLOCK_SIZE);
            continue;
        }

        g_cache_stats.misses++;
        slot = block_cache_get_slot();
        if (slot < 0) return -5;
        if (g_cache[slot].valid && g_cache[slot].dirty) {
            int wrc = block_cache_writeback_entry((uint32_t)slot);
            if (wrc < 0) return wrc;
        }

        int lrc = block_cache_load((uint32_t)slot, index, cur_lba);
        if (lrc < 0) return lrc;
        memcpy(dst + (i * BLOCK_CACHE_BLOCK_SIZE), g_cache[slot].data, BLOCK_CACHE_BLOCK_SIZE);
    }

    return 0;
}

int blockdev_write(uint32_t index, uint64_t lba, uint32_t count, const void* buffer) {
    if (index >= g_count || !buffer || count == 0) return -1;

    const block_device_t* d = &g_devs[index];
    if (!d->write) return -2;
    if (d->flags & BLOCKDEV_FLAG_READONLY) return -3;
    if (lba >= d->block_count) return -4;
    if ((d->block_count - lba) < (uint64_t)count) return -5;

    if (!blockdev_should_cache(d)) {
        return d->write(d->ctx, lba, count, buffer);
    }

    const uint8_t* src = (const uint8_t*)buffer;
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t cur_lba = lba + i;
        int slot = block_cache_find(index, cur_lba);
        if (slot >= 0) {
            g_cache_stats.hits++;
        } else {
            g_cache_stats.misses++;
            slot = block_cache_get_slot();
            if (slot < 0) return -6;
            if (g_cache[slot].valid && g_cache[slot].dirty) {
                int wrc = block_cache_writeback_entry((uint32_t)slot);
                if (wrc < 0) return wrc;
            }
            g_cache[slot].valid = 1;
            g_cache[slot].dev_index = index;
            g_cache[slot].lba = cur_lba;
        }

        memcpy(g_cache[slot].data, src + (i * BLOCK_CACHE_BLOCK_SIZE), BLOCK_CACHE_BLOCK_SIZE);
        g_cache[slot].dirty = 1;
        g_cache[slot].ref = 1;
    }

    return 0;
}

int blockdev_flush(uint32_t index) {
    if (index >= g_count) return -1;

    for (uint32_t i = 0; i < BLOCK_CACHE_ENTRIES; ++i) {
        if (!g_cache[i].valid || !g_cache[i].dirty) continue;
        if (g_cache[i].dev_index != index) continue;
        int rc = block_cache_writeback_entry(i);
        if (rc < 0) return rc;
    }

    return 0;
}

int blockdev_flush_all(void) {
    for (uint32_t i = 0; i < BLOCK_CACHE_ENTRIES; ++i) {
        if (!g_cache[i].valid || !g_cache[i].dirty) continue;
        int rc = block_cache_writeback_entry(i);
        if (rc < 0) return rc;
    }

    return 0;
}

void blockdev_cache_stats(blockdev_cache_stats_t* out) {
    if (!out) return;
    *out = g_cache_stats;
}
