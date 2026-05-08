/*
 * usermode.c — User-mode task support + test program
 */
#include "include/kernel/usermode.h"
#include "include/kernel/task.h"
#include "include/kernel/paging.h"
#include "include/kernel/interrupt.h"
#include "include/kernel/log.h"
#include "include/kernel/tty.h"
#include <stdio.h>
#include <stdint.h>
/* ---- User-space syscall helpers ---------------------------------------- */
static inline long user_syscall3(uint32_t num, uint32_t a1,
                                  uint32_t a2, uint32_t a3)
{
    long ret;
    __asm__ __volatile__(
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a1), "c"(a2), "d"(a3)
        : "memory"
    );
    return ret;
}
#define USYS_write 1
#define USYS_exit  3
static inline void user_puts(const char* s) {
    long n = 0;
    while (s[n]) n++;
    user_syscall3(USYS_write, 1, (uint32_t)s, (uint32_t)n);
}
static inline void user_exit(uint32_t code) {
    user_syscall3(USYS_exit, code, 0, 0);
}
/* ---- The test program (runs in ring 3) --------------------------------- */
void user_test_program(void) {
    user_puts("[user] Hello from ring 3!\n");
    user_puts("[user] Calling sys_exit(0)...\n");
    user_exit(0);
    for (;;) ;
}
/* ---- Kernel-side launcher ---------------------------------------------- */
#define USER_STACK_SIZE  (8u * 1024u)
#define USER_STACK_BASE  0xBFFF4000u
/* Ensure the test stack lives in the non-executable region carved out by the
 * user code segment limit. */
_Static_assert(USER_STACK_BASE >= USER_NX_BOUNDARY,
               "user stack must sit above USER_NX_BOUNDARY for NX emulation");
static struct { void (*entry)(void); uintptr_t ustk; } g_launch;
static void user_task_wrapper(void) {
    void (*entry)(void) = g_launch.entry;
    uintptr_t ustk = g_launch.ustk;
    uint32_t user_esp = (uint32_t)(ustk + USER_STACK_SIZE) & ~0xFu;
    enter_usermode((uint32_t)entry, user_esp);
}
int user_task_create(void (*entry)(void), const char* name) {
    if (!entry) return -1;
    for (uintptr_t va = USER_STACK_BASE; va < USER_STACK_BASE + USER_STACK_SIZE; va += 4096u) {
        if (paging_map_user(va) < 0) return -2;
    }
    g_launch.entry = entry;
    g_launch.ustk  = USER_STACK_BASE;
    return task_create_named(user_task_wrapper, 0,
                             TASK_DEFAULT_QUANTUM, name ? name : "user");
}

/* ---- #GP fault handler ------------------------------------------------- */
/*
 * Without PAE the CPU has no per-page NX bit, so we shrink the user code
 * segment to make stacks non-executable. An attempted fetch outside that
 * segment raises #GP from ring 3. We diagnose and halt — same fail-stop
 * policy as the existing page fault handler.
 */
static void user_gp_fault_handler(struct registers* r) {
    char buf[16];
    int from_user = ((r->cs & 0x3u) == 0x3u);

    terminal_writestring(from_user
        ? "\nUSER #GP at EIP=0x"
        : "\nKERNEL #GP at EIP=0x");
    itoa(r->eip, buf, 16);
    terminal_writestring(buf);
    terminal_writestring(" err=0x");
    itoa(r->err_code, buf, 16);
    terminal_writestring(buf);
    if (from_user && r->eip >= USER_NX_BOUNDARY) {
        terminal_writestring(" (NX violation: execution above 0x");
        itoa(USER_NX_BOUNDARY, buf, 16);
        terminal_writestring(buf);
        terminal_writestring(")");
        klog("user attempted to execute non-executable region (NX)");
    } else {
        klog(from_user ? "user general protection fault"
                       : "kernel general protection fault");
    }
    terminal_writestring("\nSystem halted.\n");
    for (;;) __asm__ __volatile__("hlt");
}

void usermode_install_protections(void) {
    register_interrupt_handler(13, user_gp_fault_handler);
}
