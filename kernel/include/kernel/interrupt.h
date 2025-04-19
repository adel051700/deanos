#ifndef _KERNEL_INTERRUPT_H
#define _KERNEL_INTERRUPT_H

#include <stdint.h>

// Define registers structure for interrupt handlers
struct registers {
    uint32_t gs, fs, es, ds;                         // Pushed by common stub
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax; // Pushed by pusha
    uint32_t int_no, err_code;                       // Interrupt number and error code
    uint32_t eip, cs, eflags, useresp, ss;           // Pushed by processor automatically
};

// Register an interrupt handler
void register_interrupt_handler(uint8_t n, void (*handler)(struct registers*));

// ISR handler
void isr_handler(struct registers* regs);

// IRQ handler
void irq_handler(struct registers* regs);

#endif