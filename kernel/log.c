#include "include/kernel/log.h"
#include "include/kernel/serial.h"
#include "include/kernel/tty.h"

#define KLOG_BUF_SZ 4096
static char buf[KLOG_BUF_SZ];
static volatile size_t head = 0;

static int starts_with(const char* s, const char* prefix) {
    if (!s || !prefix) return 0;
    while (*prefix) {
        if (*s++ != *prefix++) return 0;
    }
    return 1;
}

static int suppress_terminal_echo(const char* s) {
    return starts_with(s, "ata:") ||
           starts_with(s, "mbr:") ||
           starts_with(s, "minfs:") ||
           starts_with(s, "storage:");
}

void klog_init(void) {
    head = 0;
}

void klog_write(const char* s, size_t len) {
    if (!s || len == 0) return;

    for (size_t i = 0; i < len; ++i) {
        buf[head % KLOG_BUF_SZ] = s[i];
        head++;
    }
    /* Mirror to terminal and serial debug port. */
    terminal_write(s, len);
    serial_write_buf(s, len);
}

void klog(const char* s) {
    size_t n = 0;
    while (s[n]) n++;

    /* Always keep kernel logs in ring buffer and serial debug output. */
    for (size_t i = 0; i < n; ++i) {
        buf[head % KLOG_BUF_SZ] = s[i];
        head++;
    }
    buf[head % KLOG_BUF_SZ] = '\n';
    head++;

    if (!suppress_terminal_echo(s)) {
        terminal_write(s, n);
        terminal_write("\n", 1);
    }

    serial_write_buf(s, n);
    serial_write_buf("\n", 1);
}

void klog_dump(void) {
    size_t end = head;
    size_t count = (end > KLOG_BUF_SZ) ? KLOG_BUF_SZ : end;
    size_t start = end - count;

    for (size_t i = 0; i < count; ++i) {
        char c = buf[(start + i) % KLOG_BUF_SZ];
        terminal_write(&c, 1);
    }
}

void klog_clear(void) {
    head = 0;
}
