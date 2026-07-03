/*
 * rand.c — POSIX rand()/srand() over a xorshift64* generator.
 * Lazily seeded from getrandom() on first use unless srand() was called.
 */

#include <stdlib.h>
#include <unistd.h>
#include <stddef.h>

static unsigned long long g_state;
static int g_seeded;

void srand(unsigned seed) {
    g_state = seed ? (unsigned long long)seed : 1ull;
    g_seeded = 1;
}

int rand(void) {
    if (!g_seeded) {
        unsigned long long seed = 0;
        if (getrandom(&seed, sizeof seed, 0) != (long)sizeof seed || seed == 0)
            seed = 0x853c49e6748fea9bull;   /* fallback */
        g_state = seed;
        g_seeded = 1;
    }
    g_state ^= g_state >> 12;
    g_state ^= g_state << 25;
    g_state ^= g_state >> 27;
    unsigned long long r = g_state * 0x2545F4914F6CDD1Dull;
    return (int)((r >> 33) & 0x7fffffffu);
}
