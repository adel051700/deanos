#include "include/kernel/syscall.h"
#include "include/kernel/interrupt.h"
#include "include/kernel/keyboard.h"
#include "include/kernel/pit.h"
#include "include/kernel/rtc.h"
#include "include/kernel/tty.h"
#include "include/kernel/task.h"
#include "include/kernel/signal.h"
#include "include/kernel/kheap.h"
#include "include/kernel/elf.h"
#include "include/kernel/paging.h"
#include "include/kernel/vfs.h"
#include "include/kernel/shell.h"
#include "include/kernel/net.h"
#include "include/kernel/net_lwip.h"
#include "include/kernel/random.h"
#include "include/kernel/mouse.h"
#include "include/kernel/log.h"
#include "lwip_port/ksock_udp.h"
#include "lwip_port/ksock_tcp.h"
#include "lwip_port/ksock_dns.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define KSOCK_NODE_MAGIC 0x4B534F43u

/* Returned to userspace when a syscall is handed a pointer/buffer that is not
 * fully readable/writable user memory (Linux EFAULT convention). */
#define EFAULT 14

#ifndef EAGAIN
#define EAGAIN 11
#endif
#ifndef EINVAL
#define EINVAL 22
#endif

/* Upper bound for a copied-in path string, including the NUL terminator. */
#define SYS_PATH_MAX VFS_PATH_MAX

/* --- user-pointer validation & copy helpers ---------------------------------
 * Every syscall that dereferences a user-supplied pointer routes through these.
 * access_ok_* confirms the whole range is user-accessible before the kernel
 * touches it; copy_* additionally move the bytes. All copies happen while the
 * faulting task's address space is active (we are in its syscall context), and
 * this is a single-CPU kernel with no preemption mid-copy, so a range that
 * validates stays valid for the duration of the copy. */

static inline int access_ok_r(const void* uptr, uint32_t len) {
    return paging_access_ok((uintptr_t)uptr, len, 0);
}

static inline int access_ok_w(const void* uptr, uint32_t len) {
    return paging_access_ok((uintptr_t)uptr, len, 1);
}

static int copy_from_user(void* kdst, const void* usrc, uint32_t len) {
    if (len == 0u) return 0;
    if (!access_ok_r(usrc, len)) return -EFAULT;
    memcpy(kdst, usrc, len);
    return 0;
}

static int copy_to_user(void* udst, const void* ksrc, uint32_t len) {
    if (len == 0u) return 0;
    if (!access_ok_w(udst, len)) return -EFAULT;
    memcpy(udst, ksrc, len);
    return 0;
}

/* Copy a NUL-terminated user string into kdst (capacity bytes, always NUL
 * terminated on success). Validates one page at a time so an unmapped page
 * past the string is not required to be present. Returns 0 on success,
 * -EFAULT if a page is not readable user memory, -1 if the string does not
 * fit in `capacity`. */
static int copy_user_string(char* kdst, const char* usrc, uint32_t capacity) {
    if (capacity == 0u) return -1;
    uintptr_t p = (uintptr_t)usrc;
    uint32_t i = 0;
    for (;;) {
        /* Validate the byte at p is readable before dereferencing it. Checking
         * a single byte forces a per-page walk at each page boundary. */
        if (!paging_access_ok(p, 1u, 0)) return -EFAULT;
        char c = *(const char*)p;
        kdst[i] = c;
        if (c == '\0') return 0;
        if (++i >= capacity) return -1; /* no room for the terminator */
        p++;
    }
}

typedef enum ksock_role {
    KSOCK_ROLE_NONE = 0,
    KSOCK_ROLE_STREAM_CONN,
    KSOCK_ROLE_STREAM_LISTENER,
    KSOCK_ROLE_DGRAM,
} ksock_role_t;

typedef struct ksock_node_impl {
    uint32_t magic;
    uint8_t  role;
    uint8_t  nonblock;
    uint8_t  shut_rd;
    uint8_t  shut_wr;
    uint8_t  bound;         /* STREAM_LISTENER only: bind() captured a port, awaiting listen() */
    void*    udp;           /* ksock_udp_t*  when role == DGRAM */
    void*    tcp;           /* ksock_tcp_t*  when role == STREAM_CONN, or STREAM_LISTENER once listen() has run */
    uint16_t bind_port;     /* STREAM_LISTENER only: port captured at bind(), consumed by listen() */
    uint32_t rcv_timeout_ms;
    uint32_t snd_timeout_ms;
    uint32_t refs;
    uint32_t sock_flags;   /* SO_REUSEADDR, etc. */
    uint32_t tcp_flags;    /* TCP_NODELAY, etc. */
} ksock_node_impl_t;

#define KSOCK_F_REUSEADDR 0x1u
#define KSOCK_F_KEEPALIVE 0x2u
#define KSOCK_T_NODELAY   0x1u

#define KSOCK_DEFAULT_TIMEOUT_MS 2000u

static uint32_t ksock_effective_timeout(uint32_t impl_timeout_ms) {
    if (impl_timeout_ms != 0u) return impl_timeout_ms;
    return KSOCK_DEFAULT_TIMEOUT_MS;
}

static int32_t ksock_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    ksock_node_impl_t* impl;
    uint16_t out_len = 0;
    int rc;
    uint32_t to;
    (void)offset;
    if (!node || !buffer) return -1;
    impl = (ksock_node_impl_t*)node->impl;
    if (!impl || impl->magic != KSOCK_NODE_MAGIC) return -1;
    if (impl->shut_rd) return 0;
    if (impl->role != KSOCK_ROLE_STREAM_CONN || !impl->tcp) return -1;
    to = impl->nonblock ? 0u : ksock_effective_timeout(impl->rcv_timeout_ms);
    rc = ksock_tcp_recv((ksock_tcp_t*)impl->tcp, buffer, (uint16_t)size, &out_len, to, impl->nonblock);
    if (rc >= 0) return (int32_t)out_len;              /* 0 => peer closed, same as old NET_TCP_ERR_CLOSED -> 0 */
    if (rc == KSOCK_TCP_WOULDBLOCK) return NET_TCP_ERR_WOULD_BLOCK; /* preserve old ksock_read's verbatim rc pass-through */
    return -1;
}

static int32_t ksock_write(vfs_node_t* node, uint32_t offset, uint32_t size, const uint8_t* buffer) {
    ksock_node_impl_t* impl;
    int rc;
    uint32_t to;
    (void)offset;
    if (!node || (!buffer && size > 0u)) return -1;
    impl = (ksock_node_impl_t*)node->impl;
    if (!impl || impl->magic != KSOCK_NODE_MAGIC) return -1;
    if (impl->shut_wr) return -1;
    if (impl->role != KSOCK_ROLE_STREAM_CONN || !impl->tcp) return -1;
    to = impl->nonblock ? 0u : ksock_effective_timeout(impl->snd_timeout_ms);
    rc = ksock_tcp_send((ksock_tcp_t*)impl->tcp, buffer, (uint16_t)size, to, impl->nonblock);
    /* ksock_tcp_send() commits bytes into lwIP's send buffer (tcp_write +
     * tcp_output) before returning, so any rc >= 0 -- full or partial -- means
     * those bytes are already queued to the peer. Reporting WOULD_BLOCK for a
     * partial send (as the old stack's all-or-nothing helper implied) would
     * make callers resend from offset 0, duplicating bytes in the stream.
     * Only report WOULD_BLOCK when truly nothing was sent (KSOCK_TCP_WOULDBLOCK,
     * a negative value distinct from any non-negative committed count). */
    if (rc >= 0) return (int32_t)rc;
    if (rc == KSOCK_TCP_WOULDBLOCK) return NET_TCP_ERR_WOULD_BLOCK;
    return -1;
}

