#ifndef _KERNEL_IDT_H
#define _KERNEL_IDT_H

#include <stdint.h>

// IDT entry structure
struct idt_entry {
    uint16_t base_low;   // Lower 16 bits of handler address
    uint16_t selector;   // Kernel segment selector
    uint8_t  always0;    // Always 0
    uint8_t  flags;      // Flags
    uint16_t base_high;  // Higher 16 bits of handler address
} __attribute__((packed));

// IDT pointer structure
struct idt_ptr {
    uint16_t limit;      // Size of IDT - 1
    uint32_t base;       // Base address of IDT
} __attribute__((packed));

// Initialize IDT
void idt_initialize(void);

// Set an IDT entry
void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags);

// Load IDT
extern void idt_load(struct idt_ptr* idt_ptr_addr);

// External ISR handlers
extern void isr0();
extern void isr1();
extern void isr2();
extern void isr3();
extern void isr4();
extern void isr5();
extern void isr6();
extern void isr7();
extern void isr8();
extern void isr9();
extern void isr10();
extern void isr11();
extern void isr12();
extern void isr13();
extern void isr14();
extern void isr15();
extern void isr16();
extern void isr17();
extern void isr18();
extern void isr19();
extern void isr20();
extern void isr21();
extern void isr22();
extern void isr23();
extern void isr24();
extern void isr25();
extern void isr26();
extern void isr27();
extern void isr28();
extern void isr29();
extern void isr30();
extern void isr31();

// External IRQ handlers
extern void irq0();
extern void irq1();  // Keyboard IRQ
extern void irq2();
extern void irq3();
extern void irq4();
extern void irq5();
extern void irq6();
extern void irq7();
extern void irq8();
extern void irq9();
extern void irq10();
extern void irq11();
extern void irq12();
extern void irq13();
extern void irq14();
extern void irq15();

#endif