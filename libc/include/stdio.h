#ifndef _STDIO_H
#define _STDIO_H 1

#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

// Convert integer to string
char* itoa(int value, char* str, int base);

int vprintf(const char* format, va_list args);
int printf(const char* format, ...);

#ifdef __cplusplus
}
#endif

#endif /* _STDIO_H */