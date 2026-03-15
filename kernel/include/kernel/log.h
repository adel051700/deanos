#ifndef _KERNEL_LOG_H
#define _KERNEL_LOG_H

#include <stddef.h>

void klog_init(void);
void klog(const char* s);     // append string + newline to ring and tty
void klog_write(const char* s, size_t len); // raw (no newline)
void klog_dump(void);         // dump ring oldest->newest to tty
void klog_clear(void);        // clear ring buffer

#endif

