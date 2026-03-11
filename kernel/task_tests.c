#include "include/kernel/task_tests.h"
#include "include/kernel/task.h"
#include "include/kernel/log.h"
#include "include/kernel/tty.h"
#include "../libc/include/stdio.h"

#include <stdint.h>

#define TEST_ITERATIONS 5

static void log_task_iteration(const char* name, uint32_t count) {
    char buf[16];
    terminal_writestring(name);
    terminal_writestring(": run ");
    itoa(count, buf, 10);
    terminal_writestring(buf);
    terminal_writestring("\n");
}

static void spin_delay(void) {
    for (volatile uint32_t i = 0; i < 500000u; ++i)
        __asm__ __volatile__("nop");
}

static void context_switch_test_task_a(void) {
    for (uint32_t i = 1; i <= TEST_ITERATIONS; ++i) {
        log_task_iteration("Task A", i);
        spin_delay();
        task_yield();
    }
    klog("Task A: done");
    /* returning from here lands in task_trampoline → task_exit → TASK_DEAD */
}

static void context_switch_test_task_b(void) {
    for (uint32_t i = 1; i <= TEST_ITERATIONS; ++i) {
        log_task_iteration("Task B", i);
        spin_delay();
        task_yield();
    }
    klog("Task B: done");
}

void task_tests_initialize(void) {
    /* Task A gets 3 ticks (30ms), Task B gets 7 ticks (70ms).
       Both run 5 iterations — you can observe B keeps the CPU longer. */
    if (task_create_named(context_switch_test_task_a, 0, 3, "test_A") < 0)
        klog("task test A creation failed");
    if (task_create_named(context_switch_test_task_b, 0, 7, "test_B") < 0)
        klog("task test B creation failed");
}
