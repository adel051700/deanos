#include <stdio.h>
#include <sys/rtc.h>

int main(void) {
    struct uptime u;
    uptime(&u);

    printf("System uptime: ");

    if (u.days > 0) {
        printf("%d day", (int)u.days);
        if (u.days != 1) printf("s");
        printf(", ");
    }

    char buf[16];

    itoa((int)u.hours, buf, 10);
    if (u.hours < 10) printf("0");
    printf("%s:", buf);

    itoa((int)u.minutes, buf, 10);
    if (u.minutes < 10) printf("0");
    printf("%s:", buf);

    itoa((int)u.seconds, buf, 10);
    if (u.seconds < 10) printf("0");
    printf("%s\n", buf);

    return 0;
}
