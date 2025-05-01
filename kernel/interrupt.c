#include <kernel/interrupt.h>
#include <kernel/idt.h>
#include <kernel/io.h>
#include <kernel/tty.h>
#include <stdio.h>

// Function pointer array for interrupt handlers
static isr_t interrupt_handlers[256] = {0};

/**
 * Register an interrupt handler for a specific interrupt
 */
void register_interrupt_handler(uint8_t n, isr_t handler) {
    interrupt_handlers[n] = handler;
}

/**
 * ISR handler - calls the appropriate registered handler
 * If no handler is registered, displays exception info and halts
 */
void isr_handler(struct registers* regs) {
    if (interrupt_handlers[regs->int_no]) {
        interrupt_handlers[regs->int_no](regs);
    } else if (regs->int_no < 32) {
        // Handle CPU exceptions
        terminal_writestring("\nEXCEPTION: ");
        
        // Print the exception name
        if (regs->int_no < sizeof(exception_names) / sizeof(char*)) {
            terminal_writestring(exception_names[regs->int_no]);
        } else {
            terminal_writestring("Unknown Exception");
        }
        
        // Print error code if applicable
        char error_code[16];
        itoa(regs->err_code, error_code, 16);
        terminal_writestring(" (");
        terminal_writestring(error_code);
        terminal_writestring(")\n");
        
        // Print register state
        char reg_value[16];
        
        terminal_writestring("EIP: 0x");
        itoa(regs->eip, reg_value, 16);
        terminal_writestring(reg_value);
        
        terminal_writestring(" CS: 0x");
        itoa(regs->cs, reg_value, 16);
        terminal_writestring(reg_value);
        
        terminal_writestring(" EFLAGS: 0x");
        itoa(regs->eflags, reg_value, 16);
        terminal_writestring(reg_value);
        
        terminal_writestring("\nSystem halted.\n");
        
        // Hang the system to prevent further execution
        while (1) {
            __asm__ __volatile__("hlt"); 
        }
    }
}

/**
 * IRQ handler - sends EOI to PICs and calls the appropriate registered handler
 */
void irq_handler(struct registers* regs) {
    // Send an EOI (End of Interrupt) to the PICs
    // If this interrupt involved the slave PIC (IRQs 8-15)
    if (regs->int_no >= 40) {
        outb(0xA0, 0x20); // Send reset signal to slave
    }
    // Send reset signal to master PIC
    outb(0x20, 0x20);

    // Call the handler if it exists
    if (interrupt_handlers[regs->int_no]) {
        interrupt_handlers[regs->int_no](regs);
    }
}

/**
 * Setup a basic timer interrupt handler
 */
static volatile uint32_t tick = 0;

static void timer_callback(struct registers *regs) {
    (void)regs; // Unused parameter
    
    tick++;
    
    // Display tick count every second (assuming PIT is set to ~100Hz)
    if (tick % 100 == 0) {
        char tick_str[16];
        itoa(tick / 100, tick_str, 10);
        terminal_writestring("\rSystem uptime: ");
        terminal_writestring(tick_str);
        terminal_writestring(" seconds");
    }
}

/**
 * Initialize the system timer
 */
void timer_initialize(uint32_t frequency) {
    // Register our timer callback
    register_interrupt_handler(32, timer_callback);
    
    // Intel 8253/8254 PIT (Programmable Interval Timer) setup
    // Input clock runs at 1.193182 MHz
    uint32_t divisor = 1193180 / frequency;
    
    // Send operational command
    outb(0x43, 0x36);  // Command: channel 0, access mode: lobyte/hibyte, mode 3 (square wave)
    
    // Set divisor
    outb(0x40, divisor & 0xFF);          // Low byte
    outb(0x40, (divisor >> 8) & 0xFF);   // High byte
}