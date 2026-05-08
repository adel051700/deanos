#ifndef KERNEL_STACK_PROTECTOR_H
#define KERNEL_STACK_PROTECTOR_H

#include <stdint.h>

/* GCC's -fstack-protector reads/writes this symbol directly. */
extern uintptr_t __stack_chk_guard;

/* Reseed the canary with runtime entropy. Safe to call once after boot
 * progresses far enough to reach RTC and the timestamp counter. */
void stack_protector_initialize(void);

#endif
