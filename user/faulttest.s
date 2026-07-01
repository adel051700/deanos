.section .text
.global _start

/* syscall numbers */
.set SYS_write, 1
.set SYS_time,  2
.set SYS_exit,  3
.set SYS_open,  4

/* A kernel-only virtual address: the kernel heap base is mapped present but
 * supervisor-only (no PTE_U), so any syscall handed this pointer must be
 * rejected with -EFAULT rather than letting the kernel touch it. */
.set KADDR, 0x40000000
.set EFAULT_U, 0xFFFFFFF2   /* (uint32_t)(-14) */

_start:
    /* t1: write() with a kernel-address buffer must return -EFAULT. */
    movl $SYS_write, %eax
    movl $1, %ebx
    movl $KADDR, %ecx
    movl $8, %edx
    int $0x80
    cmpl $EFAULT_U, %eax
    jne fail

    /* t2: a normal write() must still succeed (regression). */
    movl $SYS_write, %eax
    movl $1, %ebx
    movl $msg_probe, %ecx
    movl $msg_probe_len, %edx
    int $0x80
    cmpl $0, %eax
    jle fail

    /* t3: open() with a kernel-address path must return -EFAULT
     * (the path is copied in and validated before the VFS strlen's it). */
    movl $SYS_open, %eax
    movl $KADDR, %ebx
    xorl %ecx, %ecx
    int $0x80
    cmpl $EFAULT_U, %eax
    jne fail

    /* t4: time() with a kernel-address out pointer must return -EFAULT. */
    movl $SYS_time, %eax
    movl $KADDR, %ebx
    int $0x80
    cmpl $EFAULT_U, %eax
    jne fail

    /* PASS */
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
msg_probe:
    .ascii "[faulttest] probe write ok\n"
.set msg_probe_len, . - msg_probe

msg_ok:
    .ascii "[faulttest] PASS\n"
.set msg_ok_len, . - msg_ok

msg_fail:
    .ascii "[faulttest] FAIL\n"
.set msg_fail_len, . - msg_fail
