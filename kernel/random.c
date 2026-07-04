/*
 * random.c — ChaCha20 CSPRNG with fast key erasure + interrupt-timing entropy.
 *
 * All RNG state lives here. random_add_entropy() may be called from IRQ
 * handlers, so every state mutation runs inside an irq_save()/irq_restore()
 * critical section (a save/restore pair, so it nests correctly when the caller
 * is already in an interrupt with IRQs disabled).
 */

#include "include/kernel/random.h"
#include "include/kernel/rtc.h"
#include "include/kernel/log.h"
#include <string.h>
#include <stdint.h>
#include <stddef.h>

/* ---- state ------------------------------------------------------------- */

static uint8_t  g_key[32];
static uint8_t  g_pool[32];
static uint32_t g_pool_pos;
static uint32_t g_bits;          /* saturating entropy estimate (bits) */
static uint32_t g_since_reseed;  /* bits mixed since last reseed */
static int      g_seeded;
static int      g_initialized;

#define RANDOM_RESEED_INTERVAL 64u   /* reseed after this many mixed bits */

/* ---- primitives -------------------------------------------------------- */

static inline uint64_t read_tsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint32_t irq_save(void) {
    uint32_t f;
    __asm__ __volatile__("pushf; pop %0; cli" : "=r"(f) : : "memory");
    return f;
}

static inline void irq_restore(uint32_t f) {
    __asm__ __volatile__("push %0; popf" : : "r"(f) : "memory", "cc");
}

static inline uint32_t load32le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void store32le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

#define ROTL32(v, n) (((v) << (n)) | ((v) >> (32 - (n))))
#define ROTL8(v, n)  ((uint8_t)(((v) << (n)) | ((uint8_t)(v) >> (8 - (n)))))

#define QR(a, b, c, d)              \
    (a += b, d ^= a, d = ROTL32(d, 16), \
     c += d, b ^= c, b = ROTL32(b, 12), \
     a += b, d ^= a, d = ROTL32(d, 8),  \
     c += d, b ^= c, b = ROTL32(b, 7))

/* Standard RFC-7539 ChaCha20 block, zero 96-bit nonce, 32-bit block counter. */
static void chacha20_block(const uint8_t key[32], uint32_t counter, uint8_t out[64]) {
    uint32_t s[16];
    s[0] = 0x61707865u; s[1] = 0x3320646eu; s[2] = 0x79622d32u; s[3] = 0x6b206574u;
    for (int i = 0; i < 8; i++) s[4 + i] = load32le(key + 4 * i);
    s[12] = counter;
    s[13] = 0u; s[14] = 0u; s[15] = 0u;

    uint32_t x[16];
    for (int i = 0; i < 16; i++) x[i] = s[i];
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8],  x[12]);
        QR(x[1], x[5], x[9],  x[13]);
        QR(x[2], x[6], x[10], x[14]);
        QR(x[3], x[7], x[11], x[15]);
        QR(x[0], x[5], x[10], x[15]);
        QR(x[1], x[6], x[11], x[12]);
        QR(x[2], x[7], x[8],  x[13]);
        QR(x[3], x[4], x[9],  x[14]);
    }
    for (int i = 0; i < 16; i++) store32le(out + 4 * i, x[i] + s[i]);
}

/* Derive a fresh key from the current key mixed with the entropy pool.
 * Caller must hold the IRQ lock. */
static void reseed_locked(void) {
    uint8_t tmpkey[32];
    for (int i = 0; i < 32; i++) tmpkey[i] = g_key[i] ^ g_pool[i];
    uint8_t ks[64];
    chacha20_block(tmpkey, 0u, ks);
    memcpy(g_key, ks, 32);
    g_since_reseed = 0;
}

/* ---- public API -------------------------------------------------------- */

