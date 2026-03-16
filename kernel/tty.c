    #include <stdint.h>
    #include <stddef.h>
    #include <string.h>
    #include "include/kernel/tty.h"

    #define TERM_COLS 128
    #define TERM_ROWS 48

    void framebuffer_clear(uint32_t color);
    void framebuffer_drawchar(uint32_t x, uint32_t y, char c, uint32_t color, uint32_t bg_color, int scale);
    uint32_t framebuffer_width(void);
    uint32_t framebuffer_height(void);

    static char term_buffer[TERM_ROWS][TERM_COLS];
    static uint8_t term_scale[TERM_ROWS][TERM_COLS];
    static uint16_t line_positions[TERM_ROWS];
    static int term_cursor_x, term_cursor_y;
    static uint32_t term_color;
    static uint32_t term_bg;
    static uint32_t current_scale;
    static int cursor_enabled = 0;
    static int cursor_visible = 1;
    static uint32_t cursor_blink_counter = 0;
    static int terminal_ready = 0;
    #define CURSOR_BLINK_INTERVAL_TICKS 50

    static void calculate_line_positions(void) {
        uint16_t y_pos = 0;
        line_positions[0] = 0;

        for (uint32_t y = 0; y < TERM_ROWS - 1; y++) {
            uint8_t line_scale = 1;
            for (uint32_t x = 0; x < TERM_COLS; x++) {
                if (term_scale[y][x] > line_scale)
                    line_scale = term_scale[y][x];
            }

            y_pos += 16 * line_scale;
            line_positions[y + 1] = y_pos;
        }
    }

    static inline int cursor_in_bounds(void) {
        return (term_cursor_x >= 0 && term_cursor_x < (int)TERM_COLS &&
                term_cursor_y >= 0 && term_cursor_y < (int)TERM_ROWS);
    }

    static void draw_cursor(void) {
        if (!terminal_ready || !cursor_enabled) return;
        if (!cursor_visible) return;
        if (!cursor_in_bounds()) return;

        uint32_t y_pos = line_positions[term_cursor_y];
        uint8_t scale = term_scale[term_cursor_y][term_cursor_x];

        uint32_t x_pos = 0;
        for (uint32_t i = 0; i < (uint32_t)term_cursor_x; i++) {
            x_pos += 8u * term_scale[term_cursor_y][i];
        }

        framebuffer_drawchar(x_pos, y_pos, (char)0xDB, term_color, term_bg, scale);
    }

    static void clear_cursor(void) {
        if (!terminal_ready || !cursor_enabled) return;
        if (!cursor_visible) return;
        if (!cursor_in_bounds()) return;

        uint32_t y_pos = line_positions[term_cursor_y];
        uint8_t scale = term_scale[term_cursor_y][term_cursor_x];
        char c = term_buffer[term_cursor_y][term_cursor_x];

        uint32_t x_pos = 0;
        for (uint32_t i = 0; i < (uint32_t)term_cursor_x; i++) {
            x_pos += 8u * term_scale[term_cursor_y][i];
        }

        framebuffer_drawchar(x_pos, y_pos, c, term_color, term_bg, scale);
    }

    void terminal_cursor_blink_tick(void) {
        if (!terminal_ready || !cursor_enabled) return;
        cursor_blink_counter++;
        if (cursor_blink_counter >= CURSOR_BLINK_INTERVAL_TICKS) {
            cursor_blink_counter = 0;
            if (cursor_visible) {
                clear_cursor();
                cursor_visible = 0;
            } else {
                cursor_visible = 1;
                draw_cursor();
            }
        }
    }

    void terminal_enable_cursor(void) {
        cursor_enabled = 1;
        cursor_visible = 1;
        cursor_blink_counter = 0;
        draw_cursor();
    }

    static void terminal_redraw(void) {
        framebuffer_clear(term_bg);

        for (uint32_t y = 0; y < TERM_ROWS; y++) {
            uint32_t y_pos = line_positions[y];
            uint32_t x_pos = 0;

            for (uint32_t x = 0; x < TERM_COLS; x++) {
                char c = term_buffer[y][x];
                uint8_t scale = term_scale[y][x];

                if (c != ' ') {
                    framebuffer_drawchar(x_pos, y_pos, c, term_color, term_bg, scale);
                }
                x_pos += 8u * scale;
            }
        }

        if (cursor_enabled && cursor_visible) {
            draw_cursor();
        }
    }

    void terminal_scroll(void) {
        for (uint32_t y = 1; y < TERM_ROWS; y++) {
            for (uint32_t x = 0; x < TERM_COLS; x++) {
                term_buffer[y - 1][x] = term_buffer[y][x];
                term_scale[y - 1][x] = term_scale[y][x];
            }
        }

        for (uint32_t x = 0; x < TERM_COLS; x++) {
            term_buffer[TERM_ROWS - 1][x] = ' ';
            term_scale[TERM_ROWS - 1][x] = 1;
        }

        calculate_line_positions();
        terminal_redraw();
    }

    static inline void clamp_or_scroll_cursor(void) {
        while (term_cursor_y >= (int)TERM_ROWS) {
            terminal_scroll();
            term_cursor_y = (int)TERM_ROWS - 1;
        }
        // Scroll when the cursor row would extend beyond the physical screen
        uint32_t fb_h = framebuffer_height();
        while (term_cursor_y > 0 &&
               (line_positions[term_cursor_y] + 16u * current_scale) > fb_h) {
            terminal_scroll();
            term_cursor_y--;
        }
        if (term_cursor_y < 0) term_cursor_y = 0;
        if (term_cursor_x < 0) term_cursor_x = 0;
        if (term_cursor_x >= (int)TERM_COLS) term_cursor_x = (int)TERM_COLS - 1;
    }

    void terminal_initialize(void) {
        term_cursor_x = 1;
        term_cursor_y = 0;
        term_color = 0x00FF00;
        term_bg = 0x000000;
        current_scale = 1;
        cursor_visible = 1;
        cursor_enabled = 0;
        terminal_ready = 0;

        for (uint32_t y = 0; y < TERM_ROWS; y++) {
            for (uint32_t x = 0; x < TERM_COLS; x++) {
                term_buffer[y][x] = ' ';
                term_scale[y][x] = 1;
            }
        }

        calculate_line_positions();
        framebuffer_clear(term_bg);
        terminal_ready = 1;
    }

    void terminal_putchar(char c) {
        if (!terminal_ready) return;

        if (cursor_enabled && cursor_visible) {
            clear_cursor();
        }

        if (c == '\n') {
            term_cursor_x = 1;
            term_cursor_y += 1;

            clamp_or_scroll_cursor();
            calculate_line_positions();
        } else if (c == '\r') {
            term_cursor_x = 0;
        } else if (c == '\b') {
            if (term_cursor_y >= 0 && term_cursor_y < (int)TERM_ROWS) {
                if (term_cursor_x > 0) {
                    term_cursor_x--;
                    term_buffer[term_cursor_y][term_cursor_x] = ' ';

                    uint32_t y_pos = line_positions[term_cursor_y];
                    uint32_t x_pos = 0;
                    for (uint32_t i = 0; i < (uint32_t)term_cursor_x; i++) {
                        x_pos += 8u * term_scale[term_cursor_y][i];
                    }

                    framebuffer_drawchar(x_pos, y_pos, ' ', term_color, term_bg, current_scale);
                } else if (term_cursor_y > 0) {
                    term_cursor_y--;
                    term_cursor_x = (int)TERM_COLS - 1;
                    while (term_cursor_x > 0 && term_buffer[term_cursor_y][term_cursor_x] == ' ') {
                        term_cursor_x--;
                    }
                    if (term_buffer[term_cursor_y][term_cursor_x] != ' ') {
                        term_cursor_x++;
                    }
                }
            }
        } else {
            if (term_cursor_x >= (int)TERM_COLS) {
                term_cursor_x = 0;
                term_cursor_y += 1;
                clamp_or_scroll_cursor();
                calculate_line_positions();
            }

            if (cursor_in_bounds()) {
                term_buffer[term_cursor_y][term_cursor_x] = c;
                term_scale[term_cursor_y][term_cursor_x] = (uint8_t)current_scale;

                uint32_t y_pos = line_positions[term_cursor_y];
                uint32_t x_pos = 0;
                for (uint32_t i = 0; i < (uint32_t)term_cursor_x; i++) {
                    x_pos += 8u * term_scale[term_cursor_y][i];
                }

                framebuffer_drawchar(x_pos, y_pos, c, term_color, term_bg, (int)current_scale);
            }

            term_cursor_x++;
            if (term_cursor_x >= (int)TERM_COLS) {
                term_cursor_x = 0;
                term_cursor_y += 1;
                clamp_or_scroll_cursor();
                calculate_line_positions();
            }
        }

        if (cursor_enabled && cursor_visible) {
            draw_cursor();
        }
    }

    void terminal_write(const char* data, size_t size) {
        for (size_t i = 0; i < size; i++)
            terminal_putchar(data[i]);
    }

    void terminal_writestring(const char* data) {
        terminal_write(data, strlen(data));
    }

    void terminal_move_cursor_left(void) {
        if (!terminal_ready) return;
        if (cursor_enabled && cursor_visible) clear_cursor();
        if (term_cursor_x > 0) {
            term_cursor_x--;
        } else if (term_cursor_y > 0) {
            term_cursor_y--;
            /* Find last used column on the previous row */
            term_cursor_x = (int)TERM_COLS - 1;
            while (term_cursor_x > 0 && term_buffer[term_cursor_y][term_cursor_x] == ' ')
                term_cursor_x--;
            if (term_buffer[term_cursor_y][term_cursor_x] != ' ')
                term_cursor_x++;
        }
        if (cursor_enabled && cursor_visible) draw_cursor();
    }

    void terminal_move_cursor_right(void) {
        if (!terminal_ready) return;
        if (cursor_enabled && cursor_visible) clear_cursor();
        term_cursor_x++;
        if (term_cursor_x >= (int)TERM_COLS) {
            term_cursor_x = 0;
            term_cursor_y++;
            clamp_or_scroll_cursor();
            calculate_line_positions();
        }
        if (cursor_enabled && cursor_visible) draw_cursor();
    }

    void terminal_setcolor(uint32_t color) { term_color = color; }
    void terminal_setbackground(uint32_t color) { term_bg = color; }
    void terminal_setscale(uint32_t scale) { current_scale = scale; }

    uint32_t terminal_get_color(void) { return term_color; }