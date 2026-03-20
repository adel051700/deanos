#ifndef _KERNEL_SYSCALL_H
#define _KERNEL_SYSCALL_H

#include <stdint.h>

/* Syscall numbers */
enum {
    SYS_write  = 1,
    SYS_time   = 2,
    SYS_exit   = 3,
    SYS_open   = 4,
    SYS_read   = 5,
    SYS_close  = 6,
    SYS_fstat  = 7,
    SYS_mkdir  = 8,
    SYS_yield  = 9,
    SYS_sleep_ms = 10,
    SYS_getpid = 11,
    SYS_getppid = 12,
    SYS_kill = 13,
    SYS_fork = 14,
    SYS_execve = 15,
    SYS_waitpid = 16,
    SYS_fcntl = 17,
    SYS_pipe = 18,
    SYS_setpgid = 19,
    SYS_getpgrp = 20,
    SYS_setsid = 21,
    SYS_tcsetpgrp = 22,
    SYS_tcgetpgrp = 23,
    SYS_sigaction = 24,
    SYS_signal = 25,
    SYS_sigreturn = 26,
};

/* Install syscall handlers on vectors 0x80 and 0x81 */
void syscall_initialize(void);

#endif