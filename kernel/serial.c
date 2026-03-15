#include "include/kernel/serial.h"
#include "include/kernel/io.h"

#include <stdint.h>

#define COM1_BASE 0x3F8

#define COM_DATA(base)       ((base) + 0)
#define COM_INT_EN(base)     ((base) + 1)
#define COM_FIFO_CTRL(base)  ((base) + 2)
#define COM_LINE_CTRL(base)  ((base) + 3)
#define COM_MODEM_CTRL(base) ((base) + 4)
#define COM_LINE_STAT(base)  ((base) + 5)

#define LSR_TX_EMPTY 0x20

static uint16_t g_com_base = COM1_BASE;
static int g_serial_ready = 0;

static int serial_can_transmit(void) {
    return (inb(COM_LINE_STAT(g_com_base)) & LSR_TX_EMPTY) != 0;
}

void serial_initialize(void) {
    outb(COM_INT_EN(g_com_base), 0x00);
    outb(COM_LINE_CTRL(g_com_base), 0x80); /* enable DLAB */
    outb(COM_DATA(g_com_base), 0x03);      /* divisor low byte: 38400 baud */
    outb(COM_INT_EN(g_com_base), 0x00);    /* divisor high byte */
    outb(COM_LINE_CTRL(g_com_base), 0x03); /* 8 bits, no parity, one stop */
    outb(COM_FIFO_CTRL(g_com_base), 0xC7); /* enable FIFO, clear, 14-byte threshold */
    outb(COM_MODEM_CTRL(g_com_base), 0x0B);/* IRQs enabled, RTS/DSR set */

    g_serial_ready = 1;
}

int serial_is_available(void) {
    return g_serial_ready;
}

void serial_write_char(char c) {
    if (!g_serial_ready) return;
    while (!serial_can_transmit()) {
        __asm__ __volatile__("pause");
    }
    outb(COM_DATA(g_com_base), (uint8_t)c);
}

void serial_write_buf(const char* s, size_t len) {
    if (!g_serial_ready || !s || len == 0) return;
    for (size_t i = 0; i < len; ++i) {
        if (s[i] == '\n') {
            serial_write_char('\r');
        }
        serial_write_char(s[i]);
    }
}

