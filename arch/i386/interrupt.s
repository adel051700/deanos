.section .text

// Common ISR stub that saves registers, calls the C handler, and restores registers
.macro ISR_NOERRCODE num
.global isr\num
isr\num:
    cli                  // Disable interrupts
    pushl $0             // Push dummy error code
    pushl $\num          // Push interrupt number
    jmp isr_common_stub  // Jump to common handler
.endm

// ISR stub for exceptions that supply their own error code
.macro ISR_ERRCODE num
.global isr\num
isr\num:
    cli                  // Disable interrupts
    // Error code already pushed by CPU
    pushl $\num          // Push interrupt number
    jmp isr_common_stub  // Jump to common handler
.endm

// IRQ stub
.macro IRQ num, irqnum
.global irq\num
irq\num:
    cli                  // Disable interrupts
    pushl $0             // Push dummy error code
    pushl $\irqnum       // Push interrupt number (32+n)
    jmp irq_common_stub  // Jump to common IRQ handler
.endm

// Define ISRs for CPU exceptions 0-31
ISR_NOERRCODE 0   // Division by zero
ISR_NOERRCODE 1   // Debug
ISR_NOERRCODE 2   // Non-maskable interrupt
ISR_NOERRCODE 3   // Breakpoint
ISR_NOERRCODE 4   // Overflow
ISR_NOERRCODE 5   // Bound Range Exceeded
ISR_NOERRCODE 6   // Invalid Opcode
ISR_NOERRCODE 7   // Device Not Available
ISR_ERRCODE   8   // Double Fault
ISR_NOERRCODE 9   // Coprocessor Segment Overrun
ISR_ERRCODE   10  // Invalid TSS
ISR_ERRCODE   11  // Segment Not Present
ISR_ERRCODE   12  // Stack-Segment Fault
ISR_ERRCODE   13  // General Protection Fault
ISR_ERRCODE   14  // Page Fault
ISR_NOERRCODE 15  // Reserved
ISR_NOERRCODE 16  // x87 Floating-Point Exception
ISR_ERRCODE   17  // Alignment Check
ISR_NOERRCODE 18  // Machine Check
ISR_NOERRCODE 19  // SIMD Floating-Point Exception
ISR_NOERRCODE 20  // Reserved
ISR_NOERRCODE 21  // Reserved
ISR_NOERRCODE 22  // Reserved
ISR_NOERRCODE 23  // Reserved
ISR_NOERRCODE 24  // Reserved
ISR_NOERRCODE 25  // Reserved
ISR_NOERRCODE 26  // Reserved
ISR_NOERRCODE 27  // Reserved
ISR_NOERRCODE 28  // Reserved
ISR_NOERRCODE 29  // Reserved
ISR_ERRCODE   30  // Security Exception
ISR_NOERRCODE 31  // Reserved

// Define IRQs 0-15 (IRQ 0-7 mapped to IDT entries 32-39, IRQ 8-15 mapped to 40-47)
IRQ 0, 32   // Programmable Interrupt Timer
IRQ 1, 33   // Keyboard
IRQ 2, 34   // Cascade for 8259A Slave controller
IRQ 3, 35   // COM2
IRQ 4, 36   // COM1
IRQ 5, 37   // LPT2
IRQ 6, 38   // Floppy Disk
IRQ 7, 39   // LPT1 / Spurious interrupt
IRQ 8, 40   // CMOS Real Time Clock
IRQ 9, 41   // Free for peripherals
IRQ 10, 42  // Free for peripherals
IRQ 11, 43  // Free for peripherals
IRQ 12, 44  // PS/2 Mouse
IRQ 13, 45  // FPU / Coprocessor / Inter-processor
IRQ 14, 46  // Primary ATA Hard Disk
IRQ 15, 47  // Secondary ATA Hard Disk

// Define system call interrupt
ISR_NOERRCODE 128  // System call via int 0x80

// Common ISR stub
isr_common_stub:
    // Save all registers
    pushal
    
    // Save segment registers
    pushl %ds
    pushl %es
    pushl %fs
    pushl %gs
    
    // Set up kernel data segment
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    
    // Call C handler with pointer to stack frame as parameter
    pushl %esp
    call isr_handler
    addl $4, %esp
    
    // Restore segment registers
    popl %gs
    popl %fs
    popl %es
    popl %ds
    
    // Restore general purpose registers
    popal
    
    // Cleanup error code and interrupt number
    addl $8, %esp
    
    // Return from interrupt
    iret

// Common IRQ stub - nearly identical to ISR stub but calls different handler
irq_common_stub:
    // Save all registers
    pushal
    
    // Save segment registers
    pushl %ds
    pushl %es
    pushl %fs
    pushl %gs
    
    // Set up kernel data segment
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %fs
    movw %ax, %gs
    
    // Call C handler with pointer to stack frame as parameter
    pushl %esp
    call irq_handler
    addl $4, %esp
    
    // Restore segment registers
    popl %gs
    popl %fs
    popl %es
    popl %ds
    
    // Restore general purpose registers
    popal
    
    // Cleanup error code and interrupt number
    addl $8, %esp
    
    // Return from interrupt
    iret

// Load IDT
.global idt_load
idt_load:
    movl 4(%esp), %eax  // Get pointer to IDT
    lidt (%eax)         // Load IDT
    sti                 // Enable interrupts
    ret
