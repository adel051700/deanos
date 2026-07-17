#ifndef _KERNEL_LOG_H
#define _KERNEL_LOG_H

#include <stddef.h>

#define KLOG_BUF_SZ 4096

void klog_init(void);
void klog(const char* s);     // append string + newline to ring and tty
void klog_clear(void);        // clear ring buffer
size_t klog_read(char* out, size_t out_len); // copy up to out_len ring bytes (oldest->newest) into out, return bytes copied

#endif

