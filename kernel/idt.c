#include <kernel/idt.h>
#include <kernel/io.h>  // Add this line
#include <string.h>

#define IDT_ENTRIES 256

// IDT entries array
static struct idt_entry idt_entries[IDT_ENTRIES];
// IDT pointer
static struct idt_ptr idtp;

// External assembly functions
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

// IRQ handlers
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

/**
 * Set an entry in the IDT
 */
void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt_entries[num].base_low = base & 0xFFFF;
    idt_entries[num].selector = sel;
    idt_entries[num].always0 = 0;
    idt_entries[num].flags = flags;
    idt_entries[num].base_high = (base >> 16) & 0xFFFF;
}

/**
 * Initialize and load the IDT
 */
void idt_initialize() {
    // Set up the IDT pointer
    idtp.limit = (sizeof(struct idt_entry) * IDT_ENTRIES) - 1;
    idtp.base = (uint32_t)&idt_entries;

    // Clear the IDT
    memset(&idt_entries, 0, sizeof(struct idt_entry) * IDT_ENTRIES);

    // First disable all interrupts
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
    
    // Remap IRQs
    // Master PIC
    outb(0x20, 0x11);  // Initialize command
    io_wait();
    outb(0x21, 0x20);  // Vector offset (32)
    io_wait();
    outb(0x21, 0x04);  // Tell Master PIC that there is a slave at IRQ2
    io_wait();
    outb(0x21, 0x01);  // ICW4: 8086 mode
    io_wait();

    // Slave PIC
    outb(0xA0, 0x11);  // Initialize command
    io_wait();
    outb(0xA1, 0x28);  // Vector offset (40)
    io_wait();
    outb(0xA1, 0x02);  // Tell Slave PIC its cascade identity
    io_wait();
    outb(0xA1, 0x01);  // ICW4: 8086 mode
    io_wait();

    // Set up all ISRs (0-31)
    for (int i = 0; i < 32; i++) {
        idt_set_gate(i, (uint32_t)(&isr0) + (i * 16), 0x08, 0x8E);
    }
    
    // Set up all IRQs (32-47)
    for (int i = 0; i < 16; i++) {
        idt_set_gate(i + 32, (uint32_t)(&irq0) + (i * 16), 0x08, 0x8E);
    }

    // Unmask only the keyboard IRQ1
    outb(0x21, ~(1 << 1));  // Master PIC - only enable keyboard (IRQ1)
    outb(0xA1, 0xFF);       // Slave PIC - mask all interrupts
    io_wait();

    // Load the IDT
    idt_load(&idtp);
    
    // Interrupts will be enabled later when we're ready
}