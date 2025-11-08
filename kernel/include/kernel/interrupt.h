#ifndef _KERNEL_INTERRUPT_H
#define _KERNEL_INTERRUPT_H

#include <stdint.h>

// Structure for storing registers during interrupt
struct registers {
    // Data segment selector
    uint32_t ds;
    // Pushed by pusha
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    // Interrupt number and error code
    uint32_t int_no, err_code;   // <-- int_no first, then err_code
    // Pushed by the processor automatically
    uint32_t eip, cs, eflags, useresp, ss;
};

// Function pointer type for interrupt handlers
typedef void (*isr_t)(struct registers *);

// Register an interrupt handler
void register_interrupt_handler(uint8_t n, isr_t handler);

#endif