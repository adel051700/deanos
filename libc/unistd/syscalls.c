#include <dirent.h>
#include <fcntl.h>
#include <kernel/syscall.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/blk.h>
#include <sys/klog.h>
#include <sys/mman.h>
#include <sys/mouse.h>
#include <sys/net.h>
#include <sys/proc.h>
#include <sys/rtc.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/vmstat.h>
#include <unistd.h>

__attribute__((naked, noreturn)) static void __signal_restorer(void) {
    __asm__ volatile(
        "add $4, %%esp\n"
        "mov %[nr], %%eax\n"
        "int $0x80\n"
        "1: hlt\n"
        "jmp 1b\n"
        :
        : [nr] "i" (SYS_sigreturn)
        : "eax", "memory");
}

static inline long syscall1(unsigned num, unsigned a1) {
    long ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num), "b"(a1) : "memory");
    return ret;
}

static inline long syscall2(unsigned num, unsigned a1, unsigned a2) {
    long ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num), "b"(a1), "c"(a2) : "memory");
    return ret;
}

static inline long syscall3(unsigned num, unsigned a1, unsigned a2, unsigned a3) {
    long ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(num), "b"(a1), "c"(a2), "d"(a3) : "memory");
    return ret;
}

ssize_t read(int fd, void* buf, size_t count) {
    return (ssize_t)syscall3(SYS_read, (unsigned)fd, (unsigned)buf, (unsigned)count);
}

ssize_t write(int fd, const void* buf, size_t count) {
    return (ssize_t)syscall3(SYS_write, (unsigned)fd, (unsigned)buf, (unsigned)count);
}

int close(int fd) {
    return (int)syscall1(SYS_close, (unsigned)fd);
}

int open(const char* path, int flags) {
    return (int)syscall2(SYS_open, (unsigned)path, (unsigned)flags);
}

int fcntl(int fd, int cmd, int arg) {
    return (int)syscall3(SYS_fcntl, (unsigned)fd, (unsigned)cmd, (unsigned)arg);
}

int fstat(int fd, struct stat* st) {
    return (int)syscall2(SYS_fstat, (unsigned)fd, (unsigned)st);
}

int mkdir(const char* path) {
    return (int)syscall1(SYS_mkdir, (unsigned)path);
}

int sched_yield(void) {
    return (int)syscall1(SYS_yield, 0);
}

int sleep_ms(unsigned milliseconds) {
    return (int)syscall1(SYS_sleep_ms, milliseconds);
}

int getpid(void) {
    return (int)syscall1(SYS_getpid, 0);
}

int getppid(void) {
    return (int)syscall1(SYS_getppid, 0);
}

int getuid(void) {
    return (int)syscall1(SYS_getuid, 0);
}

int getgid(void) {
    return (int)syscall1(SYS_getgid, 0);
}

int kill(int pid, int sig) {
    return (int)syscall2(SYS_kill, (unsigned)pid, (unsigned)sig);
}

int sigaction(int signum, const struct sigaction* act, struct sigaction* oldact) {
    struct sigaction tmp;
    const struct sigaction* use_act = act;

    if (act && act->sa_handler != SIG_DFL && act->sa_handler != SIG_IGN && act->sa_restorer == 0) {
        tmp = *act;
        tmp.sa_restorer = __signal_restorer;
        use_act = &tmp;
    }

    return (int)syscall3(SYS_sigaction, (unsigned)signum, (unsigned)use_act, (unsigned)oldact);
}

sighandler_t signal(int signum, sighandler_t handler) {
    return (sighandler_t)syscall3(SYS_signal,
                                  (unsigned)signum,
                                  (unsigned)handler,
                                  (unsigned)__signal_restorer);
}

int fork(void) {
    return (int)syscall1(SYS_fork, 0);
}

int execve(const char* path, char* const argv[]) {
    unsigned argc = 0;
    if (argv) { while (argv[argc]) argc++; }
    return (int)syscall3(SYS_execve, (unsigned)path, (unsigned)argv, argc);
}

int pipe(int pipefd[2]) {
    return (int)syscall1(SYS_pipe, (unsigned)pipefd);
}

int dup2(int oldfd, int newfd) {
    return (int)syscall2(SYS_dup2, (unsigned)oldfd, (unsigned)newfd);
}

