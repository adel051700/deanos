#ifndef _KERNEL_INTERRUPT_H
#define _KERNEL_INTERRUPT_H

#include <stdint.h>

// Exception names for better error reporting
static const char *exception_names[] = {
    "Division By Zero",
    "Debug",
    "Non Maskable Interrupt",
    "Breakpoint",
    "Into Detected Overflow",
    "Out of Bounds",
    "Invalid Opcode",
    "No Coprocessor",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Bad TSS",
    "Segment Not Present",
    "Stack Fault",
    "General Protection Fault",
    "Page Fault",
    "Unknown Interrupt",
    "Coprocessor Fault",
    "Alignment Check",
    "Machine Check",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved"
};

// Structure for storing registers during interrupt
struct registers {
    // Data segment selector
    uint32_t ds;
    // Pushed by pusha
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    // Interrupt number and error code
    uint32_t int_no, err_code;
    // Pushed by the processor automatically
    uint32_t eip, cs, eflags, useresp, ss;
};

// Function pointer type for interrupt handlers
typedef void (*isr_t)(struct registers *);

// Register an interrupt handler
void register_interrupt_handler(uint8_t n, isr_t handler);

#endif