/*
 * task.c — Round-robin preemptive scheduler
 *
 * Each task gets a configurable time-slice (quantum) measured in PIT ticks.
 * On every PIT tick the running task's `ticks_left` is decremented.
 * A context switch happens only when:
 *   1. The quantum expires (ticks_left reaches 0), OR
 *   2. The task voluntarily yields (task_yield / task_exit).
 *
 * The idle task (index 0) is only chosen when nothing else is READY.
 */

#include "include/kernel/task.h"
#include "include/kernel/kheap.h"
#include "include/kernel/paging.h"
#include "include/kernel/tss.h"
#include "include/kernel/usermode.h"
#include "include/kernel/vfs.h"
#include <stddef.h>
#include <stdint.h>

#define DEFAULT_STACK_SIZE (16u * 1024u)

extern void context_switch(task_context_t* old, task_context_t* next);
extern void task_trampoline(void);   /* in context_switch.s */

static task_t  g_tasks[TASK_MAX];
static uint32_t g_task_count = 0;
static int      g_current    = -1;
static uint32_t g_next_id    = 1;
static uint64_t g_sched_ticks = 0;

static void task_fd_table_init(task_t* t) {
    if (!t) return;
    for (int i = 0; i < TASK_MAX_FDS; ++i) {
        t->fds[i].node = NULL;
        t->fds[i].offset = 0;
        t->fds[i].open_flags = 0;
        t->fds[i].fd_flags = 0;
        t->fds[i].in_use = 0;
    }
}

static void task_fd_table_close_all(task_t* t) {
    if (!t) return;
    for (int i = 0; i < TASK_MAX_FDS; ++i) {
        if (!t->fds[i].in_use) continue;
        vfs_close_node(t->fds[i].node);
        t->fds[i].node = NULL;
        t->fds[i].offset = 0;
        t->fds[i].open_flags = 0;
        t->fds[i].fd_flags = 0;
        t->fds[i].in_use = 0;
    }
}

static int task_fd_table_clone(task_t* dst, const task_t* src) {
    if (!dst || !src) return -1;
    task_fd_table_init(dst);

    for (int i = 0; i < TASK_MAX_FDS; ++i) {
        if (!src->fds[i].in_use) continue;

        dst->fds[i] = src->fds[i];
        if (vfs_open_node(dst->fds[i].node, dst->fds[i].open_flags) < 0) {
            task_fd_table_close_all(dst);
            return -1;
        }
    }

    return 0;
}

static void task_fd_table_close_cloexec(task_t* t) {
    if (!t) return;
    for (int i = 0; i < TASK_MAX_FDS; ++i) {
        if (!t->fds[i].in_use) continue;
        if (!(t->fds[i].fd_flags & TASK_FD_CLOEXEC)) continue;
        vfs_close_node(t->fds[i].node);
        t->fds[i].node = NULL;
        t->fds[i].offset = 0;
        t->fds[i].open_flags = 0;
        t->fds[i].fd_flags = 0;
        t->fds[i].in_use = 0;
    }
}

/* ---- internal helpers -------------------------------------------------- */

static uint32_t init_task_id(void);

static void idle_thread(void) {
    for (;;)
        __asm__ __volatile__("hlt");
}

static void fork_child_entry(void) {
    if (g_current < 0) {
        task_exit();
        return;
    }

    task_t* self = &g_tasks[g_current];
    if (!self->fork_resume_user) {
        task_exit();
        return;
    }

    self->fork_resume_user = 0;
    enter_usermode_with_ret(self->fork_user_eip,
                            self->fork_user_esp,
                            self->fork_user_eflags,
                            0);

    task_exit();
}

/*
 * pick_next_ready — Round-robin selection.
 * Scans from (g_current+1) wrapping around.  Skips the idle task (index 0)
 * unless it is the only READY task.
 */
static int pick_next_ready(void) {
    if (g_task_count == 0) return -1;

    int start = (g_current < 0) ? 0 : g_current;
    int best  = -1;

    for (uint32_t n = 0; n < g_task_count; ++n) {
        int i = (start + 1 + (int)n) % (int)g_task_count;
        if (g_tasks[i].state != TASK_READY) continue;

        /* Prefer non-idle tasks; fall back to idle only if nothing else. */
        if (i == 0) {
            if (best < 0) best = 0;       /* remember idle as fallback */
        } else {
            return i;                      /* first non-idle READY task */
        }
    }
    return best;  /* either idle (0) or -1 if nothing ready */
}

