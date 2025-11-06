#include "include/kernel/rtc.h"
#include "include/kernel/io.h"
#include "include/kernel/tty.h"
#include "include/kernel/interrupt.h"
#include <stdint.h>

#define CMOS_ADDRESS 0x70
#define CMOS_DATA    0x71

// CMOS register addresses
#define RTC_SECONDS  0x00
#define RTC_MINUTES  0x02
#define RTC_HOURS    0x04
#define RTC_DAY      0x07
#define RTC_MONTH    0x08
#define RTC_YEAR     0x09
#define RTC_STATUS_A 0x0A
#define RTC_STATUS_B 0x0B

// PIT (Programmable Interval Timer) ports
#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43

static int century_register = 0x00;

// Store boot time
static rtc_time_t boot_time;
static int boot_time_initialized = 0;

/**
 * Read a value from a CMOS register
 */
static uint8_t rtc_read_register(uint8_t reg) {
    outb(CMOS_ADDRESS, reg);
    return inb(CMOS_DATA);
}

/**
 * Check if RTC is updating
 */
static int rtc_update_in_progress(void) {
    outb(CMOS_ADDRESS, RTC_STATUS_A);
    return (inb(CMOS_DATA) & 0x80);
}

/**
 * Convert BCD to binary if necessary
 */
static uint8_t bcd_to_binary(uint8_t bcd) {
    return ((bcd / 16) * 10) + (bcd & 0x0F);
}

// Add timezone offset (CET = UTC+1, CEST = UTC+2)
// For simplicity, using CET (+1 hour). You can make this configurable later.
#define TIMEZONE_OFFSET_HOURS 1

/**
 * Read current time from RTC
 */
void rtc_read_time(rtc_time_t* time) {
    uint8_t century = 0;
    uint8_t last_second, last_minute, last_hour, last_day, last_month, last_year;
    uint8_t registerB;
    
    // Wait until RTC is not updating
    while (rtc_update_in_progress());
    
    // Read all time values
    time->second = rtc_read_register(RTC_SECONDS);
    time->minute = rtc_read_register(RTC_MINUTES);
    time->hour = rtc_read_register(RTC_HOURS);
    time->day = rtc_read_register(RTC_DAY);
    time->month = rtc_read_register(RTC_MONTH);
    time->year = rtc_read_register(RTC_YEAR);
    
    if (century_register != 0) {
        century = rtc_read_register(century_register);
    }
    
    // Read again to check for consistency
    do {
        last_second = time->second;
        last_minute = time->minute;
        last_hour = time->hour;
        last_day = time->day;
        last_month = time->month;
        last_year = time->year;
        
        while (rtc_update_in_progress());
        
        time->second = rtc_read_register(RTC_SECONDS);
        time->minute = rtc_read_register(RTC_MINUTES);
        time->hour = rtc_read_register(RTC_HOURS);
        time->day = rtc_read_register(RTC_DAY);
        time->month = rtc_read_register(RTC_MONTH);
        time->year = rtc_read_register(RTC_YEAR);
        
        if (century_register != 0) {
            century = rtc_read_register(century_register);
        }
    } while ((last_second != time->second) || (last_minute != time->minute) ||
             (last_hour != time->hour) || (last_day != time->day) ||
             (last_month != time->month) || (last_year != time->year));
    
    // Convert BCD to binary if necessary
    registerB = rtc_read_register(RTC_STATUS_B);
    
    if (!(registerB & 0x04)) {
        time->second = bcd_to_binary(time->second);
        time->minute = bcd_to_binary(time->minute);
        time->hour = ((time->hour & 0x0F) + (((time->hour & 0x70) / 16) * 10)) | (time->hour & 0x80);
        time->day = bcd_to_binary(time->day);
        time->month = bcd_to_binary(time->month);
        time->year = bcd_to_binary(time->year);
        if (century_register != 0) {
            century = bcd_to_binary(century);
        }
    }
    
    // Convert 12-hour to 24-hour if necessary
    if (!(registerB & 0x02) && (time->hour & 0x80)) {
        time->hour = ((time->hour & 0x7F) + 12) % 24;
    }
    
    // Calculate full year
    if (century_register != 0) {
        time->year += century * 100;
    } else {
        time->year += 2000; // Assume 21st century
    }
    
    // Apply timezone offset
    time->hour += TIMEZONE_OFFSET_HOURS;
    if (time->hour >= 24) {
        time->hour -= 24;
        time->day += 1;
        // Note: This is a simple adjustment and doesn't handle month/year rollover
        // For a production system, you'd need proper date arithmetic
    }
}

/**
 * Timer interrupt handler
 */
static void timer_handler(struct registers* regs) {
    (void)regs; // Unused
    timer_tick();
}

/**
 * Initialize the PIT timer
 */
void timer_initialize(uint32_t frequency) {
    // Calculate divisor for desired frequency
    uint32_t divisor = 1193180 / frequency;
    
    // Send command byte: channel 0, rate generator mode
    outb(PIT_COMMAND, 0x36);
    
    // Send frequency divisor
    outb(PIT_CHANNEL0, divisor & 0xFF);
    outb(PIT_CHANNEL0, (divisor >> 8) & 0xFF);
    
    // Register the timer interrupt handler
    register_interrupt_handler(32, timer_handler);
}

/**
 * Timer tick handler - called on each timer interrupt
 */
void timer_tick(void) {
    // Update cursor blinking
    terminal_update_cursor();
}

/**
 * Get system uptime by comparing current time with boot time
 */
void get_uptime(uptime_t* uptime) {
    if (!boot_time_initialized) {
        uptime->days = 0;
        uptime->hours = 0;
        uptime->minutes = 0;
        uptime->seconds = 0;
        return;
    }
    
    rtc_time_t current_time;
    rtc_read_time(&current_time);
    
    // Calculate difference in seconds
    int32_t total_seconds = 0;
    
    total_seconds += (current_time.second - boot_time.second);
    total_seconds += (current_time.minute - boot_time.minute) * 60;
    total_seconds += (current_time.hour - boot_time.hour) * 3600;
    
    // Handle day overflow (simplified - doesn't account for month/year changes)
    int32_t day_diff = current_time.day - boot_time.day;
    if (day_diff < 0) {
        day_diff += 30; // Rough approximation
    }
    total_seconds += day_diff * 86400;
    
    // Handle negative seconds (clock adjustment)
    if (total_seconds < 0) {
        total_seconds = 0;
    }
    
    uptime->seconds = total_seconds % 60;
    total_seconds /= 60;
    
    uptime->minutes = total_seconds % 60;
    total_seconds /= 60;
    
    uptime->hours = total_seconds % 24;
    total_seconds /= 24;
    
    uptime->days = total_seconds;
}

/**
 * Initialize RTC and save boot time
 */
void rtc_initialize(void) {
    rtc_read_time(&boot_time);
    boot_time_initialized = 1;
}