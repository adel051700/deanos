.section .text

// Declare the C handler
.extern isr_handler

// Macros
.macro ISR_NOERRCODE num
.global isr\num
isr\num:
    pushl $0            // err_code
    pushl $\num         // int_no
    jmp isr_common_stub
.endm

.macro ISR_ERRCODE num
.global isr\num
isr\num:
    pushl $\num           // int_no
    jmp isr_common_stub   // CPU already pushed err_code
.endm

.macro IRQ num, vec
.global irq\num
irq\num:
    pushl $0            // err_code
    pushl $\vec         // int_no
    jmp isr_common_stub
.endm

// Exceptions 0–31
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
ISR_NOERRCODE 30
ISR_NOERRCODE 31

// Syscall (optional)
.global isr128
isr128:
    pushl $0
    pushl $128
    jmp isr_common_stub

// Add a second syscall gate (int 0x81)
.global isr129
isr129:
    pushl $0
    pushl $129
    jmp isr_common_stub

// IRQs (PIC remapped to 32–47)
IRQ 0, 32
IRQ 1, 33
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

// Common stub
.global isr_common_stub
isr_common_stub:
    // At entry (top of stack towards lower addresses):
    //   [int_no] [err_code] [eip] [cs] [eflags] [(useresp) (ss)]

    pusha                     // push eax,ecx,edx,ebx,esp,ebp,esi,edi

    // Save original DS
    mov %ds, %ax
    pushl %eax

    // Switch to kernel data segment (0x10) for handler
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    // Pass pointer to struct registers (starts at saved DS we just pushed)
    pushl %esp
    call isr_handler
    add $4, %esp

    // Restore original DS
    popl %eax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    // (Removed scheduler stack switch)

    popa
    add $8, %esp          // discard int_no, err_code
    sti
    iret

// Load IDT
.global idt_load
idt_load:
    movl 4(%esp), %eax  // Get pointer to IDT
    lidt (%eax)         // Load IDT
    // DO NOT enable interrupts here - let kernel_main do it after handlers are registered
    ret
