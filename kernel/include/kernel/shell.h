#ifndef _KERNEL_SHELL_H
#define _KERNEL_SHELL_H

#include <stddef.h>

// Initialize the shell
void shell_initialize(void);

// Process incoming keypress
void shell_process_char(char c);

// Optional polling helper (if used)
void shell_update(void);

// Mark that asynchronous console output occurred while shell input may be active.
void shell_mark_tty_async_output(void);

// Print async task output on its own line and restore prompt/input immediately.
void shell_write_async_output(const char* data, size_t len);

#endif