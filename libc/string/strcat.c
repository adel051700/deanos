#include "../include/string.h"

/**
 * Concatenates the source string to the destination string
 * 
 * Appends a copy of the source string to the destination string.
 * The terminating null character in destination is overwritten by the
 * first character of source, and a null-character is included at
 * the end of the new string formed by the concatenation.
 *
 * @param destination Pointer to the null-terminated destination string
 * @param source Pointer to the null-terminated source string
 * @return Pointer to the destination string
 */
char* strcat(char* destination, const char* source) {
    char* ptr = destination;
    
    while (*destination) {
        destination++;
    }
    
    while (*source) {
        *destination++ = *source++;
    }
    
    *destination = '\0';
    
    return ptr;
}

/**
 * Concatenates a specified number of characters from source to destination
 * 
 * Appends the first num characters of source to destination, plus a
 * terminating null-character. If the length of source is less than num,
 * only the content up to the terminating null-character is copied.
 *
 * @param destination Pointer to the null-terminated destination string
 * @param source Pointer to the null-terminated source string
 * @param num Maximum number of characters to be appended
 * @return Pointer to the destination string
 */
char* strncat(char* destination, const char* source, size_t num) {
    // Save original destination pointer to return later
    char* ptr = destination;
    
    // Calculate the length of the destination string to find where to append
    size_t dest_len = strlen(destination);
    
    // Copy up to 'num' characters from source to destination
    size_t i;
    for (i = 0; i < num && source[i] != '\0'; i++) {
        // Position characters after the end of the original destination string
        destination[dest_len + i] = source[i];
    }
    
    // Ensure the resulting string is null-terminated
    // Note: i will be either num or the index of the null terminator in source
    destination[dest_len + i] = '\0';
    
    // Return the original destination pointer
    return ptr;
}