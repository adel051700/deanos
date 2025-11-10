#ifndef _KERNEL_KEYBOARD_H
#define _KERNEL_KEYBOARD_H

// Initialize keyboard driver
void keyboard_initialize(void);

// Polling helpers used by kernel.c
int  keyboard_data_available(void);
char keyboard_getchar(void);

#endif