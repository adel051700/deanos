/* Multiboot2 header */
.set ALIGN,    1<<0
.set MEMINFO,  1<<1
.set FLAGS,    ALIGN | MEMINFO
.set MAGIC,    0xE85250D6          /* Multiboot2 magic number */
.set ARCH,     0                   /* Protected mode i386 */
.set HEADER_LEN, header_end - header_start
.set CHECKSUM, -(MAGIC + ARCH + HEADER_LEN)

/* Multiboot2 header */
.section .multiboot
.align 8
header_start:
.long MAGIC                        /* Magic number */
.long ARCH                         /* Architecture: i386 (protected mode) */
.long HEADER_LEN                   /* Header length */
.long CHECKSUM                     /* Checksum */

/* Add framebuffer tag */
.align 8
.word 5                           /* Type: framebuffer */
.word 0                           /* Flags */
.long 20                          /* Size */
.long 1024                       /* Width */
.long 768                         /* Height */
.long 32                          /* Depth */

/* Required end tag */
.align 8
.word 0    /* Type - end tag */
.word 0    /* Flags */
.long 8    /* Size */
header_end:

    /* Define global variables to store multiboot info */
.section .data
.global multiboot_magic
multiboot_magic:
    .long 0

.global multiboot_info_addr
multiboot_info_addr:
    .long 0

/* Stack setup */
.section .bss
.align 16
stack_bottom:
.skip 16384 # 16 KiB
stack_top:

/* Entry point */
.section .text
.global _start
.type _start, @function
_start:
    /* Disable interrupts until we're ready */
    cli
    
    /* Set up the stack */
    mov $stack_top, %esp

    /* Save multiboot info to global variables */
    mov %eax, multiboot_magic
    mov %ebx, multiboot_info_addr

    /* Call global constructors */
    call call_constructors

    /* Call kernel main with no arguments */
    call kernel_main

    /* Infinite loop */
    cli
1:  hlt
    jmp 1b

.size _start, . - _start
