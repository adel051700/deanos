#include "include/kernel/blockdev.h"
#include "include/kernel/mbr.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define BLOCKDEV_MAX 16
#define BLOCK_CACHE_ENTRIES 128u
#define BLOCK_CACHE_BLOCK_SIZE 512u
#define BLOCKDEV_ASYNC_QUEUE_DEPTH 64u

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

typedef struct {
    blockdev_request_t req;
    int status;
} blockdev_completion_entry_t;

static blockdev_request_t g_pending[BLOCKDEV_ASYNC_QUEUE_DEPTH];
static uint32_t g_pending_head = 0;
static uint32_t g_pending_tail = 0;
static uint32_t g_pending_count = 0;

static blockdev_completion_entry_t g_completed[BLOCKDEV_ASYNC_QUEUE_DEPTH];
static uint32_t g_completed_head = 0;
static uint32_t g_completed_tail = 0;
static uint32_t g_completed_count = 0;
static uint32_t g_async_inflight = 0;

static int blockdev_should_cache(const block_device_t* d) {
    if (!d || !d->read) return 0;
    if (d->flags & BLOCKDEV_FLAG_PARTITION) return 0;
    return d->block_size == BLOCK_CACHE_BLOCK_SIZE;
}

static void blockdev_update_async_pending_stat(void) {
    g_cache_stats.async_pending = g_pending_count + g_completed_count + g_async_inflight;
}

static int blockdev_pending_push(const blockdev_request_t* req) {
    if (!req) return -1;
    if (g_pending_count >= BLOCKDEV_ASYNC_QUEUE_DEPTH) return -1;
    g_pending[g_pending_tail] = *req;
    g_pending_tail = (g_pending_tail + 1u) % BLOCKDEV_ASYNC_QUEUE_DEPTH;
    g_pending_count++;
    blockdev_update_async_pending_stat();
    return 0;
}

static int blockdev_pending_pop(blockdev_request_t* out) {
    if (!out) return -1;
    if (g_pending_count == 0) return -1;
    *out = g_pending[g_pending_head];
    g_pending_head = (g_pending_head + 1u) % BLOCKDEV_ASYNC_QUEUE_DEPTH;
    g_pending_count--;
    blockdev_update_async_pending_stat();
    return 0;
}

static int blockdev_completed_push(const blockdev_request_t* req, int status) {
    if (!req) return -1;
    if (g_completed_count >= BLOCKDEV_ASYNC_QUEUE_DEPTH) return -1;
    g_completed[g_completed_tail].req = *req;
    g_completed[g_completed_tail].status = status;
    g_completed_tail = (g_completed_tail + 1u) % BLOCKDEV_ASYNC_QUEUE_DEPTH;
    g_completed_count++;
    blockdev_update_async_pending_stat();
    return 0;
}

static int blockdev_completed_pop(blockdev_completion_entry_t* out) {
    if (!out) return -1;
    if (g_completed_count == 0) return -1;
    *out = g_completed[g_completed_head];
    g_completed_head = (g_completed_head + 1u) % BLOCKDEV_ASYNC_QUEUE_DEPTH;
    g_completed_count--;
    blockdev_update_async_pending_stat();
    return 0;
}

