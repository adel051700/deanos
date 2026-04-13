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
    SYS_mmap = 27,
    SYS_munmap = 28,
    SYS_shm_open = 29,
    SYS_shm_unlink = 30,
    SYS_socket = 31,
    SYS_socket_close = 32,
    SYS_bind = 33,
    SYS_sendto = 34,
    SYS_recvfrom = 35,
    SYS_getuid = 36,
    SYS_getgid = 37,
    SYS_chmod = 38,
    SYS_chown = 39,
};

#define KSOCK_AF_INET      2u
#define KSOCK_SOCK_DGRAM   2u
#define KSOCK_IPPROTO_UDP 17u

/* mmap protection flags */
#define MMAP_PROT_READ   0x1u
#define MMAP_PROT_WRITE  0x2u
#define MMAP_PROT_EXEC   0x4u

/* mmap mapping flags */
#define MMAP_MAP_SHARED     0x01u
#define MMAP_MAP_PRIVATE    0x02u
#define MMAP_MAP_FIXED      0x10u
#define MMAP_MAP_ANONYMOUS  0x20u
#define MMAP_MAP_SHM        0x40u

/* shm_open flags */
#define SHM_OPEN_CREATE     0x1u
#define SHM_OPEN_EXCL       0x2u

typedef struct syscall_mmap_args {
    uintptr_t addr;
    uint32_t  length;
    uint32_t  prot;
    uint32_t  flags;
    int32_t   fd;
    uint32_t  offset;
} syscall_mmap_args_t;

typedef struct syscall_bind_args {
    int32_t socket_id;
    uint16_t local_port;
} syscall_bind_args_t;

typedef struct syscall_sendto_args {
    int32_t socket_id;
    uint8_t dst_ip[4];
    uint16_t dst_port;
    const void* payload;
    uint32_t payload_len;
} syscall_sendto_args_t;

typedef struct syscall_recvfrom_args {
    int32_t socket_id;
    void* out_payload;
    uint32_t payload_capacity;
    uint16_t* out_payload_len;
    uint8_t* out_from_ip;
    uint16_t* out_from_port;
    uint32_t timeout_ms;
} syscall_recvfrom_args_t;

/* Install syscall handlers on vectors 0x80 and 0x81 */
void syscall_initialize(void);

#endif