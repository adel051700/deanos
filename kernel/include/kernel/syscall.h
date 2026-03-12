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
};

/* Install syscall handlers on vectors 0x80 and 0x81 */
void syscall_initialize(void);

#endif