// filepath: /home/adel/deanos/libc/string/strlen.c
#include <../include/string.h>

/**
 * Calculates the length of a string
 * 
 * This function computes the length of the null-terminated string pointed 
 * to by 'str', excluding the terminating null character.
 * 
 * @param str Pointer to the null-terminated string to be examined
 * @return The number of characters in the string before the terminating null character
 */
size_t strlen(const char* str) {
    size_t len = 0;
    while (str[len]) {
        len++;
    }
    return len;
}