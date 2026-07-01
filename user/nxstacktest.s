.section .text
.global _start

/* syscall numbers */
.set SYS_write,   1
.set SYS_exit,    3
.set SYS_fork,   14
.set SYS_waitpid,16

/* Kernel status for a task killed by a fault: 128 + SIGSEGV(11). */
.set FAULT_STATUS, 139
/* Sentinel the child exits with if stack code actually ran (NX failed). */
.set EXECUTED_STATUS, 7

_start:
    movl $SYS_fork, %eax
    xorl %ebx, %ebx
    int $0x80
    cmpl $0, %eax
    jl fail            /* fork failed */
    je child_path

parent_path:
    /* waitpid(-1, &status_word, 0) */
    movl $SYS_waitpid, %eax
    movl $-1, %ebx
    movl $status_word, %ecx
    xorl %edx, %edx
    int $0x80
    cmpl $0, %eax
    jle fail

    movl status_word, %eax
    cmpl $EXECUTED_STATUS, %eax
    je fail            /* child executed code on its stack -> NX not enforced */
    cmpl $FAULT_STATUS, %eax
    jne fail           /* child died some other way -> not a clean NX kill */

    /* PASS: child was killed trying to execute its stack. */
    movl $SYS_write, %eax
    movl $1, %ebx
    movl $msg_ok, %ecx
    movl $msg_ok_len, %edx
    int $0x80
    movl $SYS_exit, %eax
    xorl %ebx, %ebx
    int $0x80

child_path:
    /* Announce, then try to execute an instruction placed on the stack. */
    movl $SYS_write, %eax
    movl $1, %ebx
    movl $msg_child, %ecx
    movl $msg_child_len, %edx
    int $0x80

    subl $16, %esp
    movb $0xC3, (%esp)     /* a lone 'ret' opcode on the stack */
    movl %esp, %eax
    call *%eax             /* instruction fetch from the stack: #GP if NX works */

    /* Only reached if the stack was executable (the ret returned here). */
    addl $16, %esp
    movl $SYS_exit, %eax
    movl $EXECUTED_STATUS, %ebx
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

.section .data
status_word:
    .long 0

.section .rodata
msg_child:
    .ascii "[nxstacktest] child jumping to stack\n"
.set msg_child_len, . - msg_child

msg_ok:
    .ascii "[nxstacktest] PASS (stack not executable)\n"
.set msg_ok_len, . - msg_ok

msg_fail:
    .ascii "[nxstacktest] FAIL\n"
.set msg_fail_len, . - msg_fail