static void* alloc_kstack(uint32_t size) {
    if (size == 0) size = DEFAULT_STACK_SIZE;
    return kmalloc(size);
}

static int is_task_dead_or_missing(int id) {
    for (uint32_t i = 0; i < g_task_count; ++i) {
        if ((int)g_tasks[i].id == id)
            return (g_tasks[i].state == TASK_DEAD);
    }
    return 1;
}

static int has_child_for_wait(uint32_t parent_id, int pid_filter) {
    for (uint32_t i = 0; i < g_task_count; ++i) {
        const task_t* t = &g_tasks[i];
        if (t->parent_id != parent_id) continue;
        if (pid_filter > 0 && (int)t->id != pid_filter) continue;
        if (t->state == TASK_DEAD && t->wait_collected) continue;
        return 1;
    }
    return 0;
}

static int find_waitable_child_index(uint32_t parent_id, int pid_filter) {
    for (uint32_t i = 0; i < g_task_count; ++i) {
        const task_t* t = &g_tasks[i];
        if (t->parent_id != parent_id) continue;
        if (pid_filter > 0 && (int)t->id != pid_filter) continue;
        if (t->state != TASK_DEAD) continue;
        if (t->wait_collected) continue;
        return (int)i;
    }
    return -1;
}

static void reap_task_index(uint32_t idx) {
    if (idx >= g_task_count) return;
    if ((int)idx == g_current) return;
    if (g_tasks[idx].state != TASK_DEAD) return;

    if (g_tasks[idx].kstack_base) {
        kfree((void*)g_tasks[idx].kstack_base);
        g_tasks[idx].kstack_base = 0;
    }

    for (uint32_t i = idx + 1; i < g_task_count; ++i) {
        g_tasks[i - 1] = g_tasks[i];
    }

    if (g_task_count > 0) g_task_count--;
    if (g_current > (int)idx) g_current--;
}

static void sweep_reapable_tasks(void) {
    uint32_t init_id = init_task_id();
    for (uint32_t i = 0; i < g_task_count; ++i) {
        task_t* t = &g_tasks[i];
        if (t->state != TASK_DEAD) continue;
        if (t->id == init_id) continue;

        /* Parent-reaped tasks and init-orphans are reclaimable. */
        if (t->wait_collected || t->parent_id == init_id) {
            reap_task_index(i);
            i--;
        }
    }
}

static int find_task_index_by_id(int id) {
    if (id <= 0) return -1;
    for (uint32_t i = 0; i < g_task_count; ++i) {
        if ((int)g_tasks[i].id == id) return (int)i;
    }
    return -1;
}

static uint32_t init_task_id(void) {
    if (g_task_count > 0) return g_tasks[0].id;
    return 1;
}

static void reparent_children(uint32_t old_parent_id, uint32_t new_parent_id) {
    for (uint32_t i = 0; i < g_task_count; ++i) {
        if (g_tasks[i].parent_id == old_parent_id)
            g_tasks[i].parent_id = new_parent_id;
    }
}

static void release_task_mm(task_t* t) {
    if (!t) return;
    paging_release_mm_metadata(t->mm_id, t->mm_flags);
    t->mm_flags = 0;
}

static void wake_blocked_tasks(void) {
    for (uint32_t i = 0; i < g_task_count; ++i) {
        task_t* t = &g_tasks[i];
        if (t->state != TASK_BLOCKED) continue;

        if (t->wait_task_id > 0 && is_task_dead_or_missing(t->wait_task_id)) {
            int idx = find_task_index_by_id(t->wait_task_id);
            if (idx < 0 || (g_tasks[idx].state == TASK_DEAD && !g_tasks[idx].wait_collected)) {
                t->wait_task_id = 0;
                t->state = TASK_READY;
                continue;
            }
        }

        if (t->wait_task_id == -1) {
            int waitable = find_waitable_child_index(t->id, -1);
            if (waitable >= 0) {
                t->wait_task_id = 0;
                t->state = TASK_READY;
                continue;
            }
        }

        if (t->wake_tick != 0 && g_sched_ticks >= t->wake_tick) {
            t->wake_tick = 0;
            t->state = TASK_READY;
        }
    }
}