static int blockdev_validate_request(const blockdev_request_t* req) {
    if (!req) return -1;
    if (req->dev_index >= g_count) return -2;

    const block_device_t* d = &g_devs[req->dev_index];

    if (req->op == BLOCKDEV_REQ_FLUSH) {
        return 0;
    }

    if (req->count == 0 || !req->buffer) return -3;
    if (req->lba >= d->block_count) return -4;
    if ((d->block_count - req->lba) < (uint64_t)req->count) return -5;

    if (req->op == BLOCKDEV_REQ_READ) {
        if (!d->read) return -6;
        return 0;
    }

    if (req->op == BLOCKDEV_REQ_WRITE) {
        if (!d->write) return -6;
        if (d->flags & BLOCKDEV_FLAG_READONLY) return -7;
        return 0;
    }

    return -8;
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

static int blockdev_read_sync_impl(uint32_t index, uint64_t lba, uint32_t count, void* buffer) {
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

static int blockdev_write_sync_impl(uint32_t index, uint64_t lba, uint32_t count, const void* buffer) {
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

static int blockdev_flush_sync_impl(uint32_t index) {
    if (index >= g_count) return -1;

    uint32_t target_a = index;
    uint32_t target_b = UINT32_MAX;
    const block_device_t* d = &g_devs[index];
    if (d->flags & BLOCKDEV_FLAG_PARTITION) {
        uint32_t parent = 0;
        if (mbr_partition_parent_index(index, &parent) == 0) {
            target_b = parent;
        }
    }

    for (uint32_t i = 0; i < BLOCK_CACHE_ENTRIES; ++i) {
        if (!g_cache[i].valid || !g_cache[i].dirty) continue;
        if (g_cache[i].dev_index != target_a && g_cache[i].dev_index != target_b) continue;
        int rc = block_cache_writeback_entry(i);
        if (rc < 0) return rc;
    }

    return 0;
}

static int blockdev_execute_request(const blockdev_request_t* req) {
    if (!req) return -1;
    if (req->op == BLOCKDEV_REQ_READ) {
        return blockdev_read_sync_impl(req->dev_index, req->lba, req->count, req->buffer);
    }
    if (req->op == BLOCKDEV_REQ_WRITE) {
        return blockdev_write_sync_impl(req->dev_index, req->lba, req->count, req->buffer);
    }
    if (req->op == BLOCKDEV_REQ_FLUSH) {
        return blockdev_flush_sync_impl(req->dev_index);
    }
    return -1;
}

static void blockdev_dispatch_completions(uint32_t budget) {
    if (budget == 0) budget = 1;

    for (uint32_t dispatched = 0; dispatched < budget; ++dispatched) {
        blockdev_completion_entry_t event;
        if (blockdev_completed_pop(&event) < 0) break;

        if (event.req.completion) {
            event.req.completion(&event.req, event.status, event.req.user_data);
        }
    }
}

static void blockdev_wait_callback(const blockdev_request_t* req, int status, void* user_data) {
    (void)req;
    if (!user_data) return;
    int* waiter = (int*)user_data;
    waiter[0] = status;
    waiter[1] = 1;
}

static int blockdev_submit_and_wait(blockdev_request_t* req) {
    if (!req) return -1;
    int waiter[2] = {0, 0};
    req->completion = blockdev_wait_callback;
    req->user_data = waiter;

    int src = blockdev_submit_async(req);
    if (src < 0) return src;

    while (!waiter[1]) {
        blockdev_pump(1);
    }
    return waiter[0];
}

static void blockdev_drain_async(void) {
    while (g_pending_count > 0 || g_completed_count > 0 || g_async_inflight > 0) {
        blockdev_pump(8);
    }
}

void blockdev_initialize(void) {
    g_count = 0;
    memset(g_cache, 0, sizeof(g_cache));
    g_cache_hand = 0;
    memset(&g_cache_stats, 0, sizeof(g_cache_stats));
    g_cache_stats.entries = BLOCK_CACHE_ENTRIES;

    g_pending_head = 0;
    g_pending_tail = 0;
    g_pending_count = 0;
    g_completed_head = 0;
    g_completed_tail = 0;
    g_completed_count = 0;
    g_async_inflight = 0;
    blockdev_update_async_pending_stat();
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
    blockdev_request_t req;
    memset(&req, 0, sizeof(req));
    req.op = BLOCKDEV_REQ_READ;
    req.dev_index = index;
    req.lba = lba;
    req.count = count;
    req.buffer = buffer;
    return blockdev_submit_and_wait(&req);
}

int blockdev_write(uint32_t index, uint64_t lba, uint32_t count, const void* buffer) {
    blockdev_request_t req;
    memset(&req, 0, sizeof(req));
    req.op = BLOCKDEV_REQ_WRITE;
    req.dev_index = index;
    req.lba = lba;
    req.count = count;
    req.buffer = (void*)buffer;
    return blockdev_submit_and_wait(&req);
}

int blockdev_submit_async(const blockdev_request_t* req) {
    int vrc = blockdev_validate_request(req);
    if (vrc < 0) return vrc;

    if (blockdev_pending_push(req) < 0) return -9;
    g_cache_stats.async_submitted++;
    return 0;
}

void blockdev_pump(uint32_t budget) {
    if (budget == 0) budget = 1;

    blockdev_dispatch_completions(budget);

    for (uint32_t i = 0; i < budget; ++i) {
        blockdev_request_t req;
        if (blockdev_pending_pop(&req) < 0) break;

        g_async_inflight = 1;
        blockdev_update_async_pending_stat();
        int status = blockdev_execute_request(&req);
        g_async_inflight = 0;
        blockdev_update_async_pending_stat();

        if (status < 0) g_cache_stats.async_failed++;
        g_cache_stats.async_completed++;

        if (blockdev_completed_push(&req, status) < 0) {
            if (req.completion) {
                req.completion(&req, status, req.user_data);
            }
        }

        blockdev_dispatch_completions(1);
    }
}

int blockdev_flush(uint32_t index) {
    blockdev_request_t req;
    memset(&req, 0, sizeof(req));
    req.op = BLOCKDEV_REQ_FLUSH;
    req.dev_index = index;
    return blockdev_submit_and_wait(&req);
}

int blockdev_flush_all(void) {
    blockdev_drain_async();

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
