#include <stdio.h>
#include <sys/rtc.h>

int main(void) {
    struct rtc_time t;
    get_localtime(&t);

    char buf[16];

    printf("Date: ");
    itoa(t.day, buf, 10);
    if (t.day < 10) printf("0");
    printf("%s/", buf);
    itoa(t.month, buf, 10);
    if (t.month < 10) printf("0");
    printf("%s/", buf);
    itoa(t.year, buf, 10);
    printf("%s\n", buf);

    printf("Time: ");
    itoa(t.hour, buf, 10);
    if (t.hour < 10) printf("0");
    printf("%s:", buf);
    itoa(t.minute, buf, 10);
    if (t.minute < 10) printf("0");
    printf("%s:", buf);
    itoa(t.second, buf, 10);
    if (t.second < 10) printf("0");
    printf("%s\n", buf);

    return 0;
}
