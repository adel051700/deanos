#include <stdlib.h>

int atoi(const char* s) {
    int result = 0;
    int sign = 1;

    if (!s) return 0;

    while (*s == ' ' || *s == '\t') s++;

    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }

    while (*s >= '0' && *s <= '9') {
        result = result * 10 + (*s - '0');
        s++;
    }

    return sign * result;
}
