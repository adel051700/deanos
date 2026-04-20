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
#include "include/kernel/vfs.h"
#include "include/kernel/shell.h"
#include "include/kernel/net.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define KSOCK_NODE_MAGIC 0x4B534F43u

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
    int32_t  id;
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

static uint32_t ksock_effective_timeout(uint32_t impl_timeout_ms, uint32_t override_ms) {
    if (override_ms != 0u) return override_ms;
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
    if (impl->role != KSOCK_ROLE_STREAM_CONN || impl->id < 0) return -1;
    to = impl->nonblock ? 0u : ksock_effective_timeout(impl->rcv_timeout_ms, 0u);
    rc = net_tcp_client_recv(impl->id, buffer, (uint16_t)size, &out_len, to);
    if (rc == NET_TCP_OK) return (int32_t)out_len;
    if (rc == NET_TCP_ERR_CLOSED) return 0;
    return rc;
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
    if (impl->role != KSOCK_ROLE_STREAM_CONN || impl->id < 0) return -1;
    to = impl->nonblock ? 0u : ksock_effective_timeout(impl->snd_timeout_ms, 0u);
    rc = net_tcp_client_send(impl->id, buffer, (uint16_t)size, to);
    if (rc == NET_TCP_OK) return (int32_t)size;
    return rc;
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

    if (impl->role == KSOCK_ROLE_STREAM_CONN && impl->id >= 0) {
        (void)net_tcp_client_close(impl->id, 1000u);
    } else if (impl->role == KSOCK_ROLE_STREAM_LISTENER && impl->id >= 0) {
        (void)net_tcp_listener_close(impl->id);
    } else if (impl->role == KSOCK_ROLE_DGRAM && impl->id >= 0) {
        (void)net_udp_socket_close(impl->id);
    }
    kfree(impl);
    kfree(node);
}

static vfs_node_t* ksock_make_node(uint8_t role, int32_t id, uint32_t timeout_ms) {
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
    impl->id = id;
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
    if (out) *out = seconds;
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
    return (long)signal_set_action_current((int)sig, act, oldact);
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

static long sys_execve(const char* path, struct registers* r) {
    if (!path || !r) return -1;
    if ((r->cs & 0x3u) != 0x3u) return -38;
    return (long)elf_execve_current(path, r);
}

static long sys_waitpid(int32_t pid, int32_t* status, uint32_t options) {
    int st = 0;
    int ret = task_waitpid((int)pid, status ? &st : NULL, options);
    if (ret > 0 && status) *status = st;
    return (long)ret;
}

static long sys_open(const char* path, uint32_t flags) {
    if (!path) return -1;
    return (long)vfs_fd_open(path, flags);
}

static long sys_close(uint32_t fd) {
    return (long)vfs_fd_close((int)fd);
}

static long sys_fstat(uint32_t fd, vfs_stat_t* st) {
    if (!st) return -1;
    return (long)vfs_fd_stat((int)fd, st);
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
    if (impl->role == KSOCK_ROLE_DGRAM && impl->id >= 0) {
        if ((events & KPOLLIN) && net_udp_socket_readable(impl->id)) revents |= KPOLLIN;
        if (events & KPOLLOUT) revents |= KPOLLOUT;
    } else if (impl->role == KSOCK_ROLE_STREAM_CONN && impl->id >= 0) {
        if ((events & KPOLLIN) && net_tcp_client_readable(impl->id)) revents |= KPOLLIN;
        if ((events & KPOLLOUT) && net_tcp_client_writable(impl->id)) revents |= KPOLLOUT;
    } else if (impl->role == KSOCK_ROLE_STREAM_LISTENER && impl->id >= 0) {
        if ((events & KPOLLIN) && net_tcp_listener_readable(impl->id)) revents |= KPOLLIN;
    }
    if (impl->shut_rd && (events & KPOLLIN)) revents |= KPOLLHUP;
    return revents;
}

static long sys_poll(const syscall_poll_args_t* args) {
    task_t* t;
    syscall_pollfd_t* fds;
    uint32_t nfds;
    int32_t timeout;
    uint32_t elapsed = 0;
    long ready;

    if (!args || !args->fds) return -1;
    if (args->nfds == 0u) return 0;
    t = task_current();
    if (!t) return -1;
    fds = args->fds;
    nfds = args->nfds;
    timeout = args->timeout_ms;

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

        (void)net_poll(0u);
        pit_sleep(10u);
        elapsed += 10u;
    }
}

