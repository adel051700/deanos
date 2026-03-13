/*
 * usermode.c — User-mode task support + test program
 */
#include "include/kernel/usermode.h"
#include "include/kernel/task.h"
#include "include/kernel/kheap.h"
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
static struct { void (*entry)(void); uintptr_t ustk; } g_launch;
static void user_task_wrapper(void) {
    void (*entry)(void) = g_launch.entry;
    uintptr_t ustk = g_launch.ustk;
    uint32_t user_esp = (uint32_t)(ustk + USER_STACK_SIZE) & ~0xFu;
    enter_usermode((uint32_t)entry, user_esp);
}
int user_task_create(void (*entry)(void), const char* name) {
    if (!entry) return -1;
    void* ustk = kmalloc(USER_STACK_SIZE);
    if (!ustk) return -2;
    g_launch.entry = entry;
    g_launch.ustk  = (uintptr_t)ustk;
    return task_create_named(user_task_wrapper, 0,
                             TASK_DEFAULT_QUANTUM, name ? name : "user");
}
