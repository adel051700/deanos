.section .text
.global _start
_start:
    movl $40, %esi                 /* iteration count */

loop_start:
    cmpl $0, %esi
    je pass

    movl $14, %eax                 /* SYS_fork */
    xorl %ebx, %ebx
    int $0x80

    cmpl $0, %eax
    jl fail
    je child_path

parent_path:
    movl %eax, %edi                /* expected child pid from fork() */

    /* waitpid(expected_pid, &status_word, 0) */
    movl $16, %eax                 /* SYS_waitpid */
    movl %edi, %ebx
    movl $status_word, %ecx
    xorl %edx, %edx
    int $0x80

    cmpl %edi, %eax
    jne fail

    movl status_word, %eax
    cmpl $42, %eax
    jne fail

    subl $1, %esi
    jmp loop_start

child_path:
    movl $3, %eax                  /* SYS_exit */
    movl $42, %ebx
    int $0x80

fail:
    movl $1, %eax                  /* SYS_write */
    movl $1, %ebx
    movl $fail_text, %ecx
    movl $fail_text_len, %edx
    int $0x80

    movl $3, %eax
    movl $1, %ebx
    int $0x80

pass:
    movl $1, %eax                  /* SYS_write */
    movl $1, %ebx
    movl $ok_text, %ecx
    movl $ok_text_len, %edx
    int $0x80

    movl $3, %eax
    xorl %ebx, %ebx
    int $0x80

.section .data
status_word:
    .long 0

.section .rodata
ok_text:
    .ascii "[waitstress] 40x fork/waitpid OK\n"
.set ok_text_len, . - ok_text

fail_text:
    .ascii "[waitstress] FAIL\n"
.set fail_text_len, . - fail_text

