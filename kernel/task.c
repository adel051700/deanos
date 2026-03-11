#include "include/kernel/task.h"
#include "include/kernel/pmm.h"
#include "include/kernel/kheap.h"
#include "include/kernel/log.h"
#include <stddef.h>
#include <stdint.h>

#define MAX_TASKS  32
#define DEFAULT_STACK_SIZE (16u * 1024u)

extern void context_switch(task_context_t* old, task_context_t* next);
extern void task_trampoline(void);   /* in context_switch.s */

static task_t g_tasks[MAX_TASKS];
static uint32_t g_task_count = 0;
static int g_current = -1;
static uint32_t g_next_id = 1;

/* Simple "idle" thread — just halts until next interrupt. */
static void idle_thread(void) {
    for (;;)
        __asm__ __volatile__("hlt");
}

static int pick_next_ready(void) {
    if (g_task_count == 0) return -1;
    int start = (g_current < 0) ? 0 : g_current;
    for (uint32_t n = 0; n < g_task_count; ++n) {
        int i = (start + 1 + (int)n) % (int)g_task_count;
        if (g_tasks[i].state == TASK_READY) return i;
    }
    return -1;
}

static void* alloc_kstack(uint32_t size) {
    if (size == 0) size = DEFAULT_STACK_SIZE;
    void* p = kmalloc(size);
    return p;
}

/* ---- public API -------------------------------------------------------- */

void tasking_initialize(void) {
    g_task_count = 0;
    g_current = -1;
    g_next_id = 1;

    (void)task_create(idle_thread, DEFAULT_STACK_SIZE);

    g_current = 0;
    g_tasks[g_current].state = TASK_RUNNING;
}

void task_exit(void) {
    /* Mark the current task as dead so the scheduler never picks it again. */
    if (g_current >= 0)
        g_tasks[g_current].state = TASK_DEAD;
    task_yield();
    /* Should never reach here, but just in case: */
    for (;;) __asm__ __volatile__("hlt");
}

int task_create(void (*entry)(void), uint32_t stack_size) {
    if (!entry) return -1;
    if (g_task_count >= MAX_TASKS) return -2;

    task_t* t = &g_tasks[g_task_count];
    t->id = g_next_id++;
    t->state = TASK_READY;

    if (stack_size == 0) stack_size = DEFAULT_STACK_SIZE;
    void* stack = alloc_kstack(stack_size);
    if (!stack) return -3;

    t->kstack_base = (uintptr_t)stack;
    t->kstack_size = stack_size;

    uintptr_t top = (uintptr_t)stack + stack_size;
    top &= ~0xFu;

    uint32_t* sp = (uint32_t*)top;
    *(--sp) = (uint32_t)task_trampoline;
    *(--sp) = 0;                              /* ebp */
    *(--sp) = (uint32_t)entry;               /* ebx = entry point */
    *(--sp) = 0;                              /* esi */
    *(--sp) = 0;                              /* edi */

    t->ctx.esp = (uint32_t)sp;

    g_task_count++;
    return (int)t->id;
}

void task_yield(void) {
    __asm__ __volatile__("int $0x20");
}

void scheduler_tick(void) {
    int next = pick_next_ready();
    if (next < 0 || next == g_current) return;

    int prev = g_current;
    if (prev >= 0 && g_tasks[prev].state == TASK_RUNNING)
        g_tasks[prev].state = TASK_READY;

    g_tasks[next].state = TASK_RUNNING;
    g_current = next;


    context_switch(&g_tasks[prev].ctx, &g_tasks[next].ctx);
}
