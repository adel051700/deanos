.section .text
.global _start
_start:
    /* fork() */
    movl $14, %eax          /* SYS_fork */
    xorl %ebx, %ebx
    int $0x80

    cmpl $0, %eax
    jl fork_error
    je child_path

parent_path:
    movl $1, %eax           /* SYS_write */
    movl $1, %ebx           /* stdout */
    movl $parent_msg, %ecx
    movl $parent_msg_len, %edx
    int $0x80

    movl $3, %eax           /* SYS_exit */
    xorl %ebx, %ebx
    int $0x80

child_path:
    movl $1, %eax           /* SYS_write */
    movl $1, %ebx           /* stdout */
    movl $child_msg, %ecx
    movl $child_msg_len, %edx
    int $0x80

    movl $3, %eax           /* SYS_exit */
    xorl %ebx, %ebx
    int $0x80

fork_error:
    movl $1, %eax           /* SYS_write */
    movl $1, %ebx
    movl $err_msg, %ecx
    movl $err_msg_len, %edx
    int $0x80

    movl $3, %eax
    movl $1, %ebx
    int $0x80

.section .rodata
parent_msg:
    .ascii "[forktest] parent path\n"
.set parent_msg_len, . - parent_msg

child_msg:
    .ascii "[forktest] child path\n"
.set child_msg_len, . - child_msg

err_msg:
    .ascii "[forktest] fork failed\n"
.set err_msg_len, . - err_msg

