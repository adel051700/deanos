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
    SYS_connect = 40,
    SYS_listen = 41,
    SYS_accept = 42,
    SYS_send = 43,
    SYS_recv = 44,
    SYS_resolve = 45,
    SYS_shutdown = 46,
    SYS_setsockopt = 47,
    SYS_getsockopt = 48,
    SYS_poll = 49,
};

#define KPOLLIN   0x0001
#define KPOLLOUT  0x0004
#define KPOLLERR  0x0008
#define KPOLLHUP  0x0010
#define KPOLLNVAL 0x0020

#define KSHUT_RD   0
#define KSHUT_WR   1
#define KSHUT_RDWR 2

#define KSOL_SOCKET 1
#define KSOL_TCP    6

/* Socket-level options */
#define KSO_REUSEADDR 2
#define KSO_KEEPALIVE 9
#define KSO_RCVTIMEO  20
#define KSO_SNDTIMEO  21

/* TCP-level options */
#define KTCP_NODELAY 1

#define KSOCK_AF_INET      2u
#define KSOCK_SOCK_STREAM  1u
#define KSOCK_SOCK_DGRAM   2u
#define KSOCK_IPPROTO_TCP   6u
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

typedef struct syscall_connect_args {
    int32_t socket_fd;
    uint8_t dst_ip[4];
    uint16_t dst_port;
    uint32_t timeout_ms;
} syscall_connect_args_t;

typedef struct syscall_listen_args {
    int32_t socket_fd;
    uint16_t backlog;
} syscall_listen_args_t;

typedef struct syscall_accept_args {
    int32_t socket_fd;
    uint32_t timeout_ms;
    uint8_t* out_from_ip;
    uint16_t* out_from_port;
} syscall_accept_args_t;

typedef struct syscall_send_args {
    int32_t socket_fd;
    const void* payload;
    uint32_t payload_len;
    uint32_t timeout_ms;
} syscall_send_args_t;

typedef struct syscall_recv_args {
    int32_t socket_fd;
    void* out_payload;
    uint32_t payload_capacity;
    uint16_t* out_payload_len;
    uint32_t timeout_ms;
} syscall_recv_args_t;

typedef struct syscall_resolve_args {
    const char* hostname;
    uint8_t* out_ip;
    const uint8_t* dns_server_ip; /* may be NULL for defaults */
    uint32_t timeout_ms;
} syscall_resolve_args_t;

typedef struct syscall_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
} syscall_pollfd_t;

typedef struct syscall_poll_args {
    syscall_pollfd_t* fds;
    uint32_t nfds;
    int32_t timeout_ms;  /* <0 = infinite, 0 = non-blocking */
} syscall_poll_args_t;

typedef struct syscall_sockopt_args {
    int32_t socket_fd;
    int32_t level;
    int32_t optname;
    void* optval;
    uint32_t* optlen;
} syscall_sockopt_args_t;

/* Install syscall handlers on vectors 0x80 and 0x81 */
void syscall_initialize(void);

#endif