static void do_switch(int prev, int next) {
    if (prev >= 0 && g_tasks[prev].state == TASK_RUNNING)
        g_tasks[prev].state = TASK_READY;

    g_tasks[next].state      = TASK_RUNNING;
    g_tasks[next].ticks_left = g_tasks[next].quantum;

    /* Update TSS so ring-3 → ring-0 transitions use this task's kernel stack. */
    uint32_t kstack_top = g_tasks[next].kstack_base + g_tasks[next].kstack_size;
    tss_set_kernel_stack(kstack_top);

    g_current = next;

    context_switch(&g_tasks[prev].ctx, &g_tasks[next].ctx);
}

/* ---- public API -------------------------------------------------------- */

void tasking_initialize(void) {
    g_task_count = 0;
    g_current    = -1;
    g_next_id    = 1;
    g_sched_ticks = 0;

    /* Task 0 is the idle thread — always present, lowest priority. */
    task_create_named(idle_thread, DEFAULT_STACK_SIZE, 1, "idle");

    g_current = 0;
    g_tasks[0].state = TASK_RUNNING;
}

int task_create_named(void (*entry)(void), uint32_t stack_size,
                      uint32_t quantum, const char* name)
{
    if (!entry) return -1;
    if (g_task_count >= TASK_MAX) {
        sweep_reapable_tasks();
        if (g_task_count >= TASK_MAX) return -2;
    }

    task_t* t = &g_tasks[g_task_count];
    t->id      = g_next_id++;
    t->parent_id = (g_current >= 0) ? g_tasks[g_current].id : 0;
    t->state   = TASK_READY;
    t->quantum = (quantum > 0) ? quantum : TASK_DEFAULT_QUANTUM;
    t->ticks_left = t->quantum;
    t->wake_tick = 0;
    t->wait_task_id = 0;
    t->exit_status = 0;
    t->wait_collected = 0;
    t->mm_id = 1;
    t->mm_flags = 0;
    t->fork_user_eip = 0;
    t->fork_user_esp = 0;
    t->fork_user_eflags = 0;
    t->fork_resume_user = 0;
    task_fd_table_init(t);

    /* Copy name (or generate one). */
    if (name) {
        uint32_t i = 0;
        for (; i < TASK_NAME_LEN - 1 && name[i]; ++i)
            t->name[i] = name[i];
        t->name[i] = '\0';
    } else {
        /* "task_<id>" */
        t->name[0] = 't'; t->name[1] = '_';
        /* simple decimal id */
        uint32_t id = t->id;
        char tmp[12];
        int len = 0;
        do { tmp[len++] = '0' + (id % 10); id /= 10; } while (id);
        uint32_t p = 2;
        for (int j = len - 1; j >= 0 && p < TASK_NAME_LEN - 1; --j)
            t->name[p++] = tmp[j];
        t->name[p] = '\0';
    }

    if (stack_size == 0) stack_size = DEFAULT_STACK_SIZE;
    void* stack = alloc_kstack(stack_size);
    if (!stack) return -3;

    t->kstack_base = (uintptr_t)stack;
    t->kstack_size = stack_size;

    /* Build initial stack frame for context_switch's pop/pop/pop/pop/ret */
    uintptr_t top = (uintptr_t)stack + stack_size;
    top &= ~0xFu;

    uint32_t* sp = (uint32_t*)top;
    *(--sp) = (uint32_t)task_trampoline;   /* return address         */
    *(--sp) = 0;                            /* ebp                    */
    *(--sp) = (uint32_t)entry;             /* ebx = entry point      */
    *(--sp) = 0;                            /* esi                    */
    *(--sp) = 0;                            /* edi                    */
    t->ctx.esp = (uint32_t)sp;

    g_task_count++;
    return (int)t->id;
}

int task_create(void (*entry)(void), uint32_t stack_size) {
    return task_create_named(entry, stack_size, TASK_DEFAULT_QUANTUM, NULL);
}

void task_exit(void) {
    task_exit_with_status(0);
}

