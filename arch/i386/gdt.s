.section .text
.global gdt_flush    // Make gdt_flush accessible from C code

// gdt_flush - Load the GDT and update segment registers
// Argument: Pointer to GDT descriptor passed on stack
gdt_flush:
    mov 4(%esp), %eax   // Get pointer to GDT (passed as parameter)
    lgdt (%eax)         // Load the GDT

    // Update all segment registers to use the new GDT
    mov $0x10, %ax      // 0x10 is the offset in the GDT to our kernel data segment
    mov %ax, %ds        // DS = kernel data segment
    mov %ax, %es        // ES = kernel data segment
    mov %ax, %fs        // FS = kernel data segment
    mov %ax, %gs        // GS = kernel data segment
    mov %ax, %ss        // SS = kernel data segment
    
    // Perform a far jump to update the code segment register (CS)
    // 0x08 is the offset to our kernel code segment
    ljmp $0x08, $flush  // Far jump to set CS register
flush:
    ret                 // Return to C code