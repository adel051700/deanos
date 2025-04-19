#include <kernel/keyboard.h>
#include <kernel/io.h>
#include <kernel/tty.h>
#include <stdint.h>

// US keyboard layout mapping scancode -> ASCII
static const unsigned char us_keyboard_map[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    '-', 0, 0, 0, '+', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

// Shift-modified keyboard layout
static const unsigned char us_keyboard_map_shifted[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    '-', 0, 0, 0, '+', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

#define KEYBOARD_BUFFER_SIZE 256

static char keyboard_buffer[KEYBOARD_BUFFER_SIZE];
static unsigned int kb_buffer_start = 0;
static unsigned int kb_buffer_end = 0;
static unsigned int shift_pressed = 0;
static unsigned int ctrl_pressed = 0;
static unsigned int alt_pressed = 0;
static int keyboard_initialized = 0;

/**
 * Add a character to the keyboard buffer
 */
static void keyboard_buffer_enqueue(char c) {
    unsigned int next_end = (kb_buffer_end + 1) % KEYBOARD_BUFFER_SIZE;
    
    // If buffer is full, discard the character
    if (next_end == kb_buffer_start) {
        return;
    }
    
    keyboard_buffer[kb_buffer_end] = c;
    kb_buffer_end = next_end;
}

/**
 * Initialize the keyboard driver - polling based approach with no interrupts
 */
void keyboard_initialize(void) {
    // Clear keyboard buffer
    kb_buffer_start = 0;
    kb_buffer_end = 0;
    shift_pressed = 0;
    ctrl_pressed = 0;
    alt_pressed = 0;
    
    // Flush any pending keyboard data
    while (inb(0x64) & 1) {
        inb(0x60);
    }
    
    // Mark as initialized
    keyboard_initialized = 1;
    
    // Print initialization message
    terminal_writestring("Keyboard initialized (polling mode).\n");
}

/**
 * Get a character from the keyboard buffer if available
 * Returns 0 if no character is available
 */
char keyboard_getchar(void) {
    if (kb_buffer_start == kb_buffer_end) {
        return 0; // Buffer is empty
    }
    
    char c = keyboard_buffer[kb_buffer_start];
    kb_buffer_start = (kb_buffer_start + 1) % KEYBOARD_BUFFER_SIZE;
    return c;
}

/**
 * Check if keyboard buffer has data
 */
int keyboard_data_available(void) {
    return kb_buffer_start != kb_buffer_end;
}

/**
 * Check if keyboard has pending data and process it
 * This polling-based approach is more reliable
 */
void keyboard_update(void) {
    // Check if there's data in the keyboard controller
    if (inb(0x64) & 1) {
        // Read the scancode
        uint8_t scancode = inb(0x60);
        
        // Process key press/release
        if (scancode & 0x80) {
            // Key release (bit 7 set)
            scancode &= 0x7F; // Remove the highest bit to get the actual scancode
            
            // Handle modifier key releases
            if (scancode == 0x2A || scancode == 0x36) { // Left or right shift
                shift_pressed = 0;
            } else if (scancode == 0x1D) { // Left control
                ctrl_pressed = 0;
            } else if (scancode == 0x38) { // Left alt
                alt_pressed = 0;
            }
        } else {
            // Key press
            if (scancode == 0x2A || scancode == 0x36) { // Left or right shift
                shift_pressed = 1;
            } else if (scancode == 0x1D) { // Left control
                ctrl_pressed = 1;
            } else if (scancode == 0x38) { // Left alt
                alt_pressed = 1;
            } else if (scancode < 128) {
                // Regular key
                char c;
                if (shift_pressed) {
                    c = us_keyboard_map_shifted[scancode];
                } else {
                    c = us_keyboard_map[scancode];
                }
                
                // If it's a valid character, add to buffer
                if (c) {
                    // Process control characters
                    if (ctrl_pressed && c >= 'a' && c <= 'z') {
                        c = c - 'a' + 1; // Control codes: Ctrl+A = 1, etc.
                    }
                    keyboard_buffer_enqueue(c);
                }
            }
        }
    }
}