void random_add_entropy(const void* data, uint32_t len, uint32_t est_bits) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t f = irq_save();

    uint64_t t = read_tsc();
    for (int i = 0; i < 8; i++) {
        g_pool_pos = (g_pool_pos + 1u) & 31u;
        uint8_t b = (uint8_t)t;
        g_pool[g_pool_pos] = (uint8_t)(ROTL8(g_pool[g_pool_pos] ^ b, 3) + b);
        t >>= 8;
    }
    for (uint32_t i = 0; p && i < len; i++) {
        g_pool_pos = (g_pool_pos + 1u) & 31u;
        g_pool[g_pool_pos] = (uint8_t)(ROTL8(g_pool[g_pool_pos] ^ p[i], 5) + p[i]);
    }

    if (g_bits < RANDOM_SEED_THRESHOLD_BITS) {
        g_bits += est_bits;
        if (g_bits >= RANDOM_SEED_THRESHOLD_BITS) {
            g_bits = RANDOM_SEED_THRESHOLD_BITS;
            g_seeded = 1;
        }
    }

    g_since_reseed += (est_bits ? est_bits : 1u);
    if (g_since_reseed >= RANDOM_RESEED_INTERVAL) reseed_locked();

    irq_restore(f);
}

void random_bytes(void* out, uint32_t n) {
    if (!out || n == 0u) return;
    uint8_t* dst = (uint8_t*)out;
    uint32_t f = irq_save();

    uint32_t counter = 0u;
    uint8_t ks[64];
    uint8_t newkey[32];

    /* First block: reserve first 32 bytes as the next key (fast erasure),
     * emit the remaining 32 as output. */
    chacha20_block(g_key, counter++, ks);
    memcpy(newkey, ks, 32);
    uint32_t take = (n < 32u) ? n : 32u;
    memcpy(dst, ks + 32, take);
    dst += take; n -= take;

    while (n > 0u) {
        chacha20_block(g_key, counter++, ks);
        take = (n < 64u) ? n : 64u;
        memcpy(dst, ks, take);
        dst += take; n -= take;
    }

    memcpy(g_key, newkey, 32);   /* forward secrecy */
    irq_restore(f);
}

int random_is_seeded(void) {
    return g_seeded;
}

/* Boot-time timing-jitter entropy harvest.
 *
 * With no hardware RNG and possibly no user input at boot, /dev/random would
 * otherwise never reach the seed threshold (keyboard/mouse credit 1 bit per
 * event; typing one command yields far less than 128 bits). On real hardware
 * -- and on a QEMU whose TSC tracks the host clock -- the low bits of the TSC
 * delta across a short variable-latency loop are unpredictable run-to-run and
 * sample-to-sample. We fold every delta into the pool, but only CREDIT a bit
 * when the delta's low bits actually change between samples: a perfectly
 * deterministic environment credits nothing and /dev/random stays honestly
 * unseeded, while a normal environment seeds within the first moments of boot.
 * This mirrors the jitter-entropy seeding real kernels use at boot. The
 * per-bit credit is a conservative heuristic, not a rigorous entropy proof. */
static void jitter_seed(void) {
    uint32_t prev = 0u;
    int have_prev = 0;
    /* Bound the loop so a variation-free environment cannot spin forever. */
    for (int i = 0; i < (int)(RANDOM_SEED_THRESHOLD_BITS * 8u) && !g_seeded; i++) {
        uint64_t a = read_tsc();
        volatile uint32_t spin = (uint32_t)(a & 0x0fu);
        for (volatile uint32_t j = 0; j <= spin; j++) { /* variable-latency filler */ }
        uint64_t b = read_tsc();
        uint32_t delta = (uint32_t)(b - a);
        uint32_t credit = (have_prev && (((delta ^ prev) & 0x3u) != 0u)) ? 1u : 0u;
        prev = delta;
        have_prev = 1;
        random_add_entropy(&delta, sizeof delta, credit);
    }
}

void random_initialize(void) {
    if (g_initialized) return;
    /* Nonzero default key so output is meaningful even before mixing. */
    for (int i = 0; i < 32; i++) g_key[i] = (uint8_t)(0x9eu * (uint32_t)(i + 1));
    g_initialized = 1;

    /* Boot seed: rdtsc + RTC wallclock + a couple of addresses. est_bits=0 so
     * this does NOT flip `seeded` (only real IRQ jitter does), but it makes
     * /dev/urandom non-deterministic from the first read. */
    uint64_t t = read_tsc();
    uint32_t wall = rtc_get_wallclock_seconds();
    uintptr_t addrs[2] = { (uintptr_t)&t, (uintptr_t)&random_initialize };
    random_add_entropy(&t, sizeof t, 0u);
    random_add_entropy(&wall, sizeof wall, 0u);
    random_add_entropy(addrs, sizeof addrs, 0u);

    /* Harvest timing jitter so the pool reaches the seed threshold at boot,
     * making /dev/random usable without requiring user input. */
    jitter_seed();
}
