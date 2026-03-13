#include "../include/stdio.h"

/**
 * Convert an integer to a string representation in the given base
 */
char* itoa(int value, char* str, int base) {
    // Check for supported base
    if (base < 2 || base > 36) {
        *str = '\0';
        return str;
    }

    // Handle negative numbers
    int sign = 0;
    if (value < 0 && base == 10) {
        sign = 1;
        value = -value;
    }

    // Convert integer to characters in reverse order
    int i = 0;
    do {
        int digit = value % base;
        str[i++] = digit < 10 ? '0' + digit : 'a' + digit - 10;
        value /= base;
    } while (value > 0);

    // Add negative sign if needed
    if (sign) {
        str[i++] = '-';
    }

    // Null-terminate the string
    str[i] = '\0';

    // Reverse the string
    int start = 0;
    int end = i - 1;
    while (start < end) {
        char temp = str[start];
        str[start] = str[end];
        str[end] = temp;
        start++;
        end--;
    }

    return str;
}