static int ksock_open(vfs_node_t* node, uint32_t flags) {
    ksock_node_impl_t* impl;
    (void)flags;
    if (!node) return -1;
    impl = (ksock_node_impl_t*)node->impl;
    if (!impl || impl->magic != KSOCK_NODE_MAGIC) return -1;
    impl->refs++;
    return 0;
}

static void ksock_close(vfs_node_t* node) {
    ksock_node_impl_t* impl;
    if (!node) return;
    impl = (ksock_node_impl_t*)node->impl;
    if (!impl || impl->magic != KSOCK_NODE_MAGIC) {
        kfree(node);
        return;
    }
    if (impl->refs > 0u) impl->refs--;
    if (impl->refs > 0u) return;

    if ((impl->role == KSOCK_ROLE_STREAM_CONN || impl->role == KSOCK_ROLE_STREAM_LISTENER) && impl->tcp) {
        ksock_tcp_close((ksock_tcp_t*)impl->tcp);
    } else if (impl->role == KSOCK_ROLE_DGRAM && impl->udp) {
        ksock_udp_close((ksock_udp_t*)impl->udp);
    }
    kfree(impl);
    kfree(node);
}

static vfs_node_t* ksock_make_node(uint8_t role, uint32_t timeout_ms) {
    vfs_node_t* node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    ksock_node_impl_t* impl = (ksock_node_impl_t*)kmalloc(sizeof(ksock_node_impl_t));
    if (!node || !impl) {
        if (node) kfree(node);
        if (impl) kfree(impl);
        return NULL;
    }

    memset(node, 0, sizeof(*node));
    memset(impl, 0, sizeof(*impl));
    impl->magic = KSOCK_NODE_MAGIC;
    impl->role = role;
    impl->rcv_timeout_ms = timeout_ms;
    impl->snd_timeout_ms = timeout_ms;
    impl->refs = 0u;

    strcpy(node->name, "socket");
    node->type = VFS_FILE;
    node->mode = (uint16_t)(VFS_MODE_IRUSR | VFS_MODE_IWUSR | VFS_MODE_IRGRP | VFS_MODE_IWGRP | VFS_MODE_IROTH | VFS_MODE_IWOTH);
    node->uid = 0u;
    node->gid = 0u;
    node->read = ksock_read;
    node->write = ksock_write;
    node->open = ksock_open;
    node->close = ksock_close;
    node->impl = impl;
    return node;
}

static ksock_node_impl_t* ksock_impl_from_fd(int32_t fd) {
    task_t* t = task_current();
    vfs_node_t* node;
    ksock_node_impl_t* impl;
    if (!t || fd < 0 || fd >= TASK_MAX_FDS || !t->fds[fd].in_use) return NULL;
    node = t->fds[fd].node;
    if (!node) return NULL;
    impl = (ksock_node_impl_t*)node->impl;
    if (!impl || impl->magic != KSOCK_NODE_MAGIC) return NULL;
    return impl;
}

static long sys_write(uint32_t fd, const char* buf, size_t len) {
    if (!buf || len == 0) return 0;
    if (!access_ok_r(buf, (uint32_t)len)) return -EFAULT;

    task_t* t = task_current();
    int fd_bound = 0;
    if (t && fd < TASK_MAX_FDS && t->fds[fd].in_use) fd_bound = 1;

    if (fd_bound) {
        return (long)vfs_fd_write((int)fd, (const uint8_t*)buf, (uint32_t)len);
    }

    /* Fallback console for default stdout/stderr when unbound. */
    if (fd == 1 || fd == 2) {
        shell_write_async_output(buf, len);
        return (long)len;
    }

    return -1;
}

static long sys_read(uint32_t fd, char* buf, size_t len) {
    if (!buf || len == 0) return 0;
    if (!access_ok_w(buf, (uint32_t)len)) return -EFAULT;

    task_t* t = task_current();
    int fd_bound = 0;
    if (t && fd < TASK_MAX_FDS && t->fds[fd].in_use) fd_bound = 1;

    if (fd_bound) {
        return (long)vfs_fd_read((int)fd, (uint8_t*)buf, (uint32_t)len);
    }

    /* Fallback keyboard input for default stdin when unbound. */
    if (fd == 0) {
        int fg_pgid = terminal_get_foreground_pgid();
        int ctl_sid = terminal_get_controlling_sid();
        if (t && ((fg_pgid > 0 && (int)t->pgid != fg_pgid) ||
                  (ctl_sid > 0 && (int)t->sid != ctl_sid))) {
            return -1;
        }

        size_t nread = 0;
        while (nread < len && keyboard_data_available()) {
            buf[nread++] = keyboard_getchar();
        }
        return (long)nread;
    }

    return -1;
}

static long sys_time(uint32_t* out) {
    uint32_t seconds = rtc_get_wallclock_seconds();
    if (out) {
        if (copy_to_user(out, &seconds, sizeof(seconds)) < 0) return -EFAULT;
    }
    return (long)seconds;
}

static long sys_exit(uint32_t status) {
    task_exit_with_status(status); /* marks task DEAD and yields — never returns */
    return 0;
}

static long sys_yield(void) {
    task_yield();
    return 0;
}

static long sys_sleep_ms(uint32_t milliseconds) {
    pit_sleep(milliseconds);
    return 0;
}

static long sys_getpid(void) {
    return (long)task_current_id();
}

static long sys_getppid(void) {
    return (long)task_current_ppid();
}

static long sys_getuid(void) {
    return (long)task_current_uid();
}

static long sys_getgid(void) {
    return (long)task_current_gid();
}

static long sys_kill(int32_t pid, int32_t sig) {
    return (long)signal_send_task((int)pid, (int)sig);
}

static long sys_sigaction(int32_t sig, const ksigaction_t* act, ksigaction_t* oldact) {
    ksigaction_t kact;
    ksigaction_t koldact;
    const ksigaction_t* actp = NULL;

    if (act) {
        if (copy_from_user(&kact, act, sizeof(kact)) < 0) return -EFAULT;
        actp = &kact;
    }
    if (oldact && !access_ok_w(oldact, sizeof(*oldact))) return -EFAULT;

    long rc = (long)signal_set_action_current((int)sig, actp, oldact ? &koldact : NULL);
    if (rc >= 0 && oldact) {
        if (copy_to_user(oldact, &koldact, sizeof(koldact)) < 0) return -EFAULT;
    }
    return rc;
}

static long sys_signal(int32_t sig, uintptr_t handler, uintptr_t restorer) {
    ksigaction_t oldact;
    ksigaction_t newact;
    newact.sa_handler = handler;
    newact.sa_flags = 0;
    newact.sa_mask = 0;
    newact.sa_restorer = restorer;

    if (signal_set_action_current((int)sig, &newact, &oldact) < 0) return -1;
    return (long)oldact.sa_handler;
}

static long sys_sigreturn(struct registers* r) {
    return (long)signal_sigreturn_current(r);
}

static long sys_fork(struct registers* r) {
    if (!r) return -1;
    if ((r->cs & 0x3u) != 0x3u) return -38;
    return (long)task_fork_user(r->eip, r->useresp, r->eflags);
}

