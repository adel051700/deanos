#ifndef KERNEL_USERMODE_H
#define KERNEL_USERMODE_H

#include <stdint.h>

/* Drop to ring 3 — implemented in context_switch.s */
extern void enter_usermode(uint32_t entry, uint32_t user_esp);

struct fork_frame;
extern void enter_usermode_fork(const struct fork_frame* frame);

#endif

