#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/rtc.h>

static void print_tz_offset(int hours) {
    printf("UTC");
    if (hours >= 0) printf("+");
    printf("%d", hours);
}

static void print_datetime(void) {
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
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Timezone: ");
        print_tz_offset(tz_get());
        printf("\n");
        print_datetime();
        return 0;
    }

    if (strcmp(argv[1], "set") == 0) {
        if (argc < 3) {
            printf("usage: tz set <+H|-H>\n");
            return 1;
        }

        int hours = atoi(argv[2]);
        int rc = tz_set(hours);
        if (rc < 0) {
            printf("tz: offset out of range (-12..+14)\n");
            return 1;
        }

        printf("Timezone set to ");
        print_tz_offset(hours);
        printf("\n");

        if (rc == 1) {
            printf("(warning: could not save to /timezone, active for this session only)\n");
        }
        return 0;
    }

    printf("usage: tz | tz set <+H|-H>\n");
    return 1;
}