static long sys_execve(const char* path, const char* const* uargv, uint32_t argc,
                        struct registers* r) {
    char kpath[SYS_PATH_MAX];
    char argbuf[ELF_ARGV_BYTES_MAX];
    const char* kargv[ELF_ARGV_MAX];

    if (!path || !r) return -1;
    if ((r->cs & 0x3u) != 0x3u) return -38;
    int cs = copy_user_string(kpath, path, sizeof(kpath));
    if (cs == -EFAULT) return -EFAULT;
    if (cs < 0) return -1;

    if (argc > (uint32_t)ELF_ARGV_MAX) return -EINVAL;
    if (argc > 0u && !uargv) return -EINVAL;

    uint32_t used = 0;
    for (uint32_t i = 0; i < argc; i++) {
        const char* uptr = NULL;
        if (copy_from_user(&uptr, &uargv[i], sizeof(uptr)) < 0) return -EFAULT;
        if (used >= sizeof(argbuf)) return -EINVAL;
        int alen = copy_user_string(&argbuf[used], uptr, sizeof(argbuf) - used);
        if (alen == -EFAULT) return -EFAULT;
        if (alen < 0) return -EINVAL;
        kargv[i] = &argbuf[used];
        used += (uint32_t)strlen(&argbuf[used]) + 1u;
    }

    return (long)elf_execve_current(kpath, kargv, (int)argc, r);
}

static long sys_waitpid(int32_t pid, int32_t* status, uint32_t options) {
    int st = 0;
    if (status && !access_ok_w(status, sizeof(*status))) return -EFAULT;
    int ret = task_waitpid((int)pid, status ? &st : NULL, options);
    if (ret > 0 && status) {
        if (copy_to_user(status, &st, sizeof(st)) < 0) return -EFAULT;
    }
    return (long)ret;
}

static long sys_open(const char* path, uint32_t flags) {
    char kpath[SYS_PATH_MAX];
    if (!path) return -1;
    int cs = copy_user_string(kpath, path, sizeof(kpath));
    if (cs == -EFAULT) return -EFAULT;
    if (cs < 0) return -1;
    return (long)vfs_fd_open(kpath, flags);
}

static long sys_close(uint32_t fd) {
    return (long)vfs_fd_close((int)fd);
}

static long sys_fstat(uint32_t fd, vfs_stat_t* st) {
    vfs_stat_t kst;
    if (!st) return -1;
    if (!access_ok_w(st, sizeof(*st))) return -EFAULT;
    long rc = (long)vfs_fd_stat((int)fd, &kst);
    if (rc >= 0) {
        if (copy_to_user(st, &kst, sizeof(kst)) < 0) return -EFAULT;
    }
    return rc;
}

static long sys_fcntl(uint32_t fd, uint32_t cmd, uint32_t arg) {
    if (cmd == VFS_F_GETFL || cmd == VFS_F_SETFL) {
        task_t* t = task_current();
        if (!t || fd >= TASK_MAX_FDS || !t->fds[fd].in_use) return -1;
        task_fd_t* f = &t->fds[fd];
        if (cmd == VFS_F_GETFL) {
            return (long)(f->open_flags & VFS_O_NONBLOCK);
        }
        if (arg & VFS_O_NONBLOCK) {
            f->open_flags |= VFS_O_NONBLOCK;
        } else {
            f->open_flags &= ~VFS_O_NONBLOCK;
        }
        ksock_node_impl_t* impl = ksock_impl_from_fd((int32_t)fd);
        if (impl) impl->nonblock = (arg & VFS_O_NONBLOCK) ? 1 : 0;
        return 0;
    }
    return (long)vfs_fd_fcntl((int)fd, cmd, arg);
}

static short ksock_poll_mask(ksock_node_impl_t* impl, short events) {
    short revents = 0;
    if (!impl) return 0;
    if (impl->role == KSOCK_ROLE_DGRAM && impl->udp) {
        if ((events & KPOLLIN) && ksock_udp_readable((ksock_udp_t*)impl->udp)) revents |= KPOLLIN;
        if (events & KPOLLOUT) revents |= KPOLLOUT;
    } else if (impl->role == KSOCK_ROLE_STREAM_CONN && impl->tcp) {
        if ((events & KPOLLIN) && ksock_tcp_readable((ksock_tcp_t*)impl->tcp)) revents |= KPOLLIN;
        if ((events & KPOLLOUT) && ksock_tcp_writable((ksock_tcp_t*)impl->tcp)) revents |= KPOLLOUT;
    } else if (impl->role == KSOCK_ROLE_STREAM_LISTENER && impl->tcp) {
        /* A listener's ksock_tcp_t never gets tcp_recv wired up (only
         * accepted connections do), so rx_len/peer_closed - and therefore
         * ksock_tcp_readable() - never reflect a pending connection. Use the
         * accept-queue accessor instead so KPOLLIN means "accept() would not
         * block". */
        if ((events & KPOLLIN) && ksock_tcp_accept_ready((ksock_tcp_t*)impl->tcp)) revents |= KPOLLIN;
    }
    if (impl->shut_rd && (events & KPOLLIN)) revents |= KPOLLHUP;
    return revents;
}

static long sys_poll(const syscall_poll_args_t* uargs) {
    syscall_poll_args_t args;
    task_t* t;
    syscall_pollfd_t* fds;
    uint32_t nfds;
    int32_t timeout;
    uint32_t elapsed = 0;
    long ready;

    if (!uargs) return -1;
    if (copy_from_user(&args, uargs, sizeof(args)) < 0) return -EFAULT;
    if (!args.fds) return -1;
    if (args.nfds == 0u) return 0;
    /* The fds array is read and its revents fields written in place across
     * poll iterations, so validate the whole array (read+write) once up front.
     * Guard the size computation against 32-bit overflow. */
    if (args.nfds > 0xFFFFFFFFu / sizeof(syscall_pollfd_t)) return -EFAULT;
    if (!access_ok_w(args.fds, args.nfds * (uint32_t)sizeof(syscall_pollfd_t))) return -EFAULT;
    t = task_current();
    if (!t) return -1;
    fds = args.fds;
    nfds = args.nfds;
    timeout = args.timeout_ms;

    for (;;) {
        ready = 0;
        for (uint32_t i = 0; i < nfds; ++i) {
            short revents = 0;
            int32_t fd = fds[i].fd;
            short events = fds[i].events;
            if (fd < 0) {
                fds[i].revents = 0;
                continue;
            }
            if ((uint32_t)fd >= TASK_MAX_FDS || !t->fds[fd].in_use) {
                fds[i].revents = KPOLLNVAL;
                ready++;
                continue;
            }
            ksock_node_impl_t* impl = ksock_impl_from_fd(fd);
            if (impl) {
                revents = ksock_poll_mask(impl, events);
            } else {
                /* Non-socket fds: treat as always ready for requested events. */
                revents = (short)(events & (KPOLLIN | KPOLLOUT));
            }
            fds[i].revents = revents;
            if (revents) ready++;
        }

        if (ready > 0) return ready;
        if (timeout == 0) return 0;
        if (timeout > 0 && elapsed >= (uint32_t)timeout) return 0;

        net_service_tick();
        pit_sleep(10u);
        elapsed += 10u;
    }
}

#define RANDOM_GETRANDOM_MAX (1u << 20)   /* clamp a single call to 1 MiB */

