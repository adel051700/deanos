#include "include/kernel/log.h"
#include "include/kernel/tty.h"
#include "../libc/include/string.h"

#define KLOG_BUF_SZ 4096
static char buf[KLOG_BUF_SZ];
static volatile size_t head = 0;

void klog_init(void) {
    head = 0;
}

void klog_write(const char* s, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        buf[head % KLOG_BUF_SZ] = s[i];
        head++;
    }
    // Also mirror to terminal if ready; tty guards itself
    terminal_write(s, len);
}

void klog(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    klog_write(s, n);
    klog_write("\n", 1);
}