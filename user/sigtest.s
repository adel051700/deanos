.section .text
.global _start

/* syscall numbers */
.set SYS_write,     1
.set SYS_exit,      3
.set SYS_sleep_ms, 10
.set SYS_getpid,   11
.set SYS_kill,     13
.set SYS_signal,   25
.set SYS_sigreturn,26

.set SIGINT,  2
.set SIGTERM, 15
.set SIG_IGN, 1

_start:
    movl $0, handled_flag

    movl $SYS_write, %eax
    movl $1, %ebx
    movl $msg_boot, %ecx
    movl $msg_boot_len, %edx
    int $0x80

    /* Install SIGTERM handler. */
    movl $SYS_signal, %eax
    movl $SIGTERM, %ebx
    movl $sigterm_handler, %ecx
    movl $sig_restorer, %edx
    int $0x80
    cmpl $0, %eax
    jl fail

    movl $SYS_getpid, %eax
    xorl %ebx, %ebx
    int $0x80
    cmpl $0, %eax
    jle fail
    movl %eax, self_pid

    movl $SYS_write, %eax
    movl $1, %ebx
    movl $msg_send_term, %ecx
    movl $msg_send_term_len, %edx
    int $0x80

    /* Trigger handler with SIGTERM to self. */
    movl $SYS_kill, %eax
    movl self_pid, %ebx
    movl $SIGTERM, %ecx
    int $0x80
    cmpl $0, %eax
    jl fail

    movl handled_flag, %eax
    cmpl $1, %eax
    jne fail

    /* Ignore SIGINT and verify we survive. */
    movl $SYS_signal, %eax
    movl $SIGINT, %ebx
    movl $SIG_IGN, %ecx
    xorl %edx, %edx
    int $0x80
    cmpl $0, %eax
    jl fail

    movl $SYS_kill, %eax
    movl self_pid, %ebx
    movl $SIGINT, %ecx
    int $0x80
    cmpl $0, %eax
    jl fail

    movl $SYS_sleep_ms, %eax
    movl $20, %ebx
    xorl %ecx, %ecx
    xorl %edx, %edx
    int $0x80

    movl $SYS_write, %eax
    movl $1, %ebx
    movl $msg_ok, %ecx
    movl $msg_ok_len, %edx
    int $0x80

    movl $SYS_exit, %eax
    xorl %ebx, %ebx
    int $0x80

fail:
    movl $SYS_write, %eax
    movl $1, %ebx
    movl $msg_fail, %ecx
    movl $msg_fail_len, %edx
    int $0x80

    movl $SYS_exit, %eax
    movl $1, %ebx
    int $0x80

sigterm_handler:
    movl $1, handled_flag

    movl $SYS_write, %eax
    movl $1, %ebx
    movl $msg_handler, %ecx
    movl $msg_handler_len, %edx
    int $0x80

    ret

sig_restorer:
    addl $4, %esp
    movl $SYS_sigreturn, %eax
    int $0x80
1:
    hlt
    jmp 1b

.section .data
handled_flag:
    .long 0
self_pid:
    .long 0

.section .rodata
msg_boot:
    .ascii "[sigtest] install TERM handler\n"
.set msg_boot_len, . - msg_boot

msg_send_term:
    .ascii "[sigtest] send SIGTERM to self\n"
.set msg_send_term_len, . - msg_send_term

msg_handler:
    .ascii "[sigtest] SIGTERM handler invoked\n"
.set msg_handler_len, . - msg_handler

msg_ok:
    .ascii "[sigtest] PASS\n"
.set msg_ok_len, . - msg_ok

msg_fail:
    .ascii "[sigtest] FAIL\n"
.set msg_fail_len, . - msg_fail

