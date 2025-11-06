#include "include/kernel/io.h"

/**
 * Write a byte to a port
 */
void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

/**
 * Read a byte from a port
 */
uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/**
 * Wait for a very small amount of time (1 to 4 microseconds)
 * Useful after I/O operations
 */
void io_wait() {
    outb(0x80, 0); // Write to unused port
}