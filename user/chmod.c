#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

static int is_octal_token(const char* s) {
    if (!s || *s == '\0') return 0;
    while (*s) {
        if (*s < '0' || *s > '7') return 0;
        s++;
    }
    return 1;
}

static int parse_mode_octal(const char* token, uint16_t* out_mode) {
    if (!is_octal_token(token)) return -1;

    uint32_t mode = 0;
    const char* p = token;
    while (*p) {
        mode = (mode << 3u) + (uint32_t)(*p - '0');
        p++;
    }
    if (mode > 0777u) return -1;
    *out_mode = (uint16_t)mode;
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("usage: chmod <octal-mode> <path>\n");
        return 1;
    }

    uint16_t mode = 0;
    if (parse_mode_octal(argv[1], &mode) < 0) {
        printf("chmod: invalid mode (use octal like 644 or 755)\n");
        return 1;
    }

    if (chmod(argv[2], mode) < 0) {
        printf("chmod: failed\n");
        return 1;
    }
    return 0;
}
