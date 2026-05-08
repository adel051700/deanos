/* Simple kernel randomness/entropy interface */
#ifndef _KERNEL_RANDOM_H
#define _KERNEL_RANDOM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize randomness subsystem and create /dev/random and /dev/urandom */
void random_initialize(void);

/* Mix an external 32-bit event into the entropy pool (call from IRQ handlers) */
void random_add_entropy(uint32_t event);

/* Fill buffer with cryptographically-weak random bytes (non-blocking) */
void random_get_bytes(uint8_t* out, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* _KERNEL_RANDOM_H */

