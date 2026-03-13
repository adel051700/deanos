#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

static size_t utoa_base(unsigned long value, unsigned base, int uppercase, char* out) {
    const char* digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    size_t i = 0;

    if (base < 2 || base > 16) {
        out[0] = '\0';
        return 0;
    }

    do {
        out[i++] = digits[value % base];
        value /= base;
    } while (value != 0);

    for (size_t j = 0; j < i / 2; j++) {
        char tmp = out[j];
        out[j] = out[i - 1 - j];
        out[i - 1 - j] = tmp;
    }

    out[i] = '\0';
    return i;
}

static int write_text(const char* s, size_t len) {
    if (len == 0) {
        return 0;
    }
    return (write(1, s, len) < 0) ? -1 : (int)len;
}

int vprintf(const char* format, va_list args) {
    int written = 0;

    if (!format) {
        return -1;
    }

    for (size_t i = 0; format[i] != '\0'; i++) {
        if (format[i] != '%') {
            if (write_text(&format[i], 1) < 0) {
                return -1;
            }
            written++;
            continue;
        }

        i++;
        if (format[i] == '\0') {
            break;
        }

        if (format[i] == '%') {
            if (write_text("%", 1) < 0) {
                return -1;
            }
            written++;
            continue;
        }

        char numbuf[32];
        const char* text = numbuf;
        size_t len = 0;

        switch (format[i]) {
            case 'c': {
                numbuf[0] = (char)va_arg(args, int);
                len = 1;
                break;
            }
            case 's': {
                text = va_arg(args, const char*);
                if (!text) {
                    text = "(null)";
                }
                while (text[len] != '\0') {
                    len++;
                }
                break;
            }
            case 'd':
            case 'i': {
                int v = va_arg(args, int);
                if (v < 0) {
                    if (write_text("-", 1) < 0) {
                        return -1;
                    }
                    written++;
                    len = utoa_base((unsigned long)(-(long)v), 10, 0, numbuf);
                } else {
                    len = utoa_base((unsigned long)v, 10, 0, numbuf);
                }
                break;
            }
            case 'u': {
                unsigned v = va_arg(args, unsigned);
                len = utoa_base((unsigned long)v, 10, 0, numbuf);
                break;
            }
            case 'x': {
                unsigned v = va_arg(args, unsigned);
                len = utoa_base((unsigned long)v, 16, 0, numbuf);
                break;
            }
            case 'X': {
                unsigned v = va_arg(args, unsigned);
                len = utoa_base((unsigned long)v, 16, 1, numbuf);
                break;
            }
            case 'p': {
                uintptr_t v = (uintptr_t)va_arg(args, void*);
                if (write_text("0x", 2) < 0) {
                    return -1;
                }
                written += 2;
                len = utoa_base((unsigned long)v, 16, 0, numbuf);
                break;
            }
            default: {
                if (write_text("%", 1) < 0 || write_text(&format[i], 1) < 0) {
                    return -1;
                }
                written += 2;
                continue;
            }
        }

        if (write_text(text, len) < 0) {
            return -1;
        }
        written += (int)len;
    }

    return written;
}

int printf(const char* format, ...) {
    va_list args;
    int ret;

    va_start(args, format);
    ret = vprintf(format, args);
    va_end(args);

    return ret;
}
