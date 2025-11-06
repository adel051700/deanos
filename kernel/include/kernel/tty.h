#ifndef _KERNEL_TTY_H
#define _KERNEL_TTY_H

#include <stddef.h>
#include <stdint.h>

void terminal_initialize(void);
void terminal_putchar(char c);
void terminal_write(const char* data, size_t size);
void terminal_writestring(const char* data);
void terminal_setcolor(uint32_t color);
void terminal_setbackground(uint32_t color);
void terminal_setscale(uint32_t scale);
void terminal_update_cursor(void);
void terminal_enable_cursor(void);

#endif