void task_exit_with_status(uint32_t status) {
    if (g_current >= 0) {
        uint32_t dying_id = g_tasks[g_current].id;
        reparent_children(dying_id, init_task_id());
        task_fd_table_close_all(&g_tasks[g_current]);
        g_tasks[g_current].exit_status = status;
        g_tasks[g_current].wait_collected = 0;
        release_task_mm(&g_tasks[g_current]);
        g_tasks[g_current].state = TASK_DEAD;
    }
    task_yield();
    for (;;) __asm__ __volatile__("hlt");
}

int task_kill(int id) {
    int idx = find_task_index_by_id(id);
    if (idx < 0) return -1;
    if (idx == 0) return -2; /* never kill idle */

    if (idx == g_current) {
        task_exit_with_status(128u + 9u);
        return 0;
    }

    if (g_tasks[idx].state == TASK_DEAD) return 0;

    uint32_t dying_id = g_tasks[idx].id;
    reparent_children(dying_id, init_task_id());
    task_fd_table_close_all(&g_tasks[idx]);
    release_task_mm(&g_tasks[idx]);

    g_tasks[idx].wake_tick = 0;
    g_tasks[idx].wait_task_id = 0;
    g_tasks[idx].exit_status = 128u + 9u; /* SIGKILL-style convention */
    g_tasks[idx].wait_collected = 0;
    g_tasks[idx].state = TASK_DEAD;
    return 0;
}

int task_fork_user(uint32_t user_eip, uint32_t user_esp, uint32_t user_eflags) {
    if (g_current < 0) return -1;

    task_t* parent = &g_tasks[g_current];
    int child_id = task_create_named(fork_child_entry,
                                     parent->kstack_size,
                                     parent->quantum,
                                     parent->name);
    if (child_id < 0) return child_id;

    int child_idx = find_task_index_by_id(child_id);
    if (child_idx < 0) return -2;

    task_t* child = &g_tasks[child_idx];
    child->parent_id = parent->id;

    uint32_t child_mm_id = parent->mm_id;
    uint32_t child_mm_flags = parent->mm_flags;
    if (paging_clone_current_mm_metadata(&child_mm_id, &child_mm_flags) < 0) {
        child->state = TASK_DEAD;
        return -3;
    }

    child->mm_id = child_mm_id;
    child->mm_flags = child_mm_flags;
    if (task_fd_table_clone(child, parent) < 0) {
        release_task_mm(child);
        child->state = TASK_DEAD;
        return -4;
    }
    child->fork_user_eip = user_eip;
    child->fork_user_esp = user_esp;
    child->fork_user_eflags = user_eflags;
    child->fork_resume_user = 1;
    return child_id;
}

void task_wait(int id) {
    (void)task_waitpid(id, NULL, 0);
}

int task_waitpid(int pid, int* status, uint32_t options) {
    if (g_current < 0) return -1;
    if (pid == 0 || pid < -1) return -2;

    uint32_t parent_id = g_tasks[g_current].id;

    for (;;) {
        int child_idx = find_waitable_child_index(parent_id, pid);
        if (child_idx >= 0) {
            task_t* child = &g_tasks[child_idx];
            int child_id = (int)child->id;
            uint32_t child_status = child->exit_status;
            if (status) *status = (int)child->exit_status;
            child->wait_collected = 1;

            reap_task_index((uint32_t)child_idx);
            if (status) *status = (int)child_status;
            return child_id;
        }

        if (!has_child_for_wait(parent_id, pid)) return -3;
        if (options & TASK_WAIT_NOHANG) return 0;

        g_tasks[g_current].wait_task_id = (pid > 0) ? pid : -1;
        g_tasks[g_current].wake_tick = 0;
        g_tasks[g_current].state = TASK_BLOCKED;
        task_yield();
    }
}

void task_sleep_ticks(uint64_t ticks) {
    if (ticks == 0) {
        task_yield();
        return;
    }
    if (g_current <= 0) return; /* never block idle/non-running context */

    g_tasks[g_current].wake_tick = g_sched_ticks + ticks;
    g_tasks[g_current].wait_task_id = 0;
    g_tasks[g_current].state = TASK_BLOCKED;
    task_yield();
}

