.section .text
.global _start
_start:
    movl $14, %eax          /* SYS_fork */
    xorl %ebx, %ebx
    int $0x80

    cmpl $0, %eax
    jl fail_msg
    je child_path

parent_path:
    /* waitpid(-1, &status_word, 0) */
    movl $16, %eax          /* SYS_waitpid */
    movl $-1, %ebx
    movl $status_word, %ecx
    xorl %edx, %edx
    int $0x80

    cmpl $0, %eax
    jle fail_msg

    movl status_word, %eax
    cmpl $42, %eax
    jne fail_msg

    movl $1, %eax           /* SYS_write */
    movl $1, %ebx
    movl $ok_text, %ecx
    movl $ok_text_len, %edx
    int $0x80

    movl $3, %eax           /* SYS_exit */
    xorl %ebx, %ebx
    int $0x80

child_path:
    movl $3, %eax           /* SYS_exit */
    movl $42, %ebx
    int $0x80

fail_msg:
    movl $1, %eax
    movl $1, %ebx
    movl $fail_text, %ecx
    movl $fail_text_len, %edx
    int $0x80

    movl $3, %eax
    movl $1, %ebx
    int $0x80

.section .data
status_word:
    .long 0

.section .rodata
ok_text:
    .ascii "[waittest] waitpid status OK\n"
.set ok_text_len, . - ok_text

fail_text:
    .ascii "[waittest] waitpid failed\n"
.set fail_text_len, . - fail_text