int wait(int* status) {
    return (int)syscall3(SYS_waitpid, (unsigned)-1, (unsigned)status, 0u);
}

int waitpid(int pid, int* status, int options) {
    return (int)syscall3(SYS_waitpid, (unsigned)pid, (unsigned)status, (unsigned)options);
}

int setpgid(int pid, int pgid) {
    return (int)syscall2(SYS_setpgid, (unsigned)pid, (unsigned)pgid);
}

int getpgrp(void) {
    return (int)syscall1(SYS_getpgrp, 0);
}

int setsid(void) {
    return (int)syscall1(SYS_setsid, 0);
}

int tcsetpgrp(int fd, int pgrp) {
    return (int)syscall2(SYS_tcsetpgrp, (unsigned)fd, (unsigned)pgrp);
}

int tcgetpgrp(int fd) {
    return (int)syscall1(SYS_tcgetpgrp, (unsigned)fd);
}

int chmod(const char* path, uint16_t mode) {
    return (int)syscall2(SYS_chmod, (unsigned)path, (unsigned)mode);
}

int chown(const char* path, uint32_t uid, uint32_t gid) {
    return (int)syscall3(SYS_chown, (unsigned)path, (unsigned)uid, (unsigned)gid);
}

void* mmap(void* addr, size_t length, int prot, int flags, int fd, unsigned offset) {
    syscall_mmap_args_t args;
    args.addr = (uintptr_t)addr;
    args.length = (uint32_t)length;
    args.prot = (uint32_t)prot;
    args.flags = (uint32_t)flags;
    args.fd = fd;
    args.offset = offset;

    long ret = syscall1(SYS_mmap, (unsigned)&args);
    if (ret < 0) return MAP_FAILED;
    return (void*)(uintptr_t)ret;
}

int munmap(void* addr, size_t length) {
    return (int)syscall2(SYS_munmap, (unsigned)addr, (unsigned)length);
}

int shm_open(int key, uint32_t size, uint32_t flags) {
    return (int)syscall3(SYS_shm_open, (unsigned)key, (unsigned)size, (unsigned)flags);
}

int shm_unlink(int key) {
    return (int)syscall1(SYS_shm_unlink, (unsigned)key);
}

int socket(int domain, int type, int protocol) {
    return (int)syscall3(SYS_socket, (unsigned)domain, (unsigned)type, (unsigned)protocol);
}

int closesocket(int sockfd) {
    return close(sockfd);
}

int bind(int sockfd, const struct sockaddr_in* addr, socklen_t addrlen) {
    syscall_bind_args_t args;
    if (!addr || addrlen != (socklen_t)sizeof(struct sockaddr_in)) return -1;
    args.socket_id = sockfd;
    args.local_port = addr->sin_port;
    return (int)syscall1(SYS_bind, (unsigned)&args);
}

int connect(int sockfd, const struct sockaddr_in* addr, socklen_t addrlen) {
    syscall_connect_args_t args;
    if (!addr || addrlen != (socklen_t)sizeof(struct sockaddr_in)) return -1;
    args.socket_fd = sockfd;
    args.dst_ip[0] = addr->sin_addr.s_addr[0];
    args.dst_ip[1] = addr->sin_addr.s_addr[1];
    args.dst_ip[2] = addr->sin_addr.s_addr[2];
    args.dst_ip[3] = addr->sin_addr.s_addr[3];
    args.dst_port = addr->sin_port;
    return (int)syscall1(SYS_connect, (unsigned)&args);
}

int listen(int sockfd, int backlog) {
    syscall_listen_args_t args;
    if (backlog < 0) backlog = 0;
    args.socket_fd = sockfd;
    args.backlog = (uint16_t)backlog;
    return (int)syscall1(SYS_listen, (unsigned)&args);
}

int accept(int sockfd, struct sockaddr_in* addr, socklen_t* addrlen) {
    syscall_accept_args_t args;
    uint8_t from_ip[4] = {0, 0, 0, 0};
    uint16_t from_port = 0;
    int fd;

    args.socket_fd = sockfd;
    args.out_from_ip = from_ip;
    args.out_from_port = &from_port;
    fd = (int)syscall1(SYS_accept, (unsigned)&args);
    if (fd >= 0 && addr) {
        addr->sin_family = AF_INET;
        addr->sin_port = from_port;
        addr->sin_addr.s_addr[0] = from_ip[0];
        addr->sin_addr.s_addr[1] = from_ip[1];
        addr->sin_addr.s_addr[2] = from_ip[2];
        addr->sin_addr.s_addr[3] = from_ip[3];
        if (addrlen) *addrlen = (socklen_t)sizeof(struct sockaddr_in);
    }
    return fd;
}

