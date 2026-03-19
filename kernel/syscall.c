#include "include/kernel/syscall.h"
#include "include/kernel/interrupt.h"
#include "include/kernel/keyboard.h"
#include "include/kernel/pit.h"
#include "include/kernel/rtc.h"
#include "include/kernel/tty.h"
#include "include/kernel/task.h"
#include "include/kernel/signal.h"
#include "include/kernel/elf.h"
#include "include/kernel/vfs.h"
#include "include/kernel/shell.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

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

static long sys_kill(uint32_t pid) {
    return (long)signal_send_task((int)pid, KSIGKILL);
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
    return (long)vfs_fd_fcntl((int)fd, cmd, arg);
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
    /* Resolve parent, create directory child */
    /* Find the last '/' to split parent and name */
    size_t plen = 0;
    while (path[plen]) plen++;
    if (plen == 0) return -1;

    /* Find last slash */
    int last_slash = -1;
    for (size_t i = 0; i < plen; i++) {
        if (path[i] == '/') last_slash = (int)i;
    }

    char parent_path[VFS_PATH_MAX];
    char dir_name[VFS_NAME_MAX];

    if (last_slash < 0) {
        parent_path[0] = '/'; parent_path[1] = '\0';
        strncpy(dir_name, path, VFS_NAME_MAX - 1);
        dir_name[VFS_NAME_MAX - 1] = '\0';
    } else if (last_slash == 0) {
        parent_path[0] = '/'; parent_path[1] = '\0';
        strncpy(dir_name, path + 1, VFS_NAME_MAX - 1);
        dir_name[VFS_NAME_MAX - 1] = '\0';
    } else {
        size_t dlen = (size_t)last_slash;
        if (dlen >= VFS_PATH_MAX) dlen = VFS_PATH_MAX - 1;
        memcpy(parent_path, path, dlen);
        parent_path[dlen] = '\0';
        strncpy(dir_name, path + last_slash + 1, VFS_NAME_MAX - 1);
        dir_name[VFS_NAME_MAX - 1] = '\0';
    }

    vfs_node_t* parent = vfs_namei(parent_path);
    if (!parent) return -1;

    return (long)vfs_create(parent, dir_name, VFS_DIRECTORY);
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
        case SYS_kill: return sys_kill(a1);
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