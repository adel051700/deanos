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
    /* waitpid(-1, &status_word, 0) */
    movl $16, %eax          /* SYS_waitpid */
    movl $-1, %ebx
    movl $status_word, %ecx
    xorl %edx, %edx
    int $0x80
    cmpl $0, %eax
    jle parent_fail

    movl status_word, %eax
    cmpl $7, %eax
    jne parent_fail

    movl shared_word, %eax
    cmpl $0x11111111, %eax
    jne parent_fail

    movl $1, %eax           /* SYS_write */
    movl $1, %ebx           /* stdout */
    movl $pass_msg, %ecx
    movl $pass_msg_len, %edx
    int $0x80

    movl $3, %eax           /* SYS_exit */
    xorl %ebx, %ebx
    int $0x80

child_path:
    movl shared_word, %eax
    cmpl $0x11111111, %eax
    jne child_fail

    /* Child write should fault-copy this page, not alter parent mapping. */
    movl $0x22222222, shared_word

    movl $1, %eax           /* SYS_write */
    movl $1, %ebx           /* stdout */
    movl $child_msg, %ecx
    movl $child_msg_len, %edx
    int $0x80

    movl $3, %eax           /* SYS_exit */
    movl $7, %ebx
    int $0x80

child_fail:
    movl $3, %eax
    movl $9, %ebx
    int $0x80

parent_fail:
    movl $1, %eax
    movl $1, %ebx
    movl $fail_msg, %ecx
    movl $fail_msg_len, %edx
    int $0x80

    movl $3, %eax
    movl $1, %ebx
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
child_msg:
    .ascii "[forktest] child wrote private copy\n"
.set child_msg_len, . - child_msg

pass_msg:
    .ascii "[forktest] COW PASS parent value preserved\n"
.set pass_msg_len, . - pass_msg

fail_msg:
    .ascii "[forktest] COW FAIL\n"
.set fail_msg_len, . - fail_msg

err_msg:
    .ascii "[forktest] fork failed\n"
.set err_msg_len, . - err_msg

.section .data
shared_word:
    .long 0x11111111
status_word:
    .long 0

