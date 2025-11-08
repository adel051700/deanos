// Multiboot2 constants
.set MULTIBOOT2_MAGIC,              0xE85250D6
.set MULTIBOOT2_ARCHITECTURE,       0           // 0 = i386
.set MULTIBOOT2_HEADER_LENGTH,      (multiboot_header_end - multiboot_header)
.set MULTIBOOT2_CHECKSUM,           -(MULTIBOOT2_MAGIC + MULTIBOOT2_ARCHITECTURE + MULTIBOOT2_HEADER_LENGTH)

// Multiboot2 header - must be in first 32KB of kernel
.section .multiboot
.align 8
multiboot_header:
    .long MULTIBOOT2_MAGIC
    .long MULTIBOOT2_ARCHITECTURE
    .long MULTIBOOT2_HEADER_LENGTH
    .long MULTIBOOT2_CHECKSUM
    
    // Framebuffer tag - request specific video mode
    .align 8
framebuffer_tag_start:
    .short 5                        // Type: framebuffer
    .short 0                        // Flags
    .long framebuffer_tag_end - framebuffer_tag_start
    .long 1024                      // Width
    .long 768                       // Height
    .long 32                        // Depth (bits per pixel)
framebuffer_tag_end:
    
    // End tag - required
    .align 8
    .short 0                        // Type: end
    .short 0                        // Flags
    .long 8                         // Size
multiboot_header_end:

// Stack setup
.section .bss
.align 16
stack_bottom:
.skip 16384                         // 16 KB stack
stack_top:

// Export symbols for C code
.section .data
.global multiboot_info_addr
.global multiboot_magic

multiboot_info_addr:
    .long 0

multiboot_magic:
    .long 0

// Kernel entry point
.section .text
.global _start
.type _start, @function

_start:
    // Bootloader puts multiboot info address in EBX and magic in EAX
    // Save them before we do anything else
    movl %eax, multiboot_magic
    movl %ebx, multiboot_info_addr
    
    // Set up the stack
    movl $stack_top, %esp
    
    // Reset EFLAGS
    pushl $0
    popf
    
    // Call global constructors (hardware init, framebuffer setup, etc.)
    call call_constructors
    
    // Call the kernel main function
    call kernel_main
    
    // If kernel_main returns, hang the system
    cli
hang:
    hlt
    jmp hang

.size _start, . - _start