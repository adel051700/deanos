#ifndef KERNEL_USERMODE_H
#define KERNEL_USERMODE_H

#include <stdint.h>

/*
 * Address at which the user code segment limit ends. Anything at or above
 * this is unreachable via CS-relative fetches and therefore non-executable
 * from ring 3, even though data accesses (DS/SS) still work. All user stacks
 * must live above this boundary so shellcode planted on them cannot execute.
 *
 * Must be 4 KiB aligned; the GDT enforces it with page-granular limits.
 */
#define USER_NX_BOUNDARY 0xBFFE0000u

/* Drop to ring 3 — implemented in context_switch.s */
extern void enter_usermode(uint32_t entry, uint32_t user_esp);
extern void enter_usermode_with_ret(uint32_t entry, uint32_t user_esp,
									uint32_t user_eflags, uint32_t user_eax);

/* Create a kernel task that immediately drops to ring 3 and runs `entry`. */
int user_task_create(void (*entry)(void), const char* name);

/* A tiny test program that runs entirely in ring 3. */
void user_test_program(void);

/* Register the #GP fault handler that catches user-mode NX violations. */
void usermode_install_protections(void);

#endif

