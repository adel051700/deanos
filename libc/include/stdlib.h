#ifndef _STDLIB_H
#define _STDLIB_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void* malloc(size_t size);
void free(void* ptr);
void* calloc(size_t nmemb, size_t size);
void* realloc(void* ptr, size_t size);

int atoi(const char* s);

#define RAND_MAX 0x7fffffff

int rand(void);
void srand(unsigned seed);

void abort(void);
void exit(int status);

#ifdef __cplusplus
}
#endif

#endif /* _STDLIB_H */

