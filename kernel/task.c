#include "include/kernel/task.h"
#include "include/kernel/pmm.h"
#include "include/kernel/irq.h"
#include "include/kernel/pic.h"
#include "include/kernel/log.h"
#include <stddef.h>
#include <stdint.h>

#define MAX_TASKS  32
#define DEFAULT_STACK_SIZE (16u * 1024u)

extern void context_switch(task_context_t* old, task_context_t* next);

static task_t g_tasks[MAX_TASKS];
static uint32_t g_task_count = 0;
static int g_current = -1;
static uint32_t g_next_id = 1;

/* Simple "idle" thread. */
static void idle_thread(void) {
    for (;;) {
        __asm__ __volatile__("hlt");
    }
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
    /* Allocate stack as contiguous frames. */
    uint32_t frames = (size + 4095u) / 4096u;
    uintptr_t phys = phys_alloc_contiguous(frames, 1);
    if (!phys) return NULL;

    /* Assumes identity mapping for allocated physical memory or a direct map.
       If you do not identity map RAM, you must map this stack into kernel VA. */
    return (void*)phys;
}

void tasking_initialize(void) {
    /* Create task 0 as the currently running "bootstrap" context. */
    g_task_count = 0;
    g_current = -1;
    g_next_id = 1;

    /* Create an idle thread so the scheduler always has something to run. */
    (void)task_create(idle_thread, DEFAULT_STACK_SIZE);

    /* Mark task 0 (idle) ready, then set it running immediately. */
    g_current = 0;
    g_tasks[g_current].state = TASK_RUNNING;
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

    /* Prepare initial context:
       We will switch as if returning into `entry` with a fresh stack.
       The assembly stub uses `ctx.eip` and `ebp` as stack pointer bootstrap. */
    t->ctx.edi = 0;
    t->ctx.esi = 0;
    t->ctx.ebx = 0;

    /* Set "fake" EBP to top of stack (down\-growing). */
    uintptr_t top = (uintptr_t)stack + stack_size;
    top &= ~0xFu; /* align */
    t->ctx.ebp = (uint32_t)top;

    t->ctx.eip = (uint32_t)entry;

    g_task_count++;
    return (int)t->id;
}

void task_yield(void) {
    __asm__ __volatile__("int $0x20"); /* trigger timer vector (after PIC remap) */
}

void scheduler_tick(void) {
    /* Called from IRQ0 handler context (interrupts already disabled by CPU). */
    int next = pick_next_ready();
    if (next < 0 || next == g_current) return;

    int prev = g_current;
    if (prev >= 0 && g_tasks[prev].state == TASK_RUNNING) {
        g_tasks[prev].state = TASK_READY;
    }

    g_tasks[next].state = TASK_RUNNING;
    g_current = next;

    context_switch(&g_tasks[prev].ctx, &g_tasks[next].ctx);
}//
// Created by adel on 08/01/2026.
//