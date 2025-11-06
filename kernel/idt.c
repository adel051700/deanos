#include "include/kernel/idt.h"
#include "include/kernel/io.h"
#include <string.h>

#define IDT_ENTRIES 256

// IDT entries array
static struct idt_entry idt_entries[IDT_ENTRIES];
// IDT pointer
static struct idt_ptr idtp;

// External assembly functions for ISRs and IRQs
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
extern void isr128(); // System call

// IRQ handlers
extern void irq0();  // Timer
extern void irq1();  // Keyboard
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
 * 
 * @param num    The interrupt number
 * @param base   The handler function address
 * @param sel    The segment selector
 * @param flags  The descriptor type and attributes
 */
void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt_entries[num].base_low = base & 0xFFFF;
    idt_entries[num].selector = sel;
    idt_entries[num].always0 = 0;
    idt_entries[num].flags = flags;
    idt_entries[num].base_high = (base >> 16) & 0xFFFF;
}

/**
 * Initialize and remap the Programmable Interrupt Controllers (PICs)
 * This remaps the IRQs 0-15 to IDT entries 32-47
 */
static void pic_initialize(void) {
    // ICW1: Start initialization sequence in cascade mode
    outb(0x20, 0x11);  // Master PIC
    io_wait();
    outb(0xA0, 0x11);  // Slave PIC
    io_wait();
    
    // ICW2: Set vector offset (remap IRQs)
    outb(0x21, 0x20);  // Master PIC - IRQs 0-7 mapped to 32-39
    io_wait();
    outb(0xA1, 0x28);  // Slave PIC - IRQs 8-15 mapped to 40-47
    io_wait();
    
    // ICW3: Set up cascading
    outb(0x21, 0x04);  // Tell Master PIC that there is a slave at IRQ2 (0000 0100)
    io_wait();
    outb(0xA1, 0x02);  // Tell Slave PIC its cascade identity (0000 0010)
    io_wait();
    
    // ICW4: Set additional environment info
    outb(0x21, 0x01);  // 8086/88 mode, normal EOI, non-buffered
    io_wait();
    outb(0xA1, 0x01);  // Same for slave
    io_wait();
    
    // Mask interrupts initially - we'll enable them later as needed
    outb(0x21, 0xFC);  // Enable IRQ0 (timer) and IRQ1 (keyboard) only
    io_wait();
    outb(0xA1, 0xFF);  // Disable all IRQs on slave
    io_wait();
}

/**
 * Initialize and load the IDT
 */
