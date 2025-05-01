#include <stddef.h>
#include <stdint.h>
#include <kernel/tty.h>
#include <kernel/idt.h>
#include <kernel/gdt.h>
#include <kernel/keyboard.h>
#include <kernel/shell.h>
#include <kernel/interrupt.h>

// Initialize timer (new declaration)
void timer_initialize(uint32_t frequency);

void kernel_main(void) {
    // Initialize GDT first
    gdt_initialize();
    
    // Set up the IDT
    idt_initialize();
    
    // Initialize timer (100 Hz)
    timer_initialize(100);
    
    // Initialize keyboard (polling mode in the current implementation)
    keyboard_initialize();
    
    // Initialize shell
    shell_initialize();
    
    // Enable interrupts
    interrupts_enable();
    
    
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
