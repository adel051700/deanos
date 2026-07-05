#ifndef _SYS_RTC_H
#define _SYS_RTC_H 1
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

struct rtc_time {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint16_t year;
};

struct uptime {
    uint32_t days;
    uint32_t hours;
    uint32_t minutes;
    uint32_t seconds;
};

struct pit_stats {
    uint64_t ticks;
    uint64_t uptime_ms;
};

int get_localtime(struct rtc_time* out);
int tz_get(void);
int tz_set(int hours);
int uptime(struct uptime* out);
int get_pit_stats(struct pit_stats* out);

#ifdef __cplusplus
}
#endif
#endif /* _SYS_RTC_H */
