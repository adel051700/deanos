#ifndef KERNEL_TASK_H
#define KERNEL_TASK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TASK_READY = 0,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_DEAD
} task_state_t;

/* Saved context for a kernel thread (ring0).
   Only ESP is stored here; callee-saved regs (edi, esi, ebx, ebp)
   are pushed/popped on the task's own stack by context_switch.s. */
typedef struct task_context {
    uint32_t esp;
} task_context_t;

typedef struct task {
    uint32_t        id;
    task_state_t    state;

    task_context_t  ctx;

    /* Kernel stack (for freeing later). */
    uintptr_t       kstack_base;
    uint32_t        kstack_size;
} task_t;

void tasking_initialize(void);
int  task_create(void (*entry)(void), uint32_t stack_size);
void task_yield(void);
void task_exit(void);

/* Called from PIT IRQ 0 (timer tick). */
void scheduler_tick(void);

#ifdef __cplusplus
}
#endif

#endif