static long sys_getrandom(void* buf, uint32_t len, uint32_t flags) {
    if (flags & ~(uint32_t)(GRND_NONBLOCK | GRND_RANDOM)) return -EINVAL;
    if (len == 0u) return 0;
    if (len > RANDOM_GETRANDOM_MAX) len = RANDOM_GETRANDOM_MAX;
    if (!access_ok_w(buf, len)) return -EFAULT;

    if ((flags & GRND_RANDOM) && !random_is_seeded()) {
        if (flags & GRND_NONBLOCK) return -EAGAIN;
        while (!random_is_seeded()) task_sleep_ms(10);
    }

    uint8_t tmp[128];
    uint32_t done = 0u;
    while (done < len) {
        uint32_t chunk = len - done;
        if (chunk > sizeof tmp) chunk = sizeof tmp;
        random_bytes(tmp, chunk);
        if (copy_to_user((uint8_t*)buf + done, tmp, chunk) < 0) return -EFAULT;
        done += chunk;
    }
    return (long)done;
}

static long sys_shutdown(int32_t fd, int32_t how) {
    ksock_node_impl_t* impl = ksock_impl_from_fd(fd);
    if (!impl) return -1;
    if (how == KSHUT_RD || how == KSHUT_RDWR) impl->shut_rd = 1;
    if (how == KSHUT_WR || how == KSHUT_RDWR) impl->shut_wr = 1;
    return 0;
}

static long sys_setsockopt(const syscall_sockopt_args_t* uargs) {
    syscall_sockopt_args_t args;
    ksock_node_impl_t* impl;
    int value;
    uint32_t len;
    if (!uargs) return -1;
    if (copy_from_user(&args, uargs, sizeof(args)) < 0) return -EFAULT;
    if (!args.optval || !args.optlen) return -1;
    if (copy_from_user(&len, args.optlen, sizeof(len)) < 0) return -EFAULT;
    if (len != sizeof(int)) return -1;
    impl = ksock_impl_from_fd(args.socket_fd);
    if (!impl) return -1;
    if (copy_from_user(&value, args.optval, sizeof(value)) < 0) return -EFAULT;

    if (args.level == KSOL_SOCKET) {
        switch (args.optname) {
            case KSO_REUSEADDR:
                if (value) impl->sock_flags |= KSOCK_F_REUSEADDR;
                else impl->sock_flags &= ~KSOCK_F_REUSEADDR;
                return 0;
            case KSO_KEEPALIVE:
                if (value) impl->sock_flags |= KSOCK_F_KEEPALIVE;
                else impl->sock_flags &= ~KSOCK_F_KEEPALIVE;
                /* ksock_tcp.h exposes no keepalive hook; stored as a flag only, per brief. */
                return 0;
            case KSO_RCVTIMEO:
                impl->rcv_timeout_ms = (value < 0) ? 0u : (uint32_t)value;
                return 0;
            case KSO_SNDTIMEO:
                impl->snd_timeout_ms = (value < 0) ? 0u : (uint32_t)value;
                return 0;
            default: return -1;
        }
    } else if (args.level == KSOL_TCP) {
        switch (args.optname) {
            case KTCP_NODELAY:
                if (value) impl->tcp_flags |= KSOCK_T_NODELAY;
                else impl->tcp_flags &= ~KSOCK_T_NODELAY;
                if (impl->role == KSOCK_ROLE_STREAM_CONN && impl->tcp) {
                    ksock_tcp_set_nodelay((ksock_tcp_t*)impl->tcp, value ? 1 : 0);
                }
                return 0;
            default: return -1;
        }
    }
    return -1;
}

static long sys_getsockopt(const syscall_sockopt_args_t* uargs) {
    syscall_sockopt_args_t args;
    ksock_node_impl_t* impl;
    int value = 0;
    uint32_t len;
    uint32_t out_len = sizeof(int);
    if (!uargs) return -1;
    if (copy_from_user(&args, uargs, sizeof(args)) < 0) return -EFAULT;
    if (!args.optval || !args.optlen) return -1;
    if (copy_from_user(&len, args.optlen, sizeof(len)) < 0) return -EFAULT;
    if (len < sizeof(int)) return -1;
    impl = ksock_impl_from_fd(args.socket_fd);
    if (!impl) return -1;

    if (args.level == KSOL_SOCKET) {
        switch (args.optname) {
            case KSO_REUSEADDR: value = (impl->sock_flags & KSOCK_F_REUSEADDR) ? 1 : 0; break;
            case KSO_KEEPALIVE: value = (impl->sock_flags & KSOCK_F_KEEPALIVE) ? 1 : 0; break;
            case KSO_RCVTIMEO: value = (int)impl->rcv_timeout_ms; break;
            case KSO_SNDTIMEO: value = (int)impl->snd_timeout_ms; break;
            default: return -1;
        }
    } else if (args.level == KSOL_TCP) {
        switch (args.optname) {
            case KTCP_NODELAY: value = (impl->tcp_flags & KSOCK_T_NODELAY) ? 1 : 0; break;
            default: return -1;
        }
    } else {
        return -1;
    }

    if (copy_to_user(args.optval, &value, sizeof(value)) < 0) return -EFAULT;
    if (copy_to_user(args.optlen, &out_len, sizeof(out_len)) < 0) return -EFAULT;
    return 0;
}

static long sys_resolve(const syscall_resolve_args_t* uargs) {
    syscall_resolve_args_t args;
    char khost[256];
    uint8_t kip[4];
    int rc, cs;
    if (!uargs) return NET_DNS_ERR_INVALID;
    if (copy_from_user(&args, uargs, sizeof(args)) < 0) return -EFAULT;
    if (!args.hostname || !args.out_ip) return NET_DNS_ERR_INVALID;
    cs = copy_user_string(khost, args.hostname, sizeof(khost));
    if (cs == -EFAULT) return -EFAULT;
    if (cs < 0) return NET_DNS_ERR_INVALID;
    if (!access_ok_w(args.out_ip, sizeof(kip))) return -EFAULT;
    /* args.dns_server_ip (custom server override) has no equivalent in ksock_dns_query_a(),
     * which always resolves through lwIP's configured resolver; the field is accepted but
     * unused, matching this task's scope of leaving the ABI/struct untouched. */
    rc = ksock_dns_query_a(khost, kip, args.timeout_ms != 0u ? args.timeout_ms : 5000u);
    if (rc != 0) return NET_DNS_ERR_NOT_FOUND;
    if (copy_to_user(args.out_ip, kip, sizeof(kip)) < 0) return -EFAULT;
    return 0;
}

static long sys_pipe(int32_t* out_fds) {
    if (!out_fds) return -1;
    if (!access_ok_w(out_fds, 2u * sizeof(int32_t))) return -EFAULT;

    int fds[2] = {-1, -1};
    int rc = vfs_fd_pipe(fds);
    if (rc < 0) return rc;

    int32_t out[2] = { (int32_t)fds[0], (int32_t)fds[1] };
    if (copy_to_user(out_fds, out, sizeof(out)) < 0) return -EFAULT;
    return 0;
}

static long sys_setpgid(int32_t pid, int32_t pgid) {
    return (long)task_setpgid((int)pid, (int)pgid);
}

static long sys_getpgrp(void) {
    return (long)task_current_pgid();
}

static long sys_setsid(void) {
    return (long)task_setsid();
}

