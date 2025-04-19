#include <stddef.h>
#include <stdint.h>
#include <kernel/tty.h>
#include <kernel/idt.h>
#include <kernel/keyboard.h>
#include <kernel/shell.h>

// Function to pause execution briefly
static void pause(void) {
    for (volatile int i = 0; i < 500000; i++) { }
}

void kernel_main(void) {
    // Disable interrupts - we'll use polling for keyboard
    __asm__ __volatile__("cli"); 
    
    // Set up the IDT
    idt_initialize();
    
    // Initialize keyboard (polling mode)
    keyboard_initialize();
    
    // Initialize shell
    shell_initialize();
    
    // Display welcome message
    terminal_writestring("System initialized.\n");
    
    // Main kernel loop - polling keyboard
    while (1) {
        // Check for keyboard input
        keyboard_update();
        
        if (keyboard_data_available()) {
            char c = keyboard_getchar();
            shell_process_char(c);
        }
        
        // Small delay to reduce CPU usage
        pause();
    }
}