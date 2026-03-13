#include "include/kernel/syscall.h"
#include "include/kernel/interrupt.h"
#include "include/kernel/keyboard.h"
#include "include/kernel/tty.h"
#include "include/kernel/pit.h"
#include "include/kernel/task.h"
#include "include/kernel/vfs.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static long sys_write(uint32_t fd, const char* buf, size_t len) {
    if (!buf || len == 0) return 0;
    /* Console for stdout/stderr */
    if (fd == 1 || fd == 2) {
        terminal_write(buf, len);
        return (long)len;
    }
    /* VFS file descriptors */
    int32_t ret = vfs_fd_write((int)fd, (const uint8_t*)buf, (uint32_t)len);
    return (long)ret;
}

static long sys_read(uint32_t fd, char* buf, size_t len) {
    if (!buf || len == 0) return 0;
    if (fd == 0) {
        size_t nread = 0;
        while (nread < len && keyboard_data_available()) {
            buf[nread++] = keyboard_getchar();
        }
        return (long)nread;
    }
    int32_t ret = vfs_fd_read((int)fd, (uint8_t*)buf, (uint32_t)len);
    return (long)ret;
}

static long sys_time(uint32_t* out) {
    uint64_t seconds = pit_get_uptime_ms() / 1000;
    if (out) *out = (uint32_t)seconds;
    return (long)seconds;
}

static long sys_exit(uint32_t status) {
    (void)status;
    task_exit();       /* marks task DEAD and yields — never returns */
    return 0;
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

static long syscall_dispatch(uint32_t num, uint32_t a1, uint32_t a2, uint32_t a3) {
    switch (num) {
        case SYS_write: return sys_write(a1, (const char*)a2, (size_t)a3);
        case SYS_read:  return sys_read(a1, (char*)a2, (size_t)a3);
        case SYS_time:  return sys_time((uint32_t*)a1);
        case SYS_exit:  return sys_exit(a1);
        case SYS_open:  return sys_open((const char*)a1, a2);
        case SYS_close: return sys_close(a1);
        case SYS_fstat: return sys_fstat(a1, (vfs_stat_t*)a2);
        case SYS_mkdir: return sys_mkdir((const char*)a1);
        default:        return -38; /* ENOSYS */
    }
}

static void syscall_isr(struct registers* r) {
    // ABI: eax=num, ebx=a1, ecx=a2, edx=a3
    uint32_t num = r->eax;
    uint32_t a1  = r->ebx;
    uint32_t a2  = r->ecx;
    uint32_t a3  = r->edx;
    long ret = syscall_dispatch(num, a1, a2, a3);
    r->eax = (uint32_t)ret;
}

void syscall_initialize(void) {
    register_interrupt_handler(128, syscall_isr); // int 0x80
    register_interrupt_handler(129, syscall_isr); // int 0x81
}