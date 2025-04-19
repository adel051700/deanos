#ifndef _KERNEL_KEYBOARD_H
#define _KERNEL_KEYBOARD_H

// Initialize keyboard driver
void keyboard_initialize(void);

// Check if keyboard data is available
int keyboard_data_available(void);

// Get a character from keyboard buffer
char keyboard_getchar(void);

// Update keyboard state (check for new key presses)
void keyboard_update(void);

#endif