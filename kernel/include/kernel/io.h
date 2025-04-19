#ifndef _KERNEL_IO_H
#define _KERNEL_IO_H

#include <stdint.h>

// Output a byte to a port
void outb(uint16_t port, uint8_t value);

// Read a byte from a port
uint8_t inb(uint16_t port);

// Short delay for I/O operations
void io_wait(void);

#endif