.section .text
.global _start
_start:
    xorl %esi, %esi                /* spawned count */

spawn_loop:
    cmpl $8, %esi
    je reap_init

    movl $14, %eax                 /* SYS_fork */
    xorl %ebx, %ebx
    int $0x80

    cmpl $0, %eax
    jl fail
    je child_path

    incl %esi
    jmp spawn_loop

child_path:
    /* Derive status from actual child PID so parent can validate by waited PID. */
    movl $11, %eax                 /* SYS_getpid */
    xorl %ebx, %ebx
    int $0x80
    movl %eax, %ebx
    andl $0x7F, %ebx

    movl $3, %eax                  /* SYS_exit */
    int $0x80

reap_init:
    xorl %esi, %esi                /* reaped count */

reap_loop:
    cmpl $8, %esi
    je verify_done

    movl $16, %eax                 /* SYS_waitpid */
    movl $-1, %ebx                 /* any child */
    movl $status_word, %ecx
    xorl %edx, %edx
    int $0x80

    cmpl $0, %eax
    jle fail

    movl %eax, %edx                /* edx = waited child pid */
    andl $0x7F, %edx               /* expected status */
    movl status_word, %ecx
    cmpl %edx, %ecx
    jne fail

    incl %esi
    jmp reap_loop

verify_done:
pass:
    movl $1, %eax                  /* SYS_write */
    movl $1, %ebx
    movl $ok_text, %ecx
    movl $ok_text_len, %edx
    int $0x80

    movl $3, %eax
    xorl %ebx, %ebx
    int $0x80

fail:
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
    .ascii "[waitstressbg] 8-way waitpid(any) OK\n"
.set ok_text_len, . - ok_text

fail_text:
    .ascii "[waitstressbg] FAIL\n"
.set fail_text_len, . - fail_text

