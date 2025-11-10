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
#include "../libc/include/stdio.h"   // for itoa

void kernel_main(void) {
    gdt_initialize();
    idt_initialize();
    pit_initialize(100);
    keyboard_initialize();
    interrupts_enable();
    rtc_initialize();


    shell_initialize();

    while (1) {
        if (keyboard_data_available()) {
            char c = keyboard_getchar();
            shell_process_char(c);
        }
        __asm__ __volatile__("hlt; nop");
    }
}
