#include "include/kernel/interrupt.h"
#include "include/kernel/idt.h"
#include "include/kernel/io.h"
#include "include/kernel/tty.h"
#include "include/kernel/rtc.h"
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
            "Reserved"
        };
        
        if (regs->int_no < 20) {
            terminal_writestring(exception_names[regs->int_no]);
        } else {
            terminal_writestring("Unknown Exception");
        }
        
        char error_code[16];
        itoa(regs->err_code, error_code, 16);
        terminal_writestring(" (");
        terminal_writestring(error_code);
        terminal_writestring(")\n");
        
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
        
        while (1) {
            __asm__ __volatile__("hlt"); 
        }
    }

    if (regs->int_no >= 32 && regs->int_no < 48) {
        if (regs->int_no >= 40) {
            outb(0xA0, 0x20);
        }
        outb(0x20, 0x20);
    }
}