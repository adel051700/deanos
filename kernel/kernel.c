#include <stddef.h>
#include <stdint.h>
#include "include/kernel/tty.h"
#include "include/kernel/idt.h"
#include "include/kernel/gdt.h"
#include "include/kernel/keyboard.h"
#include "include/kernel/shell.h"
#include "include/kernel/interrupt.h"
#include "include/kernel/rtc.h"

void kernel_main(void) {
    // Initialize GDT first
    gdt_initialize();
    
    // Set up the IDT
    idt_initialize();
    
    // Initialize RTC and save boot time
    rtc_initialize();
    
    // Initialize timer (100 Hz) - BEFORE enabling interrupts
    timer_initialize(100);
    
    // Enable interrupts AFTER timer is set up
    interrupts_enable();
    
    // Initialize keyboard (polling mode in the current implementation)
    keyboard_initialize();
    
    // Initialize shell
    shell_initialize();
    
    // Main kernel loop - polling keyboard
    while (1) {
        // Check for keyboard input
        keyboard_update();
        
        if (keyboard_data_available()) {
            char c = keyboard_getchar();
            shell_process_char(c);
        }
    }
}
