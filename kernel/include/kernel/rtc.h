#ifndef _KERNEL_RTC_H
#define _KERNEL_RTC_H

#include <stdint.h>

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
void timer_initialize(uint32_t frequency);
void timer_tick(void);
void get_uptime(uptime_t* uptime);

#endif