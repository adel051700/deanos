#include "include/kernel/mouse.h"

#include "include/kernel/idt.h"
#include "include/kernel/io.h"
#include "include/kernel/irq.h"
#include "include/kernel/log.h"

#include <stddef.h>
#include <stdint.h>

#define PS2_DATA_PORT   0x60
#define PS2_STATUS_PORT 0x64
#define PS2_CMD_PORT    0x64

#define PS2_STATUS_OUT_FULL 0x01
#define PS2_STATUS_IN_FULL  0x02
#define PS2_STATUS_AUX_DATA 0x20

#define MOUSE_CURSOR_W 8
#define MOUSE_CURSOR_H 8
#define MOUSE_CURSOR_COLOR 0x00FFFFFFu

static volatile mouse_state_t g_mouse;
static volatile uint8_t g_packet[3];
static volatile uint8_t g_packet_index = 0;
static volatile int g_mouse_ready = 0;
static volatile uint8_t g_prev_buttons = 0;

static uint32_t g_cursor_saved[MOUSE_CURSOR_W * MOUSE_CURSOR_H];
static int g_cursor_visible = 0;
static int32_t g_cursor_x = 0;
static int32_t g_cursor_y = 0;

uint32_t framebuffer_getpixel(uint32_t x, uint32_t y);
void framebuffer_putpixel(uint32_t x, uint32_t y, uint32_t color);
uint32_t framebuffer_width(void);
uint32_t framebuffer_height(void);

static void mouse_restore_cursor(void) {
    if (!g_cursor_visible) return;
    uint32_t fb_w = framebuffer_width();
    uint32_t fb_h = framebuffer_height();

    for (int y = 0; y < MOUSE_CURSOR_H; ++y) {
        for (int x = 0; x < MOUSE_CURSOR_W; ++x) {
            uint32_t px = (uint32_t)(g_cursor_x + x);
            uint32_t py = (uint32_t)(g_cursor_y + y);
            if (px < fb_w && py < fb_h) {
                framebuffer_putpixel(px, py, g_cursor_saved[y * MOUSE_CURSOR_W + x]);
            }
        }
    }
    g_cursor_visible = 0;
}

static int cursor_mask(int x, int y) {
    if (x == 0 || y == 0) return 1;      /* top/left edge */
    if (x == y && x < 8) return 1;       /* diagonal tip */
    return 0;
}

static void mouse_draw_cursor(int32_t x, int32_t y) {
    uint32_t fb_w = framebuffer_width();
    uint32_t fb_h = framebuffer_height();

    g_cursor_x = x;
    g_cursor_y = y;

    for (int py = 0; py < MOUSE_CURSOR_H; ++py) {
        for (int px = 0; px < MOUSE_CURSOR_W; ++px) {
            uint32_t sx = (uint32_t)(x + px);
            uint32_t sy = (uint32_t)(y + py);
            uint32_t idx = (uint32_t)(py * MOUSE_CURSOR_W + px);

            if (sx < fb_w && sy < fb_h) {
                g_cursor_saved[idx] = framebuffer_getpixel(sx, sy);
                if (cursor_mask(px, py)) {
                    framebuffer_putpixel(sx, sy, MOUSE_CURSOR_COLOR);
                }
            } else {
                g_cursor_saved[idx] = 0;
            }
        }
    }
    g_cursor_visible = 1;
}

static int ps2_wait_write_ready(void) {
    for (uint32_t i = 0; i < 100000u; ++i) {
        if ((inb(PS2_STATUS_PORT) & PS2_STATUS_IN_FULL) == 0)
            return 1;
    }
    return 0;
}

static int ps2_wait_read_ready(void) {
    for (uint32_t i = 0; i < 100000u; ++i) {
        if (inb(PS2_STATUS_PORT) & PS2_STATUS_OUT_FULL)
            return 1;
    }
    return 0;
}

static int ps2_write_controller(uint8_t cmd) {
    if (!ps2_wait_write_ready()) return 0;
    outb(PS2_CMD_PORT, cmd);
    return 1;
}

static int ps2_write_mouse(uint8_t value) {
    if (!ps2_write_controller(0xD4)) return 0;
    if (!ps2_wait_write_ready()) return 0;
    outb(PS2_DATA_PORT, value);
    return 1;
}

static int ps2_read_data(uint8_t* out) {
    if (!out) return 0;
    if (!ps2_wait_read_ready()) return 0;
    *out = inb(PS2_DATA_PORT);
    return 1;
}

static int mouse_send_cmd_expect_ack(uint8_t cmd) {
    uint8_t resp = 0;
    if (!ps2_write_mouse(cmd)) return 0;
    if (!ps2_read_data(&resp)) return 0;
    return (resp == 0xFA);
}