static long sys_shutdown(int32_t fd, int32_t how) {
    ksock_node_impl_t* impl = ksock_impl_from_fd(fd);
    if (!impl) return -1;
    if (how == KSHUT_RD || how == KSHUT_RDWR) impl->shut_rd = 1;
    if (how == KSHUT_WR || how == KSHUT_RDWR) impl->shut_wr = 1;
    return 0;
}

static long sys_setsockopt(const syscall_sockopt_args_t* args) {
    ksock_node_impl_t* impl;
    int value;
    uint32_t len;
    if (!args || !args->optval || !args->optlen) return -1;
    len = *args->optlen;
    if (len != sizeof(int)) return -1;
    impl = ksock_impl_from_fd(args->socket_fd);
    if (!impl) return -1;
    value = *(const int*)args->optval;

    if (args->level == KSOL_SOCKET) {
        switch (args->optname) {
            case KSO_REUSEADDR:
                if (value) impl->sock_flags |= KSOCK_F_REUSEADDR;
                else impl->sock_flags &= ~KSOCK_F_REUSEADDR;
                return 0;
            case KSO_KEEPALIVE:
                if (value) impl->sock_flags |= KSOCK_F_KEEPALIVE;
                else impl->sock_flags &= ~KSOCK_F_KEEPALIVE;
                if (impl->role == KSOCK_ROLE_STREAM_CONN && impl->id >= 0) {
                    (void)net_tcp_set_keepalive(impl->id, value ? 1 : 0);
                }
                return 0;
            case KSO_RCVTIMEO:
                impl->rcv_timeout_ms = (value < 0) ? 0u : (uint32_t)value;
                return 0;
            case KSO_SNDTIMEO:
                impl->snd_timeout_ms = (value < 0) ? 0u : (uint32_t)value;
                return 0;
            default: return -1;
        }
    } else if (args->level == KSOL_TCP) {
        switch (args->optname) {
            case KTCP_NODELAY:
                if (value) impl->tcp_flags |= KSOCK_T_NODELAY;
                else impl->tcp_flags &= ~KSOCK_T_NODELAY;
                if (impl->role == KSOCK_ROLE_STREAM_CONN && impl->id >= 0) {
                    (void)net_tcp_set_nodelay(impl->id, value ? 1 : 0);
                }
                return 0;
            default: return -1;
        }
    }
    return -1;
}

static long sys_getsockopt(const syscall_sockopt_args_t* args) {
    ksock_node_impl_t* impl;
    int value = 0;
    uint32_t len;
    if (!args || !args->optval || !args->optlen) return -1;
    len = *args->optlen;
    if (len < sizeof(int)) return -1;
    impl = ksock_impl_from_fd(args->socket_fd);
    if (!impl) return -1;

    if (args->level == KSOL_SOCKET) {
        switch (args->optname) {
            case KSO_REUSEADDR: value = (impl->sock_flags & KSOCK_F_REUSEADDR) ? 1 : 0; break;
            case KSO_KEEPALIVE: value = (impl->sock_flags & KSOCK_F_KEEPALIVE) ? 1 : 0; break;
            case KSO_RCVTIMEO: value = (int)impl->rcv_timeout_ms; break;
            case KSO_SNDTIMEO: value = (int)impl->snd_timeout_ms; break;
            default: return -1;
        }
    } else if (args->level == KSOL_TCP) {
        switch (args->optname) {
            case KTCP_NODELAY: value = (impl->tcp_flags & KSOCK_T_NODELAY) ? 1 : 0; break;
            default: return -1;
        }
    } else {
        return -1;
    }

    *(int*)args->optval = value;
    *args->optlen = sizeof(int);
    return 0;
}

static long sys_resolve(const syscall_resolve_args_t* args) {
    if (!args || !args->hostname || !args->out_ip) return NET_DNS_ERR_INVALID;
    return (long)net_dns_query_a(args->hostname,
                                 args->dns_server_ip,
                                 args->out_ip,
                                 args->timeout_ms);
}

static long sys_pipe(int32_t* out_fds) {
    if (!out_fds) return -1;

    int fds[2] = {-1, -1};
    int rc = vfs_fd_pipe(fds);
    if (rc < 0) return rc;

    out_fds[0] = fds[0];
    out_fds[1] = fds[1];
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
    if (!path) return -1;
    return (long)vfs_create_path(path, VFS_DIRECTORY);
}

static long sys_chmod(const char* path, uint32_t mode) {
    if (!path) return -1;
    return (long)vfs_chmod_path(path, (uint16_t)mode);
}

static long sys_chown(const char* path, uint32_t uid, uint32_t gid) {
    if (!path) return -1;
    return (long)vfs_chown_path(path, uid, gid);
}

