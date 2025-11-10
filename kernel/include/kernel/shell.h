#ifndef _KERNEL_SHELL_H
#define _KERNEL_SHELL_H

// Initialize the shell
void shell_initialize(void);

// Process incoming keypress
void shell_process_char(char c);

// Optional polling helper (if used)
void shell_update(void);

#endif