static void mouse_irq_handler(struct registers* regs) {
    (void)regs;

    uint8_t status = inb(PS2_STATUS_PORT);
    if ((status & PS2_STATUS_OUT_FULL) == 0) return;
    if ((status & PS2_STATUS_AUX_DATA) == 0) return;

    uint8_t data = inb(PS2_DATA_PORT);

    if (g_packet_index == 0 && (data & 0x08) == 0) {
        return; /* maintain packet byte alignment */
    }

    g_packet[g_packet_index++] = data;
    if (g_packet_index < 3) return;
    g_packet_index = 0;

    int8_t dx = (int8_t)g_packet[1];
    int8_t dy = (int8_t)g_packet[2];

    uint8_t buttons = (uint8_t)(g_packet[0] & 0x07u);
    uint8_t newly_pressed = (uint8_t)(buttons & (uint8_t)~g_prev_buttons);
    g_prev_buttons = buttons;

    if (newly_pressed & 0x01u) g_mouse.left_clicks++;
    if (newly_pressed & 0x02u) g_mouse.right_clicks++;
    if (newly_pressed & 0x04u) g_mouse.middle_clicks++;

    g_mouse.buttons = buttons;
    g_mouse.x_overflow = (uint8_t)((g_packet[0] >> 6) & 0x01u);
    g_mouse.y_overflow = (uint8_t)((g_packet[0] >> 7) & 0x01u);

    g_mouse.x += dx;
    g_mouse.y -= dy;
    g_mouse.dx_total += dx;
    g_mouse.dy_total += dy;
    g_mouse.packet_count++;

    uint32_t fb_w = framebuffer_width();
    uint32_t fb_h = framebuffer_height();
    int32_t max_x = (fb_w > MOUSE_CURSOR_W) ? (int32_t)(fb_w - MOUSE_CURSOR_W) : 0;
    int32_t max_y = (fb_h > MOUSE_CURSOR_H) ? (int32_t)(fb_h - MOUSE_CURSOR_H) : 0;

    if (g_mouse.x < 0) g_mouse.x = 0;
    if (g_mouse.y < 0) g_mouse.y = 0;
    if (g_mouse.x > max_x) g_mouse.x = max_x;
    if (g_mouse.y > max_y) g_mouse.y = max_y;

    mouse_restore_cursor();
    mouse_draw_cursor(g_mouse.x, g_mouse.y);
}

void mouse_initialize(void) {
    g_mouse = (mouse_state_t){0};
    g_packet_index = 0;
    g_mouse_ready = 0;
    g_prev_buttons = 0;
    g_cursor_visible = 0;

    if (!ps2_write_controller(0xA8)) {
        klog("mouse: enable aux port failed");
        return;
    }

    if (!ps2_write_controller(0x20)) {
        klog("mouse: read controller config failed");
        return;
    }

    uint8_t config = 0;
    if (!ps2_read_data(&config)) {
        klog("mouse: read config byte failed");
        return;
    }

    config |= 0x02u;   /* enable IRQ12 */
    config &= (uint8_t)~0x20u; /* enable mouse clock */

    if (!ps2_write_controller(0x60) || !ps2_wait_write_ready()) {
        klog("mouse: write controller config failed");
        return;
    }
    outb(PS2_DATA_PORT, config);

    if (!mouse_send_cmd_expect_ack(0xF6)) {
        klog("mouse: set defaults failed");
        return;
    }

    if (!mouse_send_cmd_expect_ack(0xF4)) {
        klog("mouse: enable data reporting failed");
        return;
    }

    irq_install_handler(12, mouse_irq_handler);
    g_mouse_ready = 1;
    mouse_draw_cursor(0, 0);
    klog("mouse: PS/2 ready on IRQ12");
}

int mouse_is_ready(void) {
    return g_mouse_ready;
}

void mouse_get_state(mouse_state_t* out) {
    if (!out) return;

    interrupts_disable();
    *out = g_mouse;
    interrupts_enable();
}

void mouse_clear_motion(void) {
    interrupts_disable();
    g_mouse.dx_total = 0;
    g_mouse.dy_total = 0;
    interrupts_enable();
}

void mouse_reset_counters(void) {
    interrupts_disable();
    g_mouse.dx_total = 0;
    g_mouse.dy_total = 0;
    g_mouse.left_clicks = 0;
    g_mouse.right_clicks = 0;
    g_mouse.middle_clicks = 0;
    g_mouse.packet_count = 0;
    interrupts_enable();
}