ssize_t send(int sockfd, const void* buf, size_t len, int flags) {
    syscall_send_args_t args;
    if (!buf && len > 0u) return -1;
    args.socket_fd = sockfd;
    args.payload = buf;
    args.payload_len = (uint32_t)len;
    args.flags = (uint32_t)flags;
    return (ssize_t)syscall1(SYS_send, (unsigned)&args);
}

ssize_t recv(int sockfd, void* buf, size_t len, int flags) {
    syscall_recv_args_t args;
    uint16_t out_len = 0;
    long ret;
    args.socket_fd = sockfd;
    args.out_payload = buf;
    args.payload_capacity = (uint32_t)len;
    args.out_payload_len = &out_len;
    args.flags = (uint32_t)flags;
    ret = syscall1(SYS_recv, (unsigned)&args);
    if (ret >= 0) return (ssize_t)out_len;
    return (ssize_t)ret;
}

ssize_t sendto(int sockfd, const void* buf, size_t len, int flags, const struct sockaddr_in* dest, socklen_t addrlen) {
    syscall_sendto_args_t args;
    if (!dest || addrlen != (socklen_t)sizeof(struct sockaddr_in) || (!buf && len > 0u)) return -1;
    args.socket_id = sockfd;
    args.dst_ip[0] = dest->sin_addr.s_addr[0];
    args.dst_ip[1] = dest->sin_addr.s_addr[1];
    args.dst_ip[2] = dest->sin_addr.s_addr[2];
    args.dst_ip[3] = dest->sin_addr.s_addr[3];
    args.dst_port = dest->sin_port;
    args.payload = buf;
    args.payload_len = (uint32_t)len;
    args.flags = (uint32_t)flags;
    return (ssize_t)syscall1(SYS_sendto, (unsigned)&args);
}

ssize_t recvfrom(int sockfd, void* buf, size_t len, int flags, struct sockaddr_in* src, socklen_t* addrlen) {
    syscall_recvfrom_args_t args;
    uint16_t out_len = 0;
    uint8_t from_ip[4] = {0, 0, 0, 0};
    uint16_t from_port = 0;
    long ret;

    args.socket_id = sockfd;
    args.out_payload = buf;
    args.payload_capacity = (uint32_t)len;
    args.out_payload_len = &out_len;
    args.out_from_ip = from_ip;
    args.out_from_port = &from_port;
    args.flags = (uint32_t)flags;

    ret = syscall1(SYS_recvfrom, (unsigned)&args);
    if (ret >= 0 && src) {
        src->sin_family = AF_INET;
        src->sin_port = from_port;
        src->sin_addr.s_addr[0] = from_ip[0];
        src->sin_addr.s_addr[1] = from_ip[1];
        src->sin_addr.s_addr[2] = from_ip[2];
        src->sin_addr.s_addr[3] = from_ip[3];
        if (addrlen) *addrlen = (socklen_t)sizeof(struct sockaddr_in);
    }
    return (ssize_t)ret;
}

int poll(struct pollfd* fds, nfds_t nfds, int timeout_ms) {
    syscall_poll_args_t args;
    args.fds = (syscall_pollfd_t*)fds;
    args.nfds = (uint32_t)nfds;
    args.timeout_ms = timeout_ms;
    return (int)syscall1(SYS_poll, (unsigned)&args);
}

int shutdown(int sockfd, int how) {
    return (int)syscall2(SYS_shutdown, (unsigned)sockfd, (unsigned)how);
}

int setsockopt(int sockfd, int level, int optname, const void* optval, socklen_t optlen) {
    syscall_sockopt_args_t args;
    uint32_t len_copy = (uint32_t)optlen;
    args.socket_fd = sockfd;
    args.level = level;
    args.optname = optname;
    args.optval = (void*)optval;
    args.optlen = &len_copy;
    return (int)syscall1(SYS_setsockopt, (unsigned)&args);
}

