#include "include/kernel/log.h"
#include "include/kernel/serial.h"
#include "include/kernel/tty.h"

#define KLOG_BUF_SZ 4096
static char buf[KLOG_BUF_SZ];
static volatile size_t head = 0;

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
    klog_write(s, n);
    klog_write("\n", 1);
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
