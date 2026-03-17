#ifndef KERNEL_USERMODE_H
#define KERNEL_USERMODE_H

#include <stdint.h>

/* Drop to ring 3 — implemented in context_switch.s */
extern void enter_usermode(uint32_t entry, uint32_t user_esp);
extern void enter_usermode_with_ret(uint32_t entry, uint32_t user_esp,
									uint32_t user_eflags, uint32_t user_eax);

/* Create a kernel task that immediately drops to ring 3 and runs `entry`. */
int user_task_create(void (*entry)(void), const char* name);

/* A tiny test program that runs entirely in ring 3. */
void user_test_program(void);

#endif