static long sys_tcsetpgrp(uint32_t fd, int32_t pgid) {
    (void)fd;
    if (pgid <= 0) return -1;

    int sid = task_current_sid();
    if (sid <= 0) return -1;

    int ctl_sid = terminal_get_controlling_sid();
    if (ctl_sid == 0) {
        if (terminal_set_controlling_sid(sid) < 0) return -1;
    } else if (ctl_sid != sid) {
        return -1;
    }

    if (!task_pgid_exists_in_session((uint32_t)sid, (uint32_t)pgid)) {
        return -1;
    }

    return (long)terminal_set_foreground_pgid((int)pgid);
}

static long sys_tcgetpgrp(uint32_t fd) {
    (void)fd;
    return (long)terminal_get_foreground_pgid();
}

static long sys_mkdir(const char* path) {
    char kpath[SYS_PATH_MAX];
    if (!path) return -1;
    int cs = copy_user_string(kpath, path, sizeof(kpath));
    if (cs == -EFAULT) return -EFAULT;
    if (cs < 0) return -1;
    return (long)vfs_create_path(kpath, VFS_DIRECTORY);
}

static long sys_chmod(const char* path, uint32_t mode) {
    char kpath[SYS_PATH_MAX];
    if (!path) return -1;
    int cs = copy_user_string(kpath, path, sizeof(kpath));
    if (cs == -EFAULT) return -EFAULT;
    if (cs < 0) return -1;
    return (long)vfs_chmod_path(kpath, (uint16_t)mode);
}

static long sys_chown(const char* path, uint32_t uid, uint32_t gid) {
    char kpath[SYS_PATH_MAX];
    if (!path) return -1;
    int cs = copy_user_string(kpath, path, sizeof(kpath));
    if (cs == -EFAULT) return -EFAULT;
    if (cs < 0) return -1;
    return (long)vfs_chown_path(kpath, uid, gid);
}

static long sys_mmap(const syscall_mmap_args_t* args) {
    syscall_mmap_args_t kargs;
    uintptr_t out = 0;
    if (!args) return -1;
    if (copy_from_user(&kargs, args, sizeof(kargs)) < 0) return -EFAULT;
    if (task_mmap_current(&kargs, &out) < 0) return -1;
    return (long)out;
}

static long sys_munmap(uintptr_t addr, uint32_t length) {
    return (long)task_munmap_current(addr, length);
}

static long sys_shm_open(int32_t key, uint32_t size, uint32_t flags) {
    return (long)task_shm_open_current(key, size, flags);
}

static long sys_shm_unlink(int32_t key) {
    return (long)task_shm_unlink_current(key);
}

static long sys_socket(uint32_t domain, uint32_t type, uint32_t protocol) {
    if (domain != KSOCK_AF_INET) return -1;
    if (type == KSOCK_SOCK_DGRAM) {
        vfs_node_t* node;
        ksock_udp_t* u;
        int fd;
        if (protocol != 0u && protocol != KSOCK_IPPROTO_UDP) return -1;
        u = ksock_udp_open();
        if (!u) return -1;
        node = ksock_make_node(KSOCK_ROLE_DGRAM, KSOCK_DEFAULT_TIMEOUT_MS);
        if (!node) {
            ksock_udp_close(u);
            return -1;
        }
        ((ksock_node_impl_t*)node->impl)->udp = u;
        fd = vfs_fd_install_node(node, VFS_O_RDWR, 0u);
        /* vfs_fd_install_node cleans up node on failure; u is freed by ksock_close. */
        return (long)fd;
    }
    if (type == KSOCK_SOCK_STREAM) {
        vfs_node_t* node;
        int fd;
        if (protocol != 0u && protocol != KSOCK_IPPROTO_TCP) return -1;
        node = ksock_make_node(KSOCK_ROLE_NONE, KSOCK_DEFAULT_TIMEOUT_MS);
        if (!node) return -1;
        fd = vfs_fd_install_node(node, VFS_O_RDWR, 0u);
        return (long)fd;
    }
    return -1;
}

static long sys_socket_close(int32_t socket_id) {
    return (long)vfs_fd_close((int)socket_id);
}

static long sys_bind(const syscall_bind_args_t* uargs) {
    syscall_bind_args_t args;
    ksock_node_impl_t* impl;
    if (!uargs) return -1;
    if (copy_from_user(&args, uargs, sizeof(args)) < 0) return -EFAULT;
    impl = ksock_impl_from_fd(args.socket_id);
    if (!impl) return -1;
    if (impl->role == KSOCK_ROLE_STREAM_CONN) return -1;
    if (impl->role == KSOCK_ROLE_DGRAM) {
        if (!impl->udp) return -1;
        return (long)ksock_udp_bind((ksock_udp_t*)impl->udp, args.local_port);
    }
    if (impl->role == KSOCK_ROLE_NONE) {
        impl->role = KSOCK_ROLE_STREAM_LISTENER;
    }
    if (impl->role != KSOCK_ROLE_STREAM_LISTENER) return -1;
    /* ksock_tcp.h has no standalone bind step: the pcb is created, bound, and put into
     * LISTEN state in one call (ksock_tcp_listen()) once the backlog is known at listen()
     * time. Stash the requested port here; sys_listen() below performs the actual
     * ksock_tcp_listen() call. */
    impl->bind_port = args.local_port;
    impl->bound = 1;
    return 0;
}

static long sys_connect(const syscall_connect_args_t* uargs) {
    syscall_connect_args_t args;
    ksock_node_impl_t* impl;
    uint32_t timeout;
    ksock_tcp_t* c;
    if (!uargs) return -1;
    if (copy_from_user(&args, uargs, sizeof(args)) < 0) return -EFAULT;
    impl = ksock_impl_from_fd(args.socket_fd);
    if (!impl) return -1;
    if (impl->role == KSOCK_ROLE_STREAM_LISTENER || impl->role == KSOCK_ROLE_DGRAM) return -1;
    if (impl->role == KSOCK_ROLE_STREAM_CONN) return NET_TCP_ERR_ALREADY;
    /* No per-call timeout in the standard ABI: derive from SO_SNDTIMEO (falling
     * back to KSOCK_DEFAULT_TIMEOUT_MS), same as send()/recv(). */
    timeout = ksock_effective_timeout(impl->snd_timeout_ms);
    c = ksock_tcp_connect(args.dst_ip, args.dst_port, timeout);
    /* ksock_tcp_connect() collapses every failure mode (ARP, TX, no-sockets, timeout)
     * into a NULL return. NET_TCP_ERR_TIMEOUT is the old code's most common failure
     * value for this call and the best single stand-in available. */
    if (!c) return NET_TCP_ERR_TIMEOUT;
    impl->role = KSOCK_ROLE_STREAM_CONN;
    impl->tcp = c;
    if (impl->tcp_flags & KSOCK_T_NODELAY) ksock_tcp_set_nodelay(c, 1);
    /* keepalive: ksock_tcp.h exposes no hook; flag remains stored only (see setsockopt). */
    return 0;
}

static long sys_listen(const syscall_listen_args_t* uargs) {
    syscall_listen_args_t args;
    ksock_node_impl_t* impl;
    ksock_tcp_t* lst;
    if (!uargs) return -1;
    if (copy_from_user(&args, uargs, sizeof(args)) < 0) return -EFAULT;
    impl = ksock_impl_from_fd(args.socket_fd);
    if (!impl || impl->role != KSOCK_ROLE_STREAM_LISTENER || !impl->bound) return -1;
    if (impl->tcp) return 0; /* already listening */
    lst = ksock_tcp_listen(impl->bind_port, (int)args.backlog);
    if (!lst) return -1;
    impl->tcp = lst;
    return 0;
}

