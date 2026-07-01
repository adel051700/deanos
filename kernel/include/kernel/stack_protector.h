#ifndef _KERNEL_STACK_PROTECTOR_H
#define _KERNEL_STACK_PROTECTOR_H

#include <stdint.h>

/* The canary value read by GCC's -fstack-protector prologue/epilogue. */
extern uintptr_t __stack_chk_guard;

/* Reseed __stack_chk_guard with a per-boot value. Call once, first thing in
 * kernel_main (see stack_protector.c for the ordering constraint). */
void stack_protector_init(void);

/* Invoked by protected epilogues on canary mismatch; never returns. */
__attribute__((noreturn)) void __stack_chk_fail(void);

#endif