static long sys_mmap(const syscall_mmap_args_t* args) {
    uintptr_t out = 0;
    if (task_mmap_current(args, &out) < 0) return -1;
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
        int udp_id;
        int fd;
        if (protocol != 0u && protocol != KSOCK_IPPROTO_UDP) return -1;
        udp_id = net_udp_socket_open();
        if (udp_id < 0) return (long)udp_id;
        node = ksock_make_node(KSOCK_ROLE_DGRAM, udp_id, KSOCK_DEFAULT_TIMEOUT_MS);
        if (!node) {
            (void)net_udp_socket_close(udp_id);
            return -1;
        }
        fd = vfs_fd_install_node(node, VFS_O_RDWR, 0u);
        /* vfs_fd_install_node cleans up node on failure; udp_id is freed by ksock_close. */
        return (long)fd;
    }
    if (type == KSOCK_SOCK_STREAM) {
        vfs_node_t* node;
        int fd;
        if (protocol != 0u && protocol != KSOCK_IPPROTO_TCP) return -1;
        node = ksock_make_node(KSOCK_ROLE_NONE, -1, KSOCK_DEFAULT_TIMEOUT_MS);
        if (!node) return -1;
        fd = vfs_fd_install_node(node, VFS_O_RDWR, 0u);
        return (long)fd;
    }
    return -1;
}

static long sys_socket_close(int32_t socket_id) {
    return (long)vfs_fd_close((int)socket_id);
}

static long sys_bind(const syscall_bind_args_t* args) {
    ksock_node_impl_t* impl;
    int lid;
    if (!args) return -1;
    impl = ksock_impl_from_fd(args->socket_id);
    if (!impl) return -1;
    if (impl->role == KSOCK_ROLE_STREAM_CONN) return -1;
    if (impl->role == KSOCK_ROLE_DGRAM) {
        if (impl->id < 0) return -1;
        return (long)net_udp_socket_bind(impl->id, args->local_port);
    }
    if (impl->role == KSOCK_ROLE_NONE) {
        lid = net_tcp_listener_open();
        if (lid < 0) return lid;
        impl->role = KSOCK_ROLE_STREAM_LISTENER;
        impl->id = lid;
    }
    return (long)net_tcp_listener_bind(impl->id, args->local_port);
}

static long sys_connect(const syscall_connect_args_t* args) {
    ksock_node_impl_t* impl;
    uint32_t timeout;
    int sid;
    int rc;
    if (!args) return -1;
    impl = ksock_impl_from_fd(args->socket_fd);
    if (!impl) return -1;
    if (impl->role == KSOCK_ROLE_STREAM_LISTENER || impl->role == KSOCK_ROLE_DGRAM) return -1;
    if (impl->role == KSOCK_ROLE_STREAM_CONN) return NET_TCP_ERR_ALREADY;
    timeout = ksock_effective_timeout(impl->snd_timeout_ms, args->timeout_ms);
    rc = net_tcp_client_connect(args->dst_ip, args->dst_port, timeout, &sid);
    if (rc != NET_TCP_OK) return rc;
    impl->role = KSOCK_ROLE_STREAM_CONN;
    impl->id = sid;
    if (impl->tcp_flags & KSOCK_T_NODELAY) (void)net_tcp_set_nodelay(sid, 1);
    if (impl->sock_flags & KSOCK_F_KEEPALIVE) (void)net_tcp_set_keepalive(sid, 1);
    if (args->timeout_ms != 0u) {
        impl->rcv_timeout_ms = args->timeout_ms;
        impl->snd_timeout_ms = args->timeout_ms;
    }
    return 0;
}

static long sys_listen(const syscall_listen_args_t* args) {
    ksock_node_impl_t* impl;
    if (!args) return -1;
    impl = ksock_impl_from_fd(args->socket_fd);
    if (!impl || impl->role != KSOCK_ROLE_STREAM_LISTENER) return -1;
    return (long)net_tcp_listener_listen(impl->id, args->backlog);
}

