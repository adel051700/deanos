#ifndef _KERNEL_IDT_H
#define _KERNEL_IDT_H

#include <stdint.h>

// Descriptor structure for IDT entries
struct idt_entry {
    uint16_t base_low;   // Lower 16 bits of handler function address
    uint16_t selector;   // Kernel segment selector
    uint8_t  always0;    // Must be 0
    uint8_t  flags;      // Flags (type & attributes)
    uint16_t base_high;  // Upper 16 bits of handler function address
} __attribute__((packed));

// IDTR register structure
struct idt_ptr {
    uint16_t limit;      // Size of IDT array minus 1
    uint32_t base;       // Address of IDT array
} __attribute__((packed));

// Initialize the IDT
void idt_initialize(void);

// Set an entry in the IDT
void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags);

// Load the IDT (implemented in assembly)
extern void idt_load(struct idt_ptr *idt);

// Enable interrupts
void interrupts_enable(void);

// Disable interrupts
void interrupts_disable(void);

#endif