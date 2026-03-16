#ifndef _KERNEL_TTY_H
#define _KERNEL_TTY_H
// Cursor blink every 500ms at 100Hz PIT
#define CURSOR_BLINK_INTERVAL_TICKS 50

#include <stddef.h>
#include <stdint.h>

void terminal_initialize(void);
void terminal_putchar(char c);
void terminal_write(const char* data, size_t size);
void terminal_writestring(const char* data);
void terminal_setcolor(uint32_t color);
void terminal_setbackground(uint32_t color);
void terminal_setscale(uint32_t scale);
void terminal_enable_cursor(void);
void terminal_cursor_blink_tick(void);
uint32_t terminal_get_color(void);
void terminal_move_cursor_left(void);
void terminal_move_cursor_right(void);
#endif