void task_sleep_ms(uint32_t milliseconds) {
    /* Scheduler is PIT-driven at 100 Hz (10 ms per tick). */
    uint64_t ticks = ((uint64_t)milliseconds + 9u) / 10u;
    if (ticks == 0) ticks = 1;
    task_sleep_ticks(ticks);
}

void task_yield(void) {
    __asm__ __volatile__("int $0x20");
}

/*
 * scheduler_tick — called from PIT IRQ 0.
 *
 * Round-robin logic:
 *   • Decrement the running task's ticks_left.
 *   • If ticks_left > 0 AND the task is still RUNNING → keep running.
 *   • Otherwise pick the next READY task and switch.
 *   • If no other task is ready, let the current one keep going
 *     (or switch to idle if the current one died / blocked).
 */
void scheduler_tick(void) {
    g_sched_ticks++;
    wake_blocked_tasks();

    /* Decrement remaining slice of current task. */
    if (g_current >= 0 && g_tasks[g_current].state == TASK_RUNNING) {
        if (g_tasks[g_current].ticks_left > 0)
            g_tasks[g_current].ticks_left--;

        /* Still has time left → no switch. */
        if (g_tasks[g_current].ticks_left > 0)
            return;
    }

    /* Quantum expired or task is no longer RUNNING → find next. */
    int next = pick_next_ready();
    if (next < 0) return;                    /* nothing to run at all     */
    if (next == g_current) {
        /* Same task — just refill its quantum. */
        g_tasks[g_current].ticks_left = g_tasks[g_current].quantum;
        return;
    }

    int prev = g_current;
    do_switch(prev, next);
}

/* ---- Query helpers ----------------------------------------------------- */

uint32_t task_count(void)              { return g_task_count; }
const task_t* task_get(uint32_t idx)   { return (idx < g_task_count) ? &g_tasks[idx] : NULL; }
int task_current_id(void)              { return (g_current >= 0) ? (int)g_tasks[g_current].id : -1; }
int task_current_ppid(void)            { return (g_current >= 0) ? (int)g_tasks[g_current].parent_id : -1; }
int task_parent_id(int id) {
    int idx = find_task_index_by_id(id);
    if (idx < 0) return -1;
    return (int)g_tasks[idx].parent_id;
}

task_t* task_current(void) {
    return (g_current >= 0) ? &g_tasks[g_current] : NULL;
}

void task_set_current_name(const char* name) {
    if (g_current < 0 || !name) return;
    task_t* t = &g_tasks[g_current];
    uint32_t i = 0;
    for (; i < TASK_NAME_LEN - 1 && name[i]; ++i) {
        t->name[i] = name[i];
    }
    t->name[i] = '\0';
}

void task_close_cloexec_fds_current(void) {
    if (g_current < 0) return;
    task_fd_table_close_cloexec(&g_tasks[g_current]);
}

int task_clone_fd_to_task(int task_id, int target_fd, int src_fd) {
    if (g_current < 0) return -1;
    if (task_id <= 0) return -1;
    if (target_fd < 0 || target_fd >= TASK_MAX_FDS) return -1;
    if (src_fd < 0 || src_fd >= TASK_MAX_FDS) return -1;

    task_t* src_task = &g_tasks[g_current];
    if (!src_task->fds[src_fd].in_use) return -1;

    int dst_idx = find_task_index_by_id(task_id);
    if (dst_idx < 0) return -1;
    task_t* dst_task = &g_tasks[dst_idx];

    if (dst_task->fds[target_fd].in_use) {
        vfs_close_node(dst_task->fds[target_fd].node);
        dst_task->fds[target_fd].node = NULL;
        dst_task->fds[target_fd].offset = 0;
        dst_task->fds[target_fd].open_flags = 0;
        dst_task->fds[target_fd].fd_flags = 0;
        dst_task->fds[target_fd].in_use = 0;
    }

    dst_task->fds[target_fd] = src_task->fds[src_fd];
    if (vfs_open_node(dst_task->fds[target_fd].node, dst_task->fds[target_fd].open_flags) < 0) {
        dst_task->fds[target_fd].node = NULL;
        dst_task->fds[target_fd].offset = 0;
        dst_task->fds[target_fd].open_flags = 0;
        dst_task->fds[target_fd].fd_flags = 0;
        dst_task->fds[target_fd].in_use = 0;
        return -1;
    }

    return 0;
}