static long sys_accept(const syscall_accept_args_t* uargs) {
    syscall_accept_args_t args;
    ksock_node_impl_t* impl;
    ksock_node_impl_t* new_impl;
    uint32_t timeout;
    ksock_tcp_t* c;
    vfs_node_t* node;
    int fd;
    uint8_t peer_ip[4] = {0, 0, 0, 0};
    uint16_t peer_port = 0u;
    if (!uargs) return -1;
    if (copy_from_user(&args, uargs, sizeof(args)) < 0) return -EFAULT;
    if (args.out_from_ip && !access_ok_w(args.out_from_ip, 4u)) return -EFAULT;
    if (args.out_from_port && !access_ok_w(args.out_from_port, sizeof(uint16_t))) return -EFAULT;
    impl = ksock_impl_from_fd(args.socket_fd);
    if (!impl || impl->role != KSOCK_ROLE_STREAM_LISTENER || !impl->tcp) return -1;
    timeout = impl->nonblock ? 0u
                             : ksock_effective_timeout(impl->rcv_timeout_ms);
    c = ksock_tcp_accept((ksock_tcp_t*)impl->tcp, timeout, impl->nonblock);
    /* ksock_tcp_accept() returns NULL uniformly for timeout, nonblock-empty, and no
     * pending connection -- exactly the case the old code signalled with
     * NET_TCP_ERR_WOULD_BLOCK (the old listener-accept helper also returns that value
     * regardless of nonblock, since it treats timeout==0 the same way). */
    if (!c) return NET_TCP_ERR_WOULD_BLOCK;

    node = ksock_make_node(KSOCK_ROLE_STREAM_CONN, impl->rcv_timeout_ms);
    if (!node) {
        ksock_tcp_close(c);
        return -1;
    }
    new_impl = (ksock_node_impl_t*)node->impl;
    new_impl->tcp = c;
    new_impl->snd_timeout_ms = impl->snd_timeout_ms;
    new_impl->sock_flags = impl->sock_flags;
    new_impl->tcp_flags = impl->tcp_flags;
    if (new_impl->tcp_flags & KSOCK_T_NODELAY) ksock_tcp_set_nodelay(c, 1);
    /* keepalive: no backend hook; flag remains stored only (see setsockopt). */
    fd = vfs_fd_install_node(node, VFS_O_RDWR, 0u);
    if (fd < 0) return -1;

    (void)ksock_tcp_peer(c, peer_ip, &peer_port);
    if (args.out_from_ip) (void)copy_to_user(args.out_from_ip, peer_ip, 4u);
    if (args.out_from_port) (void)copy_to_user(args.out_from_port, &peer_port, sizeof(peer_port));
    return fd;
}

static long sys_send(const syscall_send_args_t* uargs) {
    syscall_send_args_t args;
    ksock_node_impl_t* impl;
    uint8_t saved_nonblock;
    long rc;
    if (!uargs) return -1;
    if (copy_from_user(&args, uargs, sizeof(args)) < 0) return -EFAULT;
    if (!args.payload && args.payload_len > 0u) return -1;
    if (args.payload_len > 0u && !access_ok_r(args.payload, args.payload_len)) return -EFAULT;
    impl = ksock_impl_from_fd(args.socket_fd);
    if (!impl || impl->role != KSOCK_ROLE_STREAM_CONN) return -1;
    /* vfs_fd_write() -> ksock_write() derives blocking purely from impl->nonblock;
     * honor a per-call MSG_DONTWAIT by flipping it for just this call. */
    saved_nonblock = impl->nonblock;
    if (args.flags & KMSG_DONTWAIT) impl->nonblock = 1;
    rc = (long)vfs_fd_write(args.socket_fd, (const uint8_t*)args.payload, args.payload_len);
    impl->nonblock = saved_nonblock;
    return rc;
}

static long sys_recv(const syscall_recv_args_t* uargs) {
    syscall_recv_args_t args;
    ksock_node_impl_t* impl;
    uint8_t saved_nonblock;
    int32_t n;
    if (!uargs) return -1;
    if (copy_from_user(&args, uargs, sizeof(args)) < 0) return -EFAULT;
    if (!args.out_payload && args.payload_capacity > 0u) return -1;
    if (args.payload_capacity > 0u && !access_ok_w(args.out_payload, args.payload_capacity)) return -EFAULT;
    if (args.out_payload_len && !access_ok_w(args.out_payload_len, sizeof(uint16_t))) return -EFAULT;
    impl = ksock_impl_from_fd(args.socket_fd);
    if (!impl || impl->role != KSOCK_ROLE_STREAM_CONN) return -1;
    saved_nonblock = impl->nonblock;
    if (args.flags & KMSG_DONTWAIT) impl->nonblock = 1;
    n = vfs_fd_read(args.socket_fd, (uint8_t*)args.out_payload, args.payload_capacity);
    impl->nonblock = saved_nonblock;
    if (n >= 0 && args.out_payload_len) {
        uint16_t nn = (uint16_t)n;
        (void)copy_to_user(args.out_payload_len, &nn, sizeof(nn));
    }
    return n;
}

static long sys_sendto(const syscall_sendto_args_t* uargs) {
    syscall_sendto_args_t args;
    ksock_node_impl_t* impl;
    int rc;
    if (!uargs) return -1;
    if (copy_from_user(&args, uargs, sizeof(args)) < 0) return -EFAULT;
    if (!args.payload && args.payload_len > 0u) return -1;
    if (args.payload_len > 0u && !access_ok_r(args.payload, args.payload_len)) return -EFAULT;
    impl = ksock_impl_from_fd(args.socket_id);
    if (!impl || impl->role != KSOCK_ROLE_DGRAM || !impl->udp) return -1;
    if (impl->shut_wr) return -1;
    /* args.flags (e.g. MSG_DONTWAIT) has no effect: ksock_udp_sendto() never blocks. */
    rc = ksock_udp_sendto((ksock_udp_t*)impl->udp,
                          args.dst_ip,
                          args.dst_port,
                          args.payload,
                          (uint16_t)args.payload_len);
    if (rc == (int)args.payload_len) return (long)args.payload_len;
    /* ksock_udp_sendto() collapses every failure (alloc, ARP, tx) into a single -1;
     * NET_UDP_ERR_TX is the closest old single-value stand-in. */
    return (long)NET_UDP_ERR_TX;
}

