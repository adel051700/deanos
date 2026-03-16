#include "include/kernel/blockdev.h"

#include <stddef.h>
#include <string.h>

#define BLOCKDEV_MAX 16

static block_device_t g_devs[BLOCKDEV_MAX];
static uint32_t g_count = 0;

void blockdev_initialize(void) {
    g_count = 0;
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

    return d->read(d->ctx, lba, count, buffer);
}

int blockdev_write(uint32_t index, uint64_t lba, uint32_t count, const void* buffer) {
    if (index >= g_count || !buffer || count == 0) return -1;

    const block_device_t* d = &g_devs[index];
    if (!d->write) return -2;
    if (d->flags & BLOCKDEV_FLAG_READONLY) return -3;
    if (lba >= d->block_count) return -4;
    if ((d->block_count - lba) < (uint64_t)count) return -5;

    return d->write(d->ctx, lba, count, buffer);
}