static long sys_accept(const syscall_accept_args_t* args) {
    ksock_node_impl_t* impl;
    ksock_node_impl_t* new_impl;
    uint32_t timeout;
    int sid;
    int rc;
    vfs_node_t* node;
    int fd;
    uint8_t peer_ip[4] = {0, 0, 0, 0};
    uint16_t peer_port = 0u;
    if (!args) return -1;
    impl = ksock_impl_from_fd(args->socket_fd);
    if (!impl || impl->role != KSOCK_ROLE_STREAM_LISTENER) return -1;
    timeout = impl->nonblock ? 0u
                             : ksock_effective_timeout(impl->rcv_timeout_ms, args->timeout_ms);
    rc = net_tcp_listener_accept(impl->id, timeout, &sid);
    if (rc != NET_TCP_OK) return rc;

    node = ksock_make_node(KSOCK_ROLE_STREAM_CONN, sid, impl->rcv_timeout_ms);
    if (!node) {
        (void)net_tcp_client_close(sid, 100u);
        return -1;
    }
    new_impl = (ksock_node_impl_t*)node->impl;
    new_impl->snd_timeout_ms = impl->snd_timeout_ms;
    new_impl->sock_flags = impl->sock_flags;
    new_impl->tcp_flags = impl->tcp_flags;
    if (new_impl->tcp_flags & KSOCK_T_NODELAY) (void)net_tcp_set_nodelay(sid, 1);
    if (new_impl->sock_flags & KSOCK_F_KEEPALIVE) (void)net_tcp_set_keepalive(sid, 1);
    fd = vfs_fd_install_node(node, VFS_O_RDWR, 0u);
    if (fd < 0) return -1;

    (void)net_tcp_socket_peer(sid, peer_ip, &peer_port);
    if (args->out_from_ip) memcpy(args->out_from_ip, peer_ip, 4);
    if (args->out_from_port) *args->out_from_port = peer_port;
    return fd;
}

static long sys_send(const syscall_send_args_t* args) {
    ksock_node_impl_t* impl;
    if (!args || (!args->payload && args->payload_len > 0u)) return -1;
    impl = ksock_impl_from_fd(args->socket_fd);
    if (!impl || impl->role != KSOCK_ROLE_STREAM_CONN) return -1;
    if (args->timeout_ms != 0u) impl->snd_timeout_ms = args->timeout_ms;
    return (long)vfs_fd_write(args->socket_fd, (const uint8_t*)args->payload, args->payload_len);
}

static long sys_recv(const syscall_recv_args_t* args) {
    ksock_node_impl_t* impl;
    int32_t n;
    if (!args || (!args->out_payload && args->payload_capacity > 0u)) return -1;
    impl = ksock_impl_from_fd(args->socket_fd);
    if (!impl || impl->role != KSOCK_ROLE_STREAM_CONN) return -1;
    if (args->timeout_ms != 0u) impl->rcv_timeout_ms = args->timeout_ms;
    n = vfs_fd_read(args->socket_fd, (uint8_t*)args->out_payload, args->payload_capacity);
    if (n >= 0 && args->out_payload_len) *args->out_payload_len = (uint16_t)n;
    return n;
}

static long sys_sendto(const syscall_sendto_args_t* args) {
    ksock_node_impl_t* impl;
    int rc;
    if (!args) return -1;
    impl = ksock_impl_from_fd(args->socket_id);
    if (!impl || impl->role != KSOCK_ROLE_DGRAM || impl->id < 0) return -1;
    if (impl->shut_wr) return -1;
    rc = net_udp_socket_sendto(impl->id,
                               args->dst_ip,
                               args->dst_port,
                               args->payload,
                               (uint16_t)args->payload_len);
    if (rc == NET_UDP_OK) return (long)args->payload_len;
    return (long)rc;
}

static long sys_recvfrom(const syscall_recvfrom_args_t* args) {
    ksock_node_impl_t* impl;
    net_udp_endpoint_t from;
    uint16_t out_len = 0;
    uint32_t timeout;
    int rc;

    if (!args) return -1;
    impl = ksock_impl_from_fd(args->socket_id);
    if (!impl || impl->role != KSOCK_ROLE_DGRAM || impl->id < 0) return -1;
    if (impl->shut_rd) {
        if (args->out_payload_len) *args->out_payload_len = 0;
        return 0;
    }

    timeout = impl->nonblock ? 0u
                             : ksock_effective_timeout(impl->rcv_timeout_ms, args->timeout_ms);

    rc = net_udp_socket_recvfrom(impl->id,
                                 args->out_payload,
                                 (uint16_t)args->payload_capacity,
                                 &out_len,
                                 &from,
                                 timeout);
    if (rc != NET_UDP_OK && rc != NET_UDP_ERR_MSG_TRUNC) {
        return (long)rc;
    }

    if (args->out_payload_len) *args->out_payload_len = out_len;
    if (args->out_from_ip) memcpy(args->out_from_ip, from.ip, 4);
    if (args->out_from_port) *args->out_from_port = from.port;

    if (rc == NET_UDP_ERR_MSG_TRUNC) return NET_UDP_ERR_MSG_TRUNC;
    return (long)out_len;
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
        case SYS_execve: return sys_execve((const char*)a1, r);
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