static long sys_recvfrom(const syscall_recvfrom_args_t* uargs) {
    syscall_recvfrom_args_t args;
    ksock_node_impl_t* impl;
    uint8_t from_ip[4] = {0, 0, 0, 0};
    uint16_t from_port = 0u;
    uint16_t out_len = 0;
    uint32_t timeout;
    int force_nonblock;
    int rc;

    if (!uargs) return -1;
    if (copy_from_user(&args, uargs, sizeof(args)) < 0) return -EFAULT;
    if (!args.out_payload && args.payload_capacity > 0u) return -1;
    if (args.payload_capacity > 0u && !access_ok_w(args.out_payload, args.payload_capacity)) return -EFAULT;
    if (args.out_payload_len && !access_ok_w(args.out_payload_len, sizeof(uint16_t))) return -EFAULT;
    if (args.out_from_ip && !access_ok_w(args.out_from_ip, 4u)) return -EFAULT;
    if (args.out_from_port && !access_ok_w(args.out_from_port, sizeof(uint16_t))) return -EFAULT;
    impl = ksock_impl_from_fd(args.socket_id);
    if (!impl || impl->role != KSOCK_ROLE_DGRAM || !impl->udp) return -1;
    if (impl->shut_rd) {
        if (args.out_payload_len) {
            uint16_t zero = 0;
            (void)copy_to_user(args.out_payload_len, &zero, sizeof(zero));
        }
        return 0;
    }

    force_nonblock = (impl->nonblock || (args.flags & KMSG_DONTWAIT)) ? 1 : 0;
    timeout = force_nonblock ? 0u : ksock_effective_timeout(impl->rcv_timeout_ms);

    rc = ksock_udp_recvfrom((ksock_udp_t*)impl->udp,
                            args.out_payload,
                            (uint16_t)args.payload_capacity,
                            &out_len,
                            from_ip, &from_port,
                            timeout, force_nonblock);
    /* ksock_udp_recvfrom() returns 0 uniformly for timeout/would-block/nonblock-empty
     * (per its header contract) and -1 only for a null socket; map both to the old
     * NET_UDP_ERR_WOULD_BLOCK value used for the (overwhelmingly common) timeout case.
     * Note: this also means a genuine 0-length UDP datagram is now indistinguishable
     * from a timeout -- an existing ksock_udp.c API limitation, not something this
     * task's syscall.c changes can recover. NET_UDP_ERR_MSG_TRUNC can likewise no
     * longer be detected since ksock_udp_recvfrom() truncates silently. */
    if (rc <= 0) return (long)NET_UDP_ERR_WOULD_BLOCK;

    if (args.out_payload_len) (void)copy_to_user(args.out_payload_len, &out_len, sizeof(out_len));
    if (args.out_from_ip) (void)copy_to_user(args.out_from_ip, from_ip, 4u);
    if (args.out_from_port) (void)copy_to_user(args.out_from_port, &from_port, sizeof(from_port));
    return (long)out_len;
}

static long sys_localtime(rtc_time_t* out) {
    if (!out) return -1;
    if (!access_ok_w(out, sizeof(*out))) return -EFAULT;
    rtc_time_t t;
    rtc_get_local_time(&t);
    if (copy_to_user(out, &t, sizeof(t)) < 0) return -EFAULT;
    return 0;
}

static long sys_tz_get(void) {
    return (long)rtc_tz_get_hours();
}

static long sys_tz_set(int32_t hours) {
    return (long)rtc_tz_set_hours(hours);
}

static long sys_uptime(uptime_t* out) {
    if (!out) return -1;
    if (!access_ok_w(out, sizeof(*out))) return -EFAULT;
    uptime_t u;
    get_uptime(&u);
    if (copy_to_user(out, &u, sizeof(u)) < 0) return -EFAULT;
    return 0;
}

static long sys_pit_stats(pit_stats_t* out) {
    if (!out) return -1;
    if (!access_ok_w(out, sizeof(*out))) return -EFAULT;
    pit_stats_t st;
    st.ticks = pit_get_ticks();
    st.uptime_ms = pit_get_uptime_ms();
    if (copy_to_user(out, &st, sizeof(st)) < 0) return -EFAULT;
    return 0;
}

static long sys_task_list(uint32_t index, task_info_t* out) {
    if (!out) return -1;
    if (!access_ok_w(out, sizeof(*out))) return -EFAULT;
    if (index >= task_count()) return -1;
    const task_t* t = task_get(index);
    if (!t) return -1;
    task_info_t info;
    info.id = (int32_t)t->id;
    info.parent_id = (int32_t)t->parent_id;
    info.sid = (int32_t)t->sid;
    info.pgid = (int32_t)t->pgid;
    info.state = (int32_t)t->state;
    info.quantum = t->quantum;
    memcpy(info.name, t->name, sizeof(info.name));
    if (copy_to_user(out, &info, sizeof(info)) < 0) return -EFAULT;
    return 0;
}

static long sys_mouse_state(mouse_state_t* out) {
    if (!out) return -1;
    if (!access_ok_w(out, sizeof(*out))) return -EFAULT;
    mouse_state_t st;
    mouse_get_state(&st);
    if (copy_to_user(out, &st, sizeof(st)) < 0) return -EFAULT;
    return (long)mouse_is_ready();
}

static long sys_mouse_reset(void) {
    mouse_reset_counters();
    return 0;
}

static long sys_vm_stats(paging_stats_t* out) {
    if (!out) return -1;
    if (!access_ok_w(out, sizeof(*out))) return -EFAULT;
    paging_stats_t st;
    paging_get_stats(&st);
    if (copy_to_user(out, &st, sizeof(st)) < 0) return -EFAULT;
    return 0;
}

static long sys_chdir(const char* path) {
    char kpath[SYS_PATH_MAX];
    char resolved[SYS_PATH_MAX];
    if (!path) return -1;
    int cs = copy_user_string(kpath, path, sizeof(kpath));
    if (cs == -EFAULT) return -EFAULT;
    if (cs < 0) return -1;

    task_t* cur = task_current();
    if (!cur) return -1;
    if (vfs_normalize_path(cur->cwd, kpath, resolved, sizeof(resolved)) < 0) return -1;

    vfs_node_t* node = vfs_namei(resolved);
    if (!node) return -1;
    if (!(node->type & VFS_DIRECTORY)) return -1;

    strncpy(cur->cwd, resolved, sizeof(cur->cwd) - 1);
    cur->cwd[sizeof(cur->cwd) - 1] = '\0';
    return 0;
}

static long sys_getcwd(char* buf, uint32_t size) {
    if (!buf) return -1;
    task_t* cur = task_current();
    if (!cur) return -1;
    size_t len = strlen(cur->cwd);
    if ((uint32_t)(len + 1) > size) return -1;
    if (!access_ok_w(buf, len + 1)) return -EFAULT;
    if (copy_to_user(buf, cur->cwd, len + 1) < 0) return -EFAULT;
    return (long)len;
}

static long sys_readdir(const char* path, uint32_t index, vfs_dirent_t* out) {
    char kpath[SYS_PATH_MAX];
    char resolved[SYS_PATH_MAX];
    if (!path || !out) return -1;
    int cs = copy_user_string(kpath, path, sizeof(kpath));
    if (cs == -EFAULT) return -EFAULT;
    if (cs < 0) return -1;
    if (!access_ok_w(out, sizeof(*out))) return -EFAULT;

    task_t* cur = task_current();
    if (!cur) return -1;
    if (vfs_normalize_path(cur->cwd, kpath, resolved, sizeof(resolved)) < 0) return -1;

    vfs_node_t* node = vfs_namei(resolved);
    if (!node) return -1;
    if (!(node->type & VFS_DIRECTORY)) return -1;

    vfs_dirent_t entry;
    int rc = vfs_readdir(node, index, &entry);
    if (rc != 0) return (long)rc;
    if (copy_to_user(out, &entry, sizeof(entry)) < 0) return -EFAULT;
    return 0;
}

static long sys_unlink(const char* path) {
    char kpath[SYS_PATH_MAX];
    char resolved[SYS_PATH_MAX];
    if (!path) return -1;
    int cs = copy_user_string(kpath, path, sizeof(kpath));
    if (cs == -EFAULT) return -EFAULT;
    if (cs < 0) return -1;

    task_t* cur = task_current();
    if (!cur) return -1;
    if (vfs_normalize_path(cur->cwd, kpath, resolved, sizeof(resolved)) < 0) return -1;

    return (long)vfs_unlink_path(resolved);
}

static long sys_stat(const char* path, vfs_stat_t* out) {
    char kpath[SYS_PATH_MAX];
    char resolved[SYS_PATH_MAX];
    if (!path || !out) return -1;
    int cs = copy_user_string(kpath, path, sizeof(kpath));
    if (cs == -EFAULT) return -EFAULT;
    if (cs < 0) return -1;
    if (!access_ok_w(out, sizeof(*out))) return -EFAULT;

    task_t* cur = task_current();
    if (!cur) return -1;
    if (vfs_normalize_path(cur->cwd, kpath, resolved, sizeof(resolved)) < 0) return -1;

    vfs_stat_t st;
    if (vfs_stat_path(resolved, &st) < 0) return -1;
    if (copy_to_user(out, &st, sizeof(st)) < 0) return -EFAULT;
    return 0;
}

static long sys_dmesg_read(char* buf, uint32_t size) {
    if (!buf) return -1;
    char kbuf[KLOG_BUF_SZ];
    size_t copied = klog_read(kbuf, sizeof(kbuf));
    size_t to_copy = (size < copied) ? size : copied;
    if (!access_ok_w(buf, to_copy)) return -EFAULT;
    if (copy_to_user(buf, kbuf, to_copy) < 0) return -EFAULT;
    return (long)to_copy;
}

static long sys_dmesg_clear(void) {
    klog_clear();
    return 0;
}

static long syscall_dispatch(uint32_t num, uint32_t a1, uint32_t a2, uint32_t a3, struct registers* r) {
    switch (num) {
        case SYS_write: return sys_write(a1, (const char*)a2, (size_t)a3);
        case SYS_read:  return sys_read(a1, (char*)a2, (size_t)a3);
        case SYS_time:  return sys_time((uint32_t*)a1);
        case SYS_exit:  return sys_exit(a1);
        case SYS_open:  return sys_open((const char*)a1, a2);
        case SYS_close: return sys_close(a1);
        case SYS_fstat: return sys_fstat(a1, (vfs_stat_t*)a2);
        case SYS_mkdir: return sys_mkdir((const char*)a1);
        case SYS_yield: return sys_yield();
        case SYS_sleep_ms: return sys_sleep_ms(a1);
        case SYS_getpid: return sys_getpid();
        case SYS_getppid: return sys_getppid();
        case SYS_getuid: return sys_getuid();
        case SYS_getgid: return sys_getgid();
        case SYS_kill: return sys_kill((int32_t)a1, (int32_t)a2);
        case SYS_fork: return sys_fork(r);
        case SYS_execve: return sys_execve((const char*)a1, (const char* const*)a2, a3, r);
        case SYS_waitpid: return sys_waitpid((int32_t)a1, (int32_t*)a2, a3);
        case SYS_fcntl: return sys_fcntl(a1, a2, a3);
        case SYS_pipe: return sys_pipe((int32_t*)a1);
        case SYS_setpgid: return sys_setpgid((int32_t)a1, (int32_t)a2);
        case SYS_getpgrp: return sys_getpgrp();
        case SYS_setsid: return sys_setsid();
        case SYS_tcsetpgrp: return sys_tcsetpgrp(a1, (int32_t)a2);
        case SYS_tcgetpgrp: return sys_tcgetpgrp(a1);
        case SYS_sigaction: return sys_sigaction((int32_t)a1, (const ksigaction_t*)a2, (ksigaction_t*)a3);
        case SYS_signal: return sys_signal((int32_t)a1, (uintptr_t)a2, (uintptr_t)a3);
        case SYS_sigreturn: return sys_sigreturn(r);
        case SYS_mmap: return sys_mmap((const syscall_mmap_args_t*)a1);
        case SYS_munmap: return sys_munmap((uintptr_t)a1, a2);
        case SYS_shm_open: return sys_shm_open((int32_t)a1, a2, a3);
        case SYS_shm_unlink: return sys_shm_unlink((int32_t)a1);
        case SYS_socket: return sys_socket(a1, a2, a3);
        case SYS_socket_close: return sys_socket_close((int32_t)a1);
        case SYS_bind: return sys_bind((const syscall_bind_args_t*)a1);
        case SYS_sendto: return sys_sendto((const syscall_sendto_args_t*)a1);
        case SYS_recvfrom: return sys_recvfrom((const syscall_recvfrom_args_t*)a1);
        case SYS_connect: return sys_connect((const syscall_connect_args_t*)a1);
        case SYS_listen: return sys_listen((const syscall_listen_args_t*)a1);
        case SYS_accept: return sys_accept((const syscall_accept_args_t*)a1);
        case SYS_send: return sys_send((const syscall_send_args_t*)a1);
        case SYS_recv: return sys_recv((const syscall_recv_args_t*)a1);
        case SYS_chmod: return sys_chmod((const char*)a1, a2);
        case SYS_chown: return sys_chown((const char*)a1, a2, a3);
        case SYS_resolve: return sys_resolve((const syscall_resolve_args_t*)a1);
        case SYS_shutdown: return sys_shutdown((int32_t)a1, (int32_t)a2);
        case SYS_setsockopt: return sys_setsockopt((const syscall_sockopt_args_t*)a1);
        case SYS_getsockopt: return sys_getsockopt((const syscall_sockopt_args_t*)a1);
        case SYS_poll: return sys_poll((const syscall_poll_args_t*)a1);
        case SYS_getrandom: return sys_getrandom((void*)a1, a2, a3);
        case SYS_localtime: return sys_localtime((rtc_time_t*)a1);
        case SYS_tz_get: return sys_tz_get();
        case SYS_tz_set: return sys_tz_set((int32_t)a1);
        case SYS_uptime: return sys_uptime((uptime_t*)a1);
        case SYS_pit_stats: return sys_pit_stats((pit_stats_t*)a1);
        case SYS_task_list: return sys_task_list(a1, (task_info_t*)a2);
        case SYS_mouse_state: return sys_mouse_state((mouse_state_t*)a1);
        case SYS_mouse_reset: return sys_mouse_reset();
        case SYS_vm_stats: return sys_vm_stats((paging_stats_t*)a1);
        case SYS_chdir: return sys_chdir((const char*)a1);
        case SYS_getcwd: return sys_getcwd((char*)a1, a2);
        case SYS_readdir: return sys_readdir((const char*)a1, a2, (vfs_dirent_t*)a3);
        case SYS_unlink: return sys_unlink((const char*)a1);
        case SYS_stat: return sys_stat((const char*)a1, (vfs_stat_t*)a2);
        case SYS_dmesg_read: return sys_dmesg_read((char*)a1, a2);
        case SYS_dmesg_clear: return sys_dmesg_clear();
        default:        return -38; /* ENOSYS */
    }
}

static void syscall_isr(struct registers* r) {
    // ABI: eax=num, ebx=a1, ecx=a2, edx=a3
    uint32_t num = r->eax;
    uint32_t a1  = r->ebx;
    uint32_t a2  = r->ecx;
    uint32_t a3  = r->edx;
    long ret = syscall_dispatch(num, a1, a2, a3, r);
    r->eax = (uint32_t)ret;
}

void syscall_initialize(void) {
    register_interrupt_handler(128, syscall_isr); // int 0x80
    register_interrupt_handler(129, syscall_isr); // int 0x81
}