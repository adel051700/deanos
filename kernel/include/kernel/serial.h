#ifndef _KERNEL_SERIAL_H
#define _KERNEL_SERIAL_H

#include <stddef.h>

void serial_initialize(void);
int serial_is_available(void);
void serial_write_char(char c);
void serial_write_buf(const char* s, size_t len);

#endif

