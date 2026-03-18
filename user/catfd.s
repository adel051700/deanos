.section .text
.global _start
_start:
read_loop:
    movl $5, %eax           /* SYS_read */
    movl $0, %ebx           /* stdin */
    movl $buf, %ecx
    movl $256, %edx
    int $0x80

    cmpl $0, %eax
    jle done

    movl %eax, %edx         /* bytes read */
    movl $1, %eax           /* SYS_write */
    movl $1, %ebx           /* stdout */
    movl $buf, %ecx
    int $0x80
    jmp read_loop

done:
    movl $3, %eax           /* SYS_exit */
    xorl %ebx, %ebx
    int $0x80

.section .bss
.align 16
buf:
    .skip 256

