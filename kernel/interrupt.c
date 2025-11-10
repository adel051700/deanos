#include "include/kernel/interrupt.h"
#include "include/kernel/idt.h"
#include "include/kernel/tty.h"
#include "include/kernel/irq.h"
#include <stdio.h>

static isr_t interrupt_handlers[256] = {0};

void register_interrupt_handler(uint8_t n, isr_t handler) {
    interrupt_handlers[n] = handler;
}

void isr_handler(struct registers* regs) {
    if (interrupt_handlers[regs->int_no]) {
        interrupt_handlers[regs->int_no](regs);
    } 
    else if (regs->int_no < 32) {
        terminal_writestring("\nEXCEPTION: ");
        const char *exception_names[] = {
            "Division By Zero","Debug","Non Maskable Interrupt","Breakpoint",
            "Into Detected Overflow","Out of Bounds","Invalid Opcode","No Coprocessor",
            "Double Fault","Coprocessor Segment Overrun","Bad TSS","Segment Not Present",
            "Stack Fault","General Protection Fault","Page Fault","Unknown Interrupt",
            "Coprocessor Fault","Alignment Check","Machine Check","Reserved"
        };
        if (regs->int_no < 20) terminal_writestring(exception_names[regs->int_no]);
        else terminal_writestring("Unknown Exception");
        char buf[16];
        itoa(regs->err_code, buf, 16);
        terminal_writestring(" ("); terminal_writestring(buf); terminal_writestring(")\n");
        itoa(regs->eip, buf, 16);
        terminal_writestring("EIP: 0x"); terminal_writestring(buf);
        itoa(regs->cs, buf, 16);
        terminal_writestring(" CS: 0x"); terminal_writestring(buf);
        itoa(regs->eflags, buf, 16);
        terminal_writestring(" EFLAGS: 0x"); terminal_writestring(buf);
        terminal_writestring("\nSystem halted.\n");
        while (1) { __asm__ __volatile__("hlt"); }
    }

    // Hardware IRQs (PIC remapped to 32..47)
    if (regs->int_no >= 32 && regs->int_no < 48) {
        irq_dispatch((uint8_t)(regs->int_no - 32), regs);
    }
}