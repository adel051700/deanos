#ifndef _STRING_H
#define _STRING_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Memory functions */
void* memcpy(void* destination, const void* source, size_t num);
void* memmove(void* destination, const void* source, size_t num);
void* memset(void* ptr, int value, size_t num);
int memcmp(const void* ptr1, const void* ptr2, size_t num);
void* memchr(const void* ptr, int value, size_t num);

/* String functions */
size_t strlen(const char* str);
char* strcpy(char* destination, const char* source);
char* strncpy(char* destination, const char* source, size_t num);
char* strcat(char* destination, const char* source);
char* strncat(char* destination, const char* source, size_t num);
int strcmp(const char* str1, const char* str2);
int strncmp(const char* str1, const char* str2, size_t num);
char* strchr(const char* str, int character);
char* strrchr(const char* str, int character);
size_t strspn(const char* str, const char* accept);
size_t strcspn(const char* str, const char* reject);
char* strpbrk(const char* str, const char* accept);
char* strstr(const char* haystack, const char* needle);

#ifdef __cplusplus
}
#endif

#endif /* _STRING_H */