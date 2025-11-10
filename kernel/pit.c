#include "include/kernel/pit.h"
#include "include/kernel/io.h"
#include "include/kernel/irq.h"
#include "include/kernel/tty.h"
#include <stdio.h>


// PIT I/O ports
#define PIT_CHANNEL0    0x40
#define PIT_CHANNEL1    0x41
#define PIT_CHANNEL2    0x42
#define PIT_COMMAND     0x43

// PIT Command register bits
#define PIT_CHANNEL0_SELECT    0x00
#define PIT_ACCESS_LOHI        0x30
#define PIT_MODE_SQUARE_WAVE   0x06
#define PIT_BINARY_MODE        0x00

// Global tick counter - volatile because it's modified by interrupt handler
static volatile uint64_t pit_ticks = 0;
static uint32_t pit_frequency_hz = 0;

/**
 * PIT interrupt handler - called on each timer tick
 */
static void pit_irq_handler(struct registers* regs) {
    (void)regs;
    pit_ticks++;

    // Call every tick; tty handles its own interval
    terminal_cursor_blink_tick();
}

/**
 * Initialize the PIT with the specified frequency
 */
void pit_initialize(uint32_t frequency) {
    pit_frequency_hz = frequency;
    irq_install_handler(0, pit_irq_handler);
    
    uint32_t divisor = PIT_FREQUENCY / frequency;
    if (divisor < 1) divisor = 1;
    if (divisor > 65535) divisor = 65535;
    
    uint8_t command = PIT_CHANNEL0_SELECT | PIT_ACCESS_LOHI | PIT_MODE_SQUARE_WAVE | PIT_BINARY_MODE;
    outb(PIT_COMMAND, command);
    
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    io_wait();
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));
    io_wait();
}

uint64_t pit_get_ticks(void) {
    return pit_ticks;
}

uint64_t pit_get_uptime_ms(void) {
    if (pit_frequency_hz == 0) return 0;
    return (pit_ticks * 1000) / pit_frequency_hz;
}

void pit_sleep(uint32_t milliseconds) {
    if (pit_frequency_hz == 0) return;
    
    uint64_t ticks_to_wait = (uint64_t)milliseconds * pit_frequency_hz / 1000;
    uint64_t start_ticks = pit_ticks;
    
    while ((pit_ticks - start_ticks) < ticks_to_wait) {
        __asm__ __volatile__("hlt");
    }
}