#include "include/kernel/keyboard.h"
#include "include/kernel/io.h"
#include "include/kernel/irq.h"
#include <stdint.h>

// dk-latin1 keyboard mapping (Set 1 scancodes)
// Notes:
    //  - Extended codes used by this font mapping: å=0x86, Å=0x8F, æ=0x91, Æ=0x92, ø=0x9B, Ø=0x9D.
//  - ISO 102nd key (< > |) is scancode 86 (0x56). Dead keys not implemented.
static const unsigned char dk_map[128] = {
    /* 00-0F */ 0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '+', '\b',
    /* 10-1F */ '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', 0x86, '^', '\n',
               0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 0x91, 0x9B, '`',
    /* 20-2F */ 0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '-', 0,
    /* 30-3F */ '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static const unsigned char dk_shift_map[128] = {
    /* 00-0F */ 0, 27, '!', '"', '#', 0xA4, '%', '&', '/', '(', ')', '=', '_', '?', '\b',
    /* 10-1F */ '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', 0x8F, 0xF9, '\n',
               0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', 0x92, 0x9D, '~',
    /* 20-2F */ 0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', ';', ':', '_', 0,
    /* 30-3F */ '*', 0, ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

// AltGr (Right-Alt, E0 38) mapping for common dk-latin1 combos
static const unsigned char dk_altgr_map[128] = {
    /* 00-0F */ 0, 0, 0, '@', 0x9C, '$', 0, 0, '{', '[', ']', '}', '\\', 0, 0,
    /* 10-1F */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,      0,
               0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,              0,
    /* 20-2F */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,             0,
    /* 30-3F */ '*', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,           0, 0, 0
};

// ISO 102nd key (< > |) at scancode 86
// We'll map it explicitly when translating scancodes.

#define KEYBOARD_BUFFER_SIZE 256

static char keyboard_buffer[KEYBOARD_BUFFER_SIZE];
static unsigned int kb_buffer_start = 0;
static unsigned int kb_buffer_end = 0;
static unsigned int shift_pressed = 0;
static unsigned int ctrl_pressed = 0;
static unsigned int alt_pressed = 0;     // Left Alt
static unsigned int altgr_pressed = 0;   // Right Alt (AltGr)
static unsigned int e0_prefix = 0;       // Set when previous byte was 0xE0
static int keyboard_initialized = 0;

// Forward declaration for the buffer enqueue function
static void keyboard_buffer_enqueue(char c);
static void keyboard_buffer_enqueue_str(const char* s);

/**
 * Keyboard interrupt handler (IRQ1)
 */
static void keyboard_irq_handler(struct registers* regs) {
    (void)regs;
    uint8_t scancode = inb(0x60);

    if (scancode == 0xE0) {
        e0_prefix = 1;
        return;
    }

    int release = scancode & 0x80;
    uint8_t code = scancode & 0x7F;

    // Handle modifier state (including E0 Right-Alt)
    if (release) {
        if (e0_prefix && code == 0x38) { altgr_pressed = 0; e0_prefix = 0; return; }
        if (code == 0x2A || code == 0x36) shift_pressed = 0;
        else if (code == 0x1D)            ctrl_pressed  = 0;
        else if (code == 0x38)            alt_pressed   = 0;
        e0_prefix = 0;
        return;
    } else {
        if (e0_prefix && code == 0x38) { altgr_pressed = 1; e0_prefix = 0; return; }
        if (code == 0x2A || code == 0x36) { shift_pressed = 1; e0_prefix = 0; return; }
        if (code == 0x1D) { ctrl_pressed = 1; e0_prefix = 0; return; }
        if (code == 0x38) { alt_pressed  = 1; e0_prefix = 0; return; }
    }

    // Handle E0-extended non-modifiers (arrows, etc.)
    if (e0_prefix) {
        switch (code) {
            case 0x48: keyboard_buffer_enqueue_str("\x1B[A"); break; // Up
            case 0x50: keyboard_buffer_enqueue_str("\x1B[B"); break; // Down
            case 0x4D: keyboard_buffer_enqueue_str("\x1B[C"); break; // Right
            case 0x4B: keyboard_buffer_enqueue_str("\x1B[D"); break; // Left
            // (optional) Home/End/Delete:
            // case 0x47: keyboard_buffer_enqueue_str("\x1B[H"); break; // Home
            // case 0x4F: keyboard_buffer_enqueue_str("\x1B[F"); break; // End
            // case 0x53: keyboard_buffer_enqueue_str("\x1B[3~"); break; // Delete
            default: break;
        }
        e0_prefix = 0;
        return;
    }

    // Non-modifier keys (non-E0)
    e0_prefix = 0;
    if (code >= 128) return;

    char c = 0;
    if (code == 86) {
        if (altgr_pressed)      c = '|';
        else if (shift_pressed) c = '>';
        else                    c = '<';
    } else {
        if (altgr_pressed && dk_altgr_map[code]) c = dk_altgr_map[code];
        else if (shift_pressed)                  c = dk_shift_map[code];
        else                                     c = dk_map[code];
    }

    if (c) {
        if (ctrl_pressed && c >= 'a' && c <= 'z') c = c - 'a' + 1;
        keyboard_buffer_enqueue(c);
    }
}

/**
 * Add a character to the keyboard buffer
 */
static void keyboard_buffer_enqueue(char c) {
    unsigned int next_end = (kb_buffer_end + 1) % KEYBOARD_BUFFER_SIZE;
    if (next_end == kb_buffer_start) return;
    keyboard_buffer[kb_buffer_end] = c;
    kb_buffer_end = next_end;
}

// Add this helper to enqueue strings (for ESC sequences)
static void keyboard_buffer_enqueue_str(const char* s) {
    while (*s) keyboard_buffer_enqueue(*s++);
}

/**
 * Initialize the keyboard driver - now interrupt based
 */
void keyboard_initialize(void) {
    kb_buffer_start = kb_buffer_end = 0;
    shift_pressed = ctrl_pressed = alt_pressed = 0;
    
    irq_install_handler(1, keyboard_irq_handler);
    
    // Mark as initialized
    keyboard_initialized = 1;
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
