#include "include/kernel/syscall.h"
#include "include/kernel/interrupt.h"
#include "include/kernel/tty.h"
#include "include/kernel/pit.h"
#include "include/kernel/log.h"
#include <stddef.h>
#include <stdint.h>

static long sys_write(uint32_t fd, const char* buf, size_t len) {
    if (!buf || len == 0) return 0;
    // Console only for now (stdout/stderr)
    if (fd == 1 || fd == 2) {
        terminal_write(buf, len);
        return (long)len;
    }
    return -1;
}

static long sys_time(uint32_t* out) {
    uint64_t seconds = pit_get_uptime_ms() / 1000;
    if (out) *out = (uint32_t)seconds;
    return (long)seconds;
}

static long sys_exit(uint32_t status) {
    (void)status;
    // No process model yet; halt the CPU as a placeholder.
    // In the future: mark current task exited and schedule.
    while (1) { __asm__ __volatile__("cli; hlt"); }
    return 0;
}

static long syscall_dispatch(uint32_t num, uint32_t a1, uint32_t a2, uint32_t a3) {
    (void)a3;
    switch (num) {
        case SYS_write: return sys_write(a1, (const char*)a2, (size_t)a3);
        case SYS_time:  return sys_time((uint32_t*)a1);
        case SYS_exit:  return sys_exit(a1);
        default:        return -38; // ENOSYS
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