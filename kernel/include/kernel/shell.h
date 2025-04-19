#ifndef _KERNEL_SHELL_H
#define _KERNEL_SHELL_H

// Initialize the shell
void shell_initialize(void);

// Process incoming keypress
void shell_process_char(char c);

// Execute a command string
void shell_execute_command(const char* command);

// Update shell state - check for and process input
void shell_update(void);

#endif