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

// Define ISRs for exceptions 0-31
ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE   8
ISR_NOERRCODE 9
ISR_ERRCODE   10
ISR_ERRCODE   11
ISR_ERRCODE   12
ISR_ERRCODE   13
ISR_ERRCODE   14
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_ERRCODE   17
ISR_NOERRCODE 18
ISR_NOERRCODE 19
ISR_NOERRCODE 20
ISR_NOERRCODE 21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29
ISR_ERRCODE   30
ISR_NOERRCODE 31

// Define IRQs 0-15
IRQ 0, 32
IRQ 1, 33  // Keyboard IRQ
IRQ 2, 34
IRQ 3, 35
IRQ 4, 36
IRQ 5, 37
IRQ 6, 38
IRQ 7, 39
IRQ 8, 40
IRQ 9, 41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

// Common ISR stub
isr_common_stub:
    // Save all registers
    pushal
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
    
    // Call C handler
    pushl %esp
    call isr_handler
    addl $4, %esp
    
    // Restore registers
    popl %gs
    popl %fs
    popl %es
    popl %ds
    popal
    addl $8, %esp    // Clean up error code and ISR number
    iret            // Return from interrupt

// Common IRQ stub
irq_common_stub:
    // Save all registers
    pushal
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
    
    // Call C handler
    pushl %esp
    call irq_handler
    addl $4, %esp
    
    // Restore registers
    popl %gs
    popl %fs
    popl %es
    popl %ds
    popal
    addl $8, %esp    // Clean up error code and IRQ number
    iret            // Return from interrupt

// Load IDT
.global idt_load
idt_load:
    movl 4(%esp), %eax  // Get pointer to IDT
    lidt (%eax)         // Load IDT
    ret
