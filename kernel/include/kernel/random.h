#ifndef _KERNEL_RANDOM_H
#define _KERNEL_RANDOM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RANDOM_SEED_THRESHOLD_BITS 128u

/* Initialize the CSPRNG and mix a boot seed (rdtsc/RTC/addresses).
 * Call once after pit_initialize() and before interrupts_enable(). */
void random_initialize(void);

/* Fold `len` bytes at `data` plus a fresh timestamp sample into the entropy
 * pool. `est_bits` is the caller's (conservative) estimate of new entropy in
 * bits; pass 0 to stir the pool without advancing the seed estimate.
 * Safe to call from interrupt context. */
void random_add_entropy(const void* data, uint32_t len, uint32_t est_bits);

/* Fill `out` with `n` cryptographically-random bytes. Never blocks. */
void random_bytes(void* out, uint32_t n);

/* Nonzero once enough entropy has been collected to consider the pool seeded. */
int random_is_seeded(void);

/* Create /dev/random and /dev/urandom in the VFS. Call after ramfs_initialize(). */
void devrandom_initialize(void);

#ifdef __cplusplus
}
#endif

#endif /* _KERNEL_RANDOM_H */
