#include <kernel/interrupt.h>
#include <kernel/idt.h>
#include <kernel/io.h>
#include <kernel/tty.h>
#include <stdio.h>

// Function pointer array for interrupt handlers
static void (*interrupt_handlers[256])(struct registers*) = {0};

/**
 * Register an interrupt handler for a specific interrupt
 */
void register_interrupt_handler(uint8_t n, void (*handler)(struct registers*)) {
    interrupt_handlers[n] = handler;
}

/**
 * ISR handler - calls the appropriate registered handler
 */
void isr_handler(struct registers* regs) {
    if (interrupt_handlers[regs->int_no]) {
        interrupt_handlers[regs->int_no](regs);
    } else {
        terminal_writestring("Unhandled exception: ");
        char s[3];
        itoa(regs->int_no, s, 10);
        terminal_writestring(s);
        terminal_writestring("\n");

        //Hang the system to prevent further execution
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

    if (interrupt_handlers[regs->int_no]) {
        interrupt_handlers[regs->int_no](regs);
    }
}