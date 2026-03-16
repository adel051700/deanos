#ifndef KERNEL_TASK_H
#define KERNEL_TASK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Configuration ----------------------------------------------------- */
#define TASK_MAX          32
#define TASK_NAME_LEN     16
#define TASK_DEFAULT_QUANTUM  5   /* PIT ticks per time-slice (50 ms at 100 Hz) */

/* ---- Types ------------------------------------------------------------- */

typedef enum {
    TASK_READY = 0,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_DEAD
} task_state_t;

/* Only ESP is stored here; callee-saved regs live on the task's own stack. */
typedef struct task_context {
    uint32_t esp;
} task_context_t;

typedef struct task {
    uint32_t        id;
    uint32_t        parent_id;
    task_state_t    state;
    char            name[TASK_NAME_LEN];

    task_context_t  ctx;

    /* Round-robin scheduling */
    uint32_t        quantum;        /* ticks allowed per slice           */
    uint32_t        ticks_left;     /* ticks remaining in current slice  */

    /* Blocking state */
    uint64_t        wake_tick;      /* scheduler tick to wake on (0 = none) */
    int             wait_task_id;   /* task id waited on by task_wait (0 = none) */

    /* Kernel stack */
    uintptr_t       kstack_base;
    uint32_t        kstack_size;
} task_t;

/* ---- Public API -------------------------------------------------------- */

void tasking_initialize(void);

/* Create a task.  quantum=0 → use TASK_DEFAULT_QUANTUM.  name may be NULL. */
int  task_create_named(void (*entry)(void), uint32_t stack_size,
                       uint32_t quantum, const char* name);

/* Convenience: default quantum, auto-generated name. */
int  task_create(void (*entry)(void), uint32_t stack_size);

void task_yield(void);
void task_exit(void);
/* Mark task DEAD by id. Returns 0 on success, negative on error. */
int  task_kill(int id);
/* Block until the task with the given ID is TASK_DEAD (or not found). */
void task_wait(int id);
/* Block current task for N scheduler ticks (N=0 => yield). */
void task_sleep_ticks(uint64_t ticks);
/* Convenience wrapper around scheduler ticks at 100 Hz PIT default. */
void task_sleep_ms(uint32_t milliseconds);

/* Called from PIT IRQ 0 (timer tick). */
void scheduler_tick(void);

/* Query helpers (for shell / diagnostics). */
uint32_t task_count(void);
const task_t* task_get(uint32_t index);
int task_current_id(void);
int task_current_ppid(void);
int task_parent_id(int id);

#ifdef __cplusplus
}
#endif

#endif