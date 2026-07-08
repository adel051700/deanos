.section .text
.global _start
_start:
    /* announce current PID before execve */
    movl $11, %eax          /* SYS_getpid */
    xorl %ebx, %ebx
    int $0x80
    movl %eax, before_pid

    movl $1, %eax           /* SYS_write */
    movl $1, %ebx
    movl $msg_before, %ecx
    movl $msg_before_len, %edx
    int $0x80

    /* execve("/bin/test/hello") should replace this process image, with no args */
    movl $15, %eax          /* SYS_execve */
    movl $path_hello, %ebx
    xorl %ecx, %ecx         /* argv = NULL */
    xorl %edx, %edx         /* argc = 0 */
    int $0x80

    /* If execve returns, it failed */
    movl $1, %eax           /* SYS_write */
    movl $1, %ebx
    movl $msg_fail, %ecx
    movl $msg_fail_len, %edx
    int $0x80

    movl $3, %eax           /* SYS_exit */
    movl $1, %ebx
    int $0x80

.section .data
before_pid:
    .long 0

.section .rodata
path_hello:
    .asciz "/bin/test/hello"

msg_before:
    .ascii "[execvetest] replacing image with /bin/test/hello\n"
.set msg_before_len, . - msg_before

msg_fail:
    .ascii "[execvetest] execve failed\n"
.set msg_fail_len, . - msg_fail

