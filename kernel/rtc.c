#include "include/kernel/rtc.h"
#include "include/kernel/io.h"
#include "include/kernel/pit.h"
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

static uint32_t synced_epoch_s = 0;
static uint64_t synced_mono_ms = 0;
static int synced = 0;

static int century_register = 0x00;
static int32_t tz_offset_hours = 0;  /* UTC offset, whole hours, range -12..+14 */

static inline uint32_t irq_save(void) {
    uint32_t f;
    __asm__ __volatile__("pushf; pop %0; cli" : "=r"(f) : : "memory");
    return f;
}

static inline void irq_restore(uint32_t f) {
    __asm__ __volatile__("push %0; popf" : : "r"(f) : "memory", "cc");
}

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
        time->year += 2000;
    }
}

/**
 * Get system uptime from monotonic PIT clock
 */
void get_uptime(uptime_t* uptime) {
    uint64_t total_seconds = pit_get_uptime_ms() / 1000u;

    uptime->seconds = (uint32_t)(total_seconds % 60u); total_seconds /= 60u;
    uptime->minutes = (uint32_t)(total_seconds % 60u); total_seconds /= 60u;
    uptime->hours   = (uint32_t)(total_seconds % 24u); total_seconds /= 24u;
    uptime->days    = (uint32_t)total_seconds;
}

/**
 * Initialize RTC
 */
void rtc_initialize(void) {
    rtc_resync_tick();
}

static int is_leap_year(uint32_t year) {
    return ((year % 4u) == 0u && (year % 100u) != 0u) || ((year % 400u) == 0u);
}

static uint32_t rtc_time_to_epoch_seconds(const rtc_time_t* t) {
    static const uint16_t days_before_month[12] = {
        0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
    };

    if (t->year < 1970 || t->month < 1 || t->month > 12 || t->day < 1 || t->day > 31 ||
        t->hour > 23 || t->minute > 59 || t->second > 59) {
        return 0;
    }

    uint32_t days = 0;

    for (uint32_t y = 1970; y < t->year; ++y) {
        days += is_leap_year(y) ? 366u : 365u;
    }

    days += days_before_month[t->month - 1];
    if (t->month > 2 && is_leap_year(t->year)) {
        days += 1;
    }

    days += (uint32_t)(t->day - 1);

    return days * 86400u +
           (uint32_t)t->hour * 3600u +
           (uint32_t)t->minute * 60u +
           (uint32_t)t->second;
}

static void rtc_epoch_to_calendar(uint32_t epoch, rtc_time_t* out) {
    static const uint8_t month_lengths[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };

    uint32_t days = epoch / 86400u;
    uint32_t rem  = epoch % 86400u;

    out->hour   = (uint8_t)(rem / 3600u);
    out->minute = (uint8_t)((rem % 3600u) / 60u);
    out->second = (uint8_t)(rem % 60u);

    uint32_t year = 1970u;
    for (;;) {
        uint32_t year_days = is_leap_year(year) ? 366u : 365u;
        if (days < year_days) break;
        days -= year_days;
        year++;
    }
    out->year = (uint16_t)year;

    uint32_t month = 0;
    for (;;) {
        uint32_t len = month_lengths[month];
        if (month == 1 && is_leap_year(year)) len = 29;  /* February */
        if (days < len) break;
        days -= len;
        month++;
    }
    out->month = (uint8_t)(month + 1);
    out->day   = (uint8_t)(days + 1);
}

static uint32_t rtc_read_epoch_now(void) {
    rtc_time_t t;
    rtc_read_time(&t);
    return rtc_time_to_epoch_seconds(&t);
}

void rtc_resync_tick(void) {
    uint64_t now_ms = pit_get_uptime_ms();
    if (synced && (now_ms - synced_mono_ms) < RTC_RESYNC_INTERVAL_MS) return;

    uint32_t epoch = rtc_read_epoch_now();
    if (epoch == 0) return;  /* invalid/garbage RTC read — keep retrying next call instead of caching a bad value */

    uint32_t f = irq_save();
    synced_epoch_s = epoch;
    synced_mono_ms = now_ms;
    synced = 1;
    irq_restore(f);
}

uint32_t rtc_get_wallclock_seconds(void) {
    if (!synced) return rtc_read_epoch_now();
    uint64_t elapsed_ms = pit_get_uptime_ms() - synced_mono_ms;
    return synced_epoch_s + (uint32_t)(elapsed_ms / 1000u);
}

int32_t rtc_tz_get_hours(void) {
    return tz_offset_hours;
}

int rtc_tz_set_hours(int32_t hours) {
    if (hours < -12 || hours > 14) return -1;
    tz_offset_hours = hours;
    return 0;
}

void rtc_get_local_time(rtc_time_t* out) {
    uint32_t utc = rtc_get_wallclock_seconds();
    int64_t local = (int64_t)utc + (int64_t)tz_offset_hours * 3600;
    if (local < 0) local = 0;  /* clamp rather than wrap to a bogus far-future date */
    rtc_epoch_to_calendar((uint32_t)local, out);
}
