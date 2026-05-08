/*
 * random.c — simple entropy collector + /dev/random / /dev/urandom devices
 *
 * This is a small, pragmatic implementation suitable for a teaching OS.
 * It provides an entropy-mixing function that IRQ handlers (keyboard/mouse)
 * can call, and two device nodes under /dev that yield bytes.  /dev/random
 * and /dev/urandom behave identically here (non-blocking), but the
 * infrastructure is present to extend blocking semantics later.
 */

#include "include/kernel/random.h"
#include "include/kernel/vfs.h"
#include "include/kernel/kheap.h"
#include "include/kernel/rtc.h"
#include "include/kernel/pit.h"
#include "include/kernel/log.h"
#include "include/kernel/idt.h"
#include "include/kernel/io.h"

#include <string.h>
#include <stdio.h>

/* Small pool/state */
static uint8_t g_pool[64];
static uint32_t g_pool_pos = 0;
static uint64_t g_prng_state = 0xfeedbeefu;

/* ----------------- PRNG: splitmix64 ----------------- */
static inline uint64_t splitmix64(uint64_t* state) {
    uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

/* Read TSC - weak timing source */
static inline uint64_t read_tsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Mix 32-bit value into pool/state. Interrupts disabled by caller if needed. */
void random_add_entropy(uint32_t event) {
    /* Mix event with current timers */
    uint64_t tsc = read_tsc();
    uint32_t mix32 = (uint32_t)(tsc ^ (tsc >> 32));
    mix32 ^= (uint32_t)pit_get_ticks();
    mix32 ^= event;

    /* Simple mixing into byte pool */
    for (int i = 0; i < 4; ++i) {
        g_pool[g_pool_pos] ^= (uint8_t)((mix32 >> (i*8)) & 0xFFu);
        g_pool_pos = (g_pool_pos + 1) % sizeof(g_pool);
    }

    /* Reseed PRNG state occasionally from pool */
    uint64_t seed = 0;
    for (size_t i = 0; i < sizeof(g_pool); i += 8) {
        uint64_t w = 0;
        for (size_t j = 0; j < 8 && (i + j) < sizeof(g_pool); ++j) {
            w |= ((uint64_t)g_pool[i + j]) << (j * 8);
        }
        seed ^= splitmix64(&w);
    }
    g_prng_state ^= seed ^ (uint64_t)mix32;
}

/* Expose writing into the entropy pool (e.g., from userspace) */
static int32_t random_write(vfs_node_t* node, uint32_t offset, uint32_t size, const uint8_t* buffer) {
    (void)node; (void)offset;
    if (!buffer) return -1;
    for (uint32_t i = 0; i < size; ++i) {
        random_add_entropy((uint32_t)buffer[i]);
    }
    return (int32_t)size;
}

static int random_open(vfs_node_t* node, uint32_t flags) {
    (void)node; (void)flags;
    return 0;
}

static void random_close(vfs_node_t* node) {
    (void)node;
}

/* Read handler used by both /dev/random and /dev/urandom */
static int32_t random_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    (void)offset;
    if (!buffer) return -1;

    /* Non-blocking: produce bytes from splitmix64 PRNG seeded from pool/state */
    for (uint32_t i = 0; i < size; ++i) {
        uint64_t v = splitmix64(&g_prng_state);
        buffer[i] = (uint8_t)(v & 0xFFu);
    }
    (void)node;
    return (int32_t)size;
}

/* Helper: create device node at path and set vtable to our callbacks */
static void make_random_device(const char* path) {
    if (!path) return;

    /* Ensure /dev exists */
    vfs_create_path("/dev", VFS_DIRECTORY);

    /* Create the file node (ramfs will create a regular file) */
    if (vfs_create_path(path, VFS_FILE) < 0) {
        /* maybe already exists — continue */
    }

    vfs_node_t* n = vfs_namei(path);
    if (!n) {
        klog("random: failed to create");
        if (path) klog(path);
        return;
    }

    /* Override vtable */
    n->read = random_read;
    n->write = random_write;
    n->open = random_open;
    n->close = random_close;
    n->mode = VFS_MODE_FILE_DEFAULT;
}

void random_initialize(void) {
    memset(g_pool, 0, sizeof(g_pool));
    g_pool_pos = 0;

    /* Seed PRNG with various boot-time sources */
    rtc_time_t t = {0};
    rtc_read_time(&t);
    uint64_t seed = read_tsc();
    seed ^= ((uint64_t)pit_get_ticks() << 16);
    seed ^= ((uint64_t)t.second << 8) | (uint64_t)t.minute;
    g_prng_state ^= seed;

    /* Create device nodes */
    make_random_device("/dev/random");
    make_random_device("/dev/urandom");

    klog("random: /dev/random and /dev/urandom initialized");
}

/* Convenience: user-facing API to obtain bytes */
void random_get_bytes(uint8_t* out, uint32_t len) {
    if (!out) return;
    for (uint32_t i = 0; i < len; ++i) {
        uint64_t v = splitmix64(&g_prng_state);
        out[i] = (uint8_t)(v & 0xFFu);
    }
}

