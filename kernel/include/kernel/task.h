#ifndef KERNEL_TASK_H
#define KERNEL_TASK_H

#include <stdint.h>

#define TASK_MM_SHARED 0x1u
#define TASK_WAIT_NOHANG 0x1u

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
    int             wait_task_id;   /* >0 specific child, -1 any child, 0 none */
    uint32_t        exit_status;    /* _exit(status) value */
    uint8_t         wait_collected; /* set once parent reaps exit status */

    /* Kernel stack */
    uintptr_t       kstack_base;
    uint32_t        kstack_size;

    /* Address-space metadata (groundwork for fork/COW). */
    uint32_t        mm_id;
    uint32_t        mm_flags;

    /* Fork return context for child first run. */
    uint32_t        fork_user_eip;
    uint32_t        fork_user_esp;
    uint32_t        fork_user_eflags;
    uint8_t         fork_resume_user;
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
void task_exit_with_status(uint32_t status);
/* Mark task DEAD by id. Returns 0 on success, negative on error. */
int  task_kill(int id);
/* Block until the task with the given ID is TASK_DEAD (or not found). */
void task_wait(int id);
int task_waitpid(int pid, int* status, uint32_t options);
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
void task_set_current_name(const char* name);

/* Fork groundwork: clone current task metadata and user return context. */
int task_fork_user(uint32_t user_eip, uint32_t user_esp, uint32_t user_eflags);

#ifdef __cplusplus
}
#endif

#endif