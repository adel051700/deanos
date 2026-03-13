#include "include/kernel/idt.h"
#include "include/kernel/gdt.h"
#include "include/kernel/keyboard.h"
#include "include/kernel/shell.h"
#include "include/kernel/rtc.h"
#include "include/kernel/task.h"
#include "include/kernel/log.h"
#include "include/kernel/pit.h"
#include "include/kernel/syscall.h"
#include "include/kernel/tss.h"
#include "include/kernel/vfs.h"
#include "include/kernel/ramfs.h"
#include "include/kernel/elf.h"

static void shell_task(void) {
    while (1) {
        if (keyboard_data_available()) {
            char c = keyboard_getchar();
            shell_process_char(c);
        }
        __asm__ __volatile__("hlt; nop");
    }
}


void kernel_main(void) {
    gdt_initialize();
    tss_initialize(0x10, 0);    /* kernel SS=0x10, ESP0 updated per-task */
    idt_initialize();
    syscall_initialize();
    pit_initialize(100);
    keyboard_initialize();

    /* Filesystem: must come after kheap (initialized in constructor) */
    vfs_initialize();
    ramfs_initialize();
    elf_install_test_programs();

    shell_initialize();
    tasking_initialize();
    if (task_create_named(shell_task, 0, TASK_DEFAULT_QUANTUM, "shell") < 0) {
        klog("shell task creation failed");
    }

    interrupts_enable();
    rtc_initialize();

    while (1) {
        __asm__ __volatile__("hlt");
    }
}
