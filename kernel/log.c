#include "include/kernel/log.h"
#include "include/kernel/serial.h"
#include "include/kernel/tty.h"

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

size_t klog_read(char* out, size_t out_len) {
    if (!out || out_len == 0) return 0;
    size_t end = head;
    size_t window = (end > KLOG_BUF_SZ) ? KLOG_BUF_SZ : end;  /* bytes currently retained */
    size_t start = end - window;                              /* oldest retained byte */
    size_t count = (window < out_len) ? window : out_len;      /* how many we can actually copy */
    for (size_t i = 0; i < count; ++i) {
        out[i] = buf[(start + i) % KLOG_BUF_SZ];
    }
    return count;
}

void klog_clear(void) {
    head = 0;
}
