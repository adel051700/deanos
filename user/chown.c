#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

static int is_decimal_token(const char* s) {
    if (!s || *s == '\0') return 0;
    while (*s) {
        if (*s < '0' || *s > '9') return 0;
        s++;
    }
    return 1;
}

static uint32_t parse_uint(const char* s) {
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10u + (uint32_t)(*s - '0'); s++; }
    return v;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        printf("usage: chown <uid> <gid> <path>\n");
        return 1;
    }

    if (!is_decimal_token(argv[1]) || !is_decimal_token(argv[2])) {
        printf("chown: uid/gid must be decimal integers\n");
        return 1;
    }

    uint32_t uid = parse_uint(argv[1]);
    uint32_t gid = parse_uint(argv[2]);

    if (chown(argv[3], uid, gid) < 0) {
        printf("chown: failed\n");
        return 1;
    }
    return 0;
}
