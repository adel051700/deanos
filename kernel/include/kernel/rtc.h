#ifndef _KERNEL_RTC_H
#define _KERNEL_RTC_H

#include <stdint.h>

#define RTC_RESYNC_INTERVAL_MS (30u * 60u * 1000u)

typedef struct {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint16_t year;
} rtc_time_t;

typedef struct {
    uint32_t days;
    uint32_t hours;
    uint32_t minutes;
    uint32_t seconds;
} uptime_t;

void rtc_initialize(void);
void rtc_read_time(rtc_time_t* time);
void get_uptime(uptime_t* uptime);
uint32_t rtc_get_wallclock_seconds(void);
void rtc_resync_tick(void);
int32_t rtc_tz_get_hours(void);
int     rtc_tz_set_hours(int32_t hours);
void    rtc_get_local_time(rtc_time_t* out);

#endif