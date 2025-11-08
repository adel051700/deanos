#ifndef _KERNEL_PIT_H
#define _KERNEL_PIT_H

#include <stdint.h>

// PIT operates at 1.193182 MHz
#define PIT_FREQUENCY 1193182

// Initialize PIT with desired frequency in Hz
void pit_initialize(uint32_t frequency);

// Get the number of timer ticks since boot
uint64_t pit_get_ticks(void);

// Get uptime in milliseconds
uint64_t pit_get_uptime_ms(void);

// Sleep for specified milliseconds (blocking)
void pit_sleep(uint32_t milliseconds);

#endif