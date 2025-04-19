#ifndef _STDIO_H
#define _STDIO_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Convert integer to string
char* itoa(int value, char* str, int base);

// Add other stdio functions as needed

#ifdef __cplusplus
}
#endif

#endif /* _STDIO_H */