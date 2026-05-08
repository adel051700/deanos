.section .text
.global _start

/* syscall numbers */
.set SYS_write,     1
.set SYS_time,      2
.set SYS_open,      4
.set SYS_pipe,     18
.set SYS_sigaction,24
.set SYS_mmap,     27
.set SYS_poll,     49
.set SYS_exit,      3

/* invalid user pointer in kernel-space range */
.set BAD_PTR, 0xFFFFF000

_start:
    movl $SYS_write, %eax
    movl $1, %ebx
    movl $msg_boot, %ecx
    movl $msg_boot_len, %edx
    int $0x80

    /* write(1, BAD_PTR, 4) -> fail */
    movl $SYS_write, %eax
    movl $1, %ebx
    movl $BAD_PTR, %ecx
    movl $4, %edx
    int $0x80
    test %eax, %eax
    jns fail

    /* open(BAD_PTR, 0) -> fail */
    movl $SYS_open, %eax
    movl $BAD_PTR, %ebx
    xorl %ecx, %ecx
    int $0x80
    test %eax, %eax
    jns fail

    /* time((uint32_t*)BAD_PTR) -> fail */
    movl $SYS_time, %eax
    movl $BAD_PTR, %ebx
    int $0x80
    test %eax, %eax
    jns fail

    /* pipe((int*)BAD_PTR) -> fail */
    movl $SYS_pipe, %eax
    movl $BAD_PTR, %ebx
    int $0x80
    test %eax, %eax
    jns fail

    /* sigaction(SIGINT=2, BAD_PTR, NULL) -> fail */
    movl $SYS_sigaction, %eax
    movl $2, %ebx
    movl $BAD_PTR, %ecx
    xorl %edx, %edx
    int $0x80
    test %eax, %eax
    jns fail

    /* mmap(BAD_PTR) -> fail */
    movl $SYS_mmap, %eax
    movl $BAD_PTR, %ebx
    int $0x80
    test %eax, %eax
    jns fail

    /* poll(BAD_PTR) -> fail */
    movl $SYS_poll, %eax
    movl $BAD_PTR, %ebx
    int $0x80
    test %eax, %eax
    jns fail

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

.section .rodata
msg_boot:
    .ascii "[negptrtest] syscall pointer hardening checks\n"
.set msg_boot_len, . - msg_boot

msg_ok:
    .ascii "[negptrtest] PASS\n"
.set msg_ok_len, . - msg_ok

msg_fail:
    .ascii "[negptrtest] FAIL\n"
.set msg_fail_len, . - msg_fail