int getsockopt(int sockfd, int level, int optname, void* optval, socklen_t* optlen) {
    syscall_sockopt_args_t args;
    uint32_t len_copy;
    long rc;
    if (!optlen) return -1;
    len_copy = (uint32_t)*optlen;
    args.socket_fd = sockfd;
    args.level = level;
    args.optname = optname;
    args.optval = optval;
    args.optlen = &len_copy;
    rc = syscall1(SYS_getsockopt, (unsigned)&args);
    *optlen = (socklen_t)len_copy;
    return (int)rc;
}

unsigned sleep(unsigned seconds) {
    (void)syscall1(SYS_sleep_ms, seconds * 1000u);
    return 0;
}

void _exit(int status) {
    (void)syscall1(SYS_exit, (unsigned)status);
    for (;;) {
        __asm__ volatile("hlt");
    }
}

long getrandom(void* buf, unsigned long len, unsigned int flags) {
    return syscall3(SYS_getrandom, (unsigned)buf, (unsigned)len, (unsigned)flags);
}

int get_localtime(struct rtc_time* out) {
    return (int)syscall1(SYS_localtime, (unsigned)out);
}

int tz_get(void) {
    return (int)syscall1(SYS_tz_get, 0u);
}

int tz_set(int hours) {
    return (int)syscall1(SYS_tz_set, (unsigned)hours);
}

int uptime(struct uptime* out) {
    return (int)syscall1(SYS_uptime, (unsigned)out);
}

int get_pit_stats(struct pit_stats* out) {
    return (int)syscall1(SYS_pit_stats, (unsigned)out);
}

int task_list(unsigned index, struct task_info* out) {
    return (int)syscall2(SYS_task_list, index, (unsigned)out);
}

int get_mouse_state(struct mouse_state* out) {
    return (int)syscall1(SYS_mouse_state, (unsigned)out);
}

int mouse_reset(void) {
    return (int)syscall1(SYS_mouse_reset, 0u);
}

int get_vm_stats(struct vm_stats* out) {
    return (int)syscall1(SYS_vm_stats, (unsigned)out);
}

int chdir(const char* path) {
    return (int)syscall1(SYS_chdir, (unsigned)path);
}

char* getcwd(char* buf, size_t size) {
    long rc = syscall2(SYS_getcwd, (unsigned)buf, (unsigned)size);
    if (rc < 0) return NULL;
    return buf;
}

int unlink(const char* path) {
    return (int)syscall1(SYS_unlink, (unsigned)path);
}

int dir_read(const char* path, unsigned index, struct dirent* out) {
    return (int)syscall3(SYS_readdir, (unsigned)path, index, (unsigned)out);
}

int stat(const char* path, struct stat* st) {
    return (int)syscall2(SYS_stat, (unsigned)path, (unsigned)st);
}

int dmesg_read(char* buf, unsigned size) {
    return (int)syscall2(SYS_dmesg_read, (unsigned)buf, size);
}

int dmesg_clear(void) {
    return (int)syscall1(SYS_dmesg_clear, 0u);
}

int blk_info(unsigned index, struct blk_info* out) {
    return (int)syscall2(SYS_blk_info, index, (unsigned)out);
}

int blk_read(unsigned dev, unsigned lba, void* buf) {
    return (int)syscall3(SYS_blk_read, dev, lba, (unsigned)buf);
}

int blk_write(unsigned dev, unsigned lba, const void* buf) {
    return (int)syscall3(SYS_blk_write, dev, lba, (unsigned)buf);
}

int blk_cache_stats(struct blk_cache_stats* out) {
    return (int)syscall1(SYS_blk_cache_stats, (unsigned)out);
}

int blk_flush(int dev) {
    return (int)syscall1(SYS_blk_flush, (unsigned)dev);
}

int net_info(struct net_info* out) {
    return (int)syscall1(SYS_net_info, (unsigned)out);
}

int net_ping(const uint8_t ip[4], unsigned short seq, unsigned timeout_ms) {
    return (int)syscall3(SYS_net_ping, (unsigned)ip, seq, timeout_ms);
}

int net_dhcp_renew(void) {
    return (int)syscall1(SYS_net_dhcp_renew, 0u);
}
