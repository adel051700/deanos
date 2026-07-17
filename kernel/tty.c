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
    static int terminal_controlling_sid = 0;
    static int terminal_foreground_pgid = 0;
    #define CURSOR_BLINK_INTERVAL_TICKS 50

    /* Output-side ANSI escape parser: ESC[2J (clear), ESC[C/ESC[D (cursor
     * move), ESC[0m / ESC[38;2;R;G;Bm / ESC[48;2;R;G;Bm (SGR reset/color).
     * Persistent across write() calls because sequences may arrive split. */
    typedef enum { TTY_ESC_IDLE = 0, TTY_ESC_GOT_ESC, TTY_ESC_IN_CSI } tty_esc_state_t;
    static tty_esc_state_t tty_esc_state = TTY_ESC_IDLE;
    #define TTY_CSI_MAX_PARAMS 5
    static uint32_t tty_csi_nums[TTY_CSI_MAX_PARAMS];
    static uint32_t tty_csi_num_count = 0;
    static uint32_t tty_csi_cur = 0;
    static int      tty_csi_cur_has_digit = 0;

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

    /* ESC[2J: clear the text buffer and home the cursor. Colors and the
     * controlling sid / foreground pgid are deliberately left untouched. */
    static void terminal_clear_and_home(void) {
        for (uint32_t y = 0; y < TERM_ROWS; y++) {
            for (uint32_t x = 0; x < TERM_COLS; x++) {
                term_buffer[y][x] = ' ';
                term_scale[y][x] = 1;
            }
        }
        term_cursor_x = 1; /* matches terminal_initialize's home column */
        term_cursor_y = 0;
        calculate_line_positions();
        framebuffer_clear(term_bg);
        if (cursor_enabled && cursor_visible) draw_cursor();
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
        terminal_controlling_sid = 0;
        terminal_foreground_pgid = 0;

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

        if (tty_esc_state == TTY_ESC_GOT_ESC) {
            if (c == '[') {
                tty_esc_state = TTY_ESC_IN_CSI;
                tty_csi_num_count = 0;
                tty_csi_cur = 0;
                tty_csi_cur_has_digit = 0;
            } else {
                tty_esc_state = TTY_ESC_IDLE; /* unrecognized: swallow */
            }
            return;
        }
        if (tty_esc_state == TTY_ESC_IN_CSI) {
            if (c >= 0x40 && c <= 0x7E) { /* final byte */
                if (tty_csi_cur_has_digit || tty_csi_num_count > 0) {
                    if (tty_csi_num_count < TTY_CSI_MAX_PARAMS) {
                        tty_csi_nums[tty_csi_num_count++] = tty_csi_cur;
                    }
                }
                if (c == 'J' && tty_csi_num_count == 1 && tty_csi_nums[0] == 2) {
                    terminal_clear_and_home();
                } else if (c == 'C' && tty_csi_num_count == 0) {
                    terminal_move_cursor_right();
                } else if (c == 'D' && tty_csi_num_count == 0) {
                    terminal_move_cursor_left();
                } else if (c == 'm') {
                    if (tty_csi_num_count == 1 && tty_csi_nums[0] == 0) {
                        terminal_setcolor(0x00FF00u);
                        terminal_setbackground(0x000000u);
                    } else if (tty_csi_num_count == 5 && tty_csi_nums[0] == 38 &&
                               tty_csi_nums[1] == 2) {
                        terminal_setcolor(((tty_csi_nums[2] & 0xFFu) << 16) |
                                          ((tty_csi_nums[3] & 0xFFu) << 8) |
                                          (tty_csi_nums[4] & 0xFFu));
                    } else if (tty_csi_num_count == 5 && tty_csi_nums[0] == 48 &&
                               tty_csi_nums[1] == 2) {
                        terminal_setbackground(((tty_csi_nums[2] & 0xFFu) << 16) |
                                               ((tty_csi_nums[3] & 0xFFu) << 8) |
                                               (tty_csi_nums[4] & 0xFFu));
                    }
                    /* any other SGR code (16-color, bold, etc.): swallow */
                }
                tty_esc_state = TTY_ESC_IDLE;
                return;
            }
            if (c >= '0' && c <= '9') {
                tty_csi_cur = tty_csi_cur * 10u + (uint32_t)(c - '0');
                tty_csi_cur_has_digit = 1;
                return;
            }
            if (c == ';') {
                if (tty_csi_num_count < TTY_CSI_MAX_PARAMS) {
                    tty_csi_nums[tty_csi_num_count++] = tty_csi_cur;
                }
                tty_csi_cur = 0;
                tty_csi_cur_has_digit = 0;
                return;
            }
            /* any other intermediate byte: ignored, but keep consuming
             * until the final byte arrives (same "drop but keep consuming"
             * policy the parser already used for params beyond its cap). */
            return;
        }
        if (c == '\x1B') {
            tty_esc_state = TTY_ESC_GOT_ESC;
            return;
        }

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
    void terminal_setbackground(uint32_t color) {
        term_bg = color;
        if (terminal_ready) {
            terminal_redraw();
        }
    }
    void terminal_setscale(uint32_t scale) { current_scale = scale; }

    uint32_t terminal_get_color(void) { return term_color; }
    uint32_t terminal_get_background(void) { return term_bg; }

    int terminal_set_foreground_pgid(int pgid) {
        if (pgid < 0) return -1;
        terminal_foreground_pgid = pgid;
        return 0;
    }

    int terminal_get_foreground_pgid(void) {
        return terminal_foreground_pgid;
    }

    int terminal_set_controlling_sid(int sid) {
        if (sid < 0) return -1;
        if (terminal_controlling_sid != 0 && terminal_controlling_sid != sid) return -1;
        terminal_controlling_sid = sid;
        return 0;
    }

    int terminal_get_controlling_sid(void) {
        return terminal_controlling_sid;
    }

    /* Detach the terminal from its controlling session (init calls this
     * when the session leader exits, so a respawned shell can claim it —
     * terminal_set_controlling_sid refuses to overwrite a live claim). */
    void terminal_release_controlling(void) {
        terminal_controlling_sid = 0;
        terminal_foreground_pgid = 0;
    }

    #define TTY_LINE_MAX 256
    /* ponytail: single global, not per-process, not saved/restored across
     * exec — matches this file's other console-wide state (foreground
     * pgid, controlling sid). /bin/sh flips this to raw at startup and
     * nothing ever flips it back, so every child it launches inherits raw
     * mode too; canonical mode is unreachable in the shipped system until
     * something explicitly requests it and restores raw afterward. */
    static int    tty_canonical_mode = 1; /* default: canonical */
    static char   tty_line_buf[TTY_LINE_MAX];
    static size_t tty_line_len;      /* bytes typed so far / ready-line length once ready */
    static int    tty_line_ready;    /* 1 once a line (or EOF) is complete, until fully drained */
    static size_t tty_line_consumed; /* drain offset into tty_line_buf while tty_line_ready */

    int terminal_set_canonical(int enable) {
        int prev = tty_canonical_mode;
        tty_canonical_mode = enable ? 1 : 0;
        return prev;
    }

    int terminal_get_canonical(void) {
        return tty_canonical_mode;
    }

    /* Feeds one raw input byte through the canonical-mode line discipline:
     * echoes it and handles ERASE (\b), KILL (^U), EOF (^D), and INTR (^C —
     * SIGINT delivery itself already happens unconditionally in
     * keyboard.c's IRQ handler; this only resets the in-progress line).
     * Returns nonzero once a line becomes ready for tty_drain_ready(). */
    int tty_process_input_byte(char c) {
        if (c == '\n' || c == '\r') {
            if (tty_line_len < TTY_LINE_MAX - 1) tty_line_buf[tty_line_len++] = '\n';
            terminal_putchar('\n');
            tty_line_ready = 1;
            return 1;
        }
        if (c == '\b') {
            if (tty_line_len > 0) {
                tty_line_len--;
                terminal_putchar('\b');
                terminal_putchar(' ');
                terminal_putchar('\b');
            }
            return 0;
        }
        if (c == (char)21) { /* ^U: kill line */
            while (tty_line_len > 0) {
                tty_line_len--;
                terminal_putchar('\b');
                terminal_putchar(' ');
                terminal_putchar('\b');
            }
            return 0;
        }
        if (c == (char)4) { /* ^D: EOF */
            tty_line_ready = 1;
            return 1;
        }
        if (c == (char)3) { /* ^C: visual reset only */
            terminal_writestring("^C\n");
            tty_line_len = 0;
            return 0;
        }
        if (tty_line_len < TTY_LINE_MAX - 1) {
            tty_line_buf[tty_line_len++] = c;
            terminal_putchar(c);
        }
        return 0;
    }

    long tty_drain_ready(char* buf, size_t len) {
        if (!tty_line_ready) return -1;
        size_t avail = tty_line_len - tty_line_consumed;
        size_t n = (len < avail) ? len : avail;
        memcpy(buf, tty_line_buf + tty_line_consumed, n);
        tty_line_consumed += n;
        if (tty_line_consumed >= tty_line_len) {
            tty_line_len = 0;
            tty_line_consumed = 0;
            tty_line_ready = 0;
        }
        return (long)n;
    }

