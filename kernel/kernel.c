#include <stddef.h>
#include <stdint.h>
#include "include/kernel/tty.h"
#include "include/kernel/idt.h"
#include "include/kernel/gdt.h"
#include "include/kernel/keyboard.h"
#include "include/kernel/shell.h"
#include "include/kernel/interrupt.h"
#include "include/kernel/rtc.h"
#include "include/kernel/pit.h"
#include "include/kernel/pmm.h"
#include "include/kernel/paging.h"
#include "include/kernel/kheap.h"
#include "include/kernel/syscall.h"
//#include "include/kernel/sched.h"   // REMOVE THIS LINE
#include "../libc/include/stdio.h"   // for itoa

// static void demo_thread(void* arg) { /* REMOVE whole function */ }

void kernel_main(void) {
    gdt_initialize();
    idt_initialize();
    syscall_initialize();
    pit_initialize(100);
    keyboard_initialize();

    // sched_initialize(); // removed

    interrupts_enable();
    rtc_initialize();

    shell_initialize();

    // task_create(...) // removed demo tasks

    while (1) {
        if (keyboard_data_available()) {
            char c = keyboard_getchar();
            shell_process_char(c);
        }
        __asm__ __volatile__("hlt; nop");
    }
}