void idt_initialize(void) {
    // Set up the IDT pointer
    idtp.limit = (sizeof(struct idt_entry) * IDT_ENTRIES) - 1;
    idtp.base = (uint32_t)&idt_entries;

    // Clear the IDT - initialize all entries to 0
    memset(&idt_entries, 0, sizeof(struct idt_entry) * IDT_ENTRIES);

    // Remap the PICs
    pic_initialize();
    
    // Set up all ISRs (0-31) - CPU exceptions
    idt_set_gate(0, (uint32_t)isr0, 0x08, 0x8E);   // Division by Zero
    idt_set_gate(1, (uint32_t)isr1, 0x08, 0x8E);   // Debug Exception
    idt_set_gate(2, (uint32_t)isr2, 0x08, 0x8E);   // Non Maskable Interrupt
    idt_set_gate(3, (uint32_t)isr3, 0x08, 0x8E);   // Breakpoint Exception
    idt_set_gate(4, (uint32_t)isr4, 0x08, 0x8E);   // Into Detected Overflow
    idt_set_gate(5, (uint32_t)isr5, 0x08, 0x8E);   // Out of Bounds Exception
    idt_set_gate(6, (uint32_t)isr6, 0x08, 0x8E);   // Invalid Opcode Exception
    idt_set_gate(7, (uint32_t)isr7, 0x08, 0x8E);   // No Coprocessor Exception
    idt_set_gate(8, (uint32_t)isr8, 0x08, 0x8E);   // Double Fault Exception
    idt_set_gate(9, (uint32_t)isr9, 0x08, 0x8E);   // Coprocessor Segment Overrun
    idt_set_gate(10, (uint32_t)isr10, 0x08, 0x8E); // Bad TSS Exception
    idt_set_gate(11, (uint32_t)isr11, 0x08, 0x8E); // Segment Not Present
    idt_set_gate(12, (uint32_t)isr12, 0x08, 0x8E); // Stack Fault Exception
    idt_set_gate(13, (uint32_t)isr13, 0x08, 0x8E); // General Protection Fault
    idt_set_gate(14, (uint32_t)isr14, 0x08, 0x8E); // Page Fault Exception
    idt_set_gate(15, (uint32_t)isr15, 0x08, 0x8E); // Reserved
    idt_set_gate(16, (uint32_t)isr16, 0x08, 0x8E); // Floating Point Exception
    idt_set_gate(17, (uint32_t)isr17, 0x08, 0x8E); // Alignment Check Exception
    idt_set_gate(18, (uint32_t)isr18, 0x08, 0x8E); // Machine Check Exception
    idt_set_gate(19, (uint32_t)isr19, 0x08, 0x8E); // Reserved
    idt_set_gate(20, (uint32_t)isr20, 0x08, 0x8E); // Reserved
    idt_set_gate(21, (uint32_t)isr21, 0x08, 0x8E); // Reserved
    idt_set_gate(22, (uint32_t)isr22, 0x08, 0x8E); // Reserved
    idt_set_gate(23, (uint32_t)isr23, 0x08, 0x8E); // Reserved
    idt_set_gate(24, (uint32_t)isr24, 0x08, 0x8E); // Reserved
    idt_set_gate(25, (uint32_t)isr25, 0x08, 0x8E); // Reserved
    idt_set_gate(26, (uint32_t)isr26, 0x08, 0x8E); // Reserved
    idt_set_gate(27, (uint32_t)isr27, 0x08, 0x8E); // Reserved
    idt_set_gate(28, (uint32_t)isr28, 0x08, 0x8E); // Reserved
    idt_set_gate(29, (uint32_t)isr29, 0x08, 0x8E); // Reserved
    idt_set_gate(30, (uint32_t)isr30, 0x08, 0x8E); // Reserved
    idt_set_gate(31, (uint32_t)isr31, 0x08, 0x8E); // Reserved
    
    // Set up all IRQs (32-47) - Hardware interrupts
    idt_set_gate(32, (uint32_t)irq0, 0x08, 0x8E);  // IRQ0: Timer
    idt_set_gate(33, (uint32_t)irq1, 0x08, 0x8E);  // IRQ1: Keyboard
    idt_set_gate(34, (uint32_t)irq2, 0x08, 0x8E);  // IRQ2: PIC Cascade
    idt_set_gate(35, (uint32_t)irq3, 0x08, 0x8E);  // IRQ3: COM2
    idt_set_gate(36, (uint32_t)irq4, 0x08, 0x8E);  // IRQ4: COM1
    idt_set_gate(37, (uint32_t)irq5, 0x08, 0x8E);  // IRQ5: LPT2
    idt_set_gate(38, (uint32_t)irq6, 0x08, 0x8E);  // IRQ6: Floppy Disk
    idt_set_gate(39, (uint32_t)irq7, 0x08, 0x8E);  // IRQ7: LPT1
    idt_set_gate(40, (uint32_t)irq8, 0x08, 0x8E);  // IRQ8: RTC
    idt_set_gate(41, (uint32_t)irq9, 0x08, 0x8E);  // IRQ9: Free
    idt_set_gate(42, (uint32_t)irq10, 0x08, 0x8E); // IRQ10: Free
    idt_set_gate(43, (uint32_t)irq11, 0x08, 0x8E); // IRQ11: Free
    idt_set_gate(44, (uint32_t)irq12, 0x08, 0x8E); // IRQ12: PS/2 Mouse
    idt_set_gate(45, (uint32_t)irq13, 0x08, 0x8E); // IRQ13: FPU
    idt_set_gate(46, (uint32_t)irq14, 0x08, 0x8E); // IRQ14: Primary ATA
    idt_set_gate(47, (uint32_t)irq15, 0x08, 0x8E); // IRQ15: Secondary ATA
    
    // Set up system call interrupt (128)
    idt_set_gate(128, (uint32_t)isr128, 0x08, 0xEE); // 0xEE = user privilege level (3)

    // Load the IDT
    idt_load(&idtp);
}

/**
 * Enable hardware interrupts
 */
void interrupts_enable(void) {
    __asm__ volatile ("sti");
}

/**
 * Disable hardware interrupts
 */
void interrupts_disable(void) {
    __asm__ volatile ("cli");
}