#include "../include/string.h"

/**
 * Locates the first occurrence of a character in a string
 * 
 * This function searches for the first occurrence of the character specified
 * by 'character' in the null-terminated string pointed to by 'str'.
 * The terminating null character is considered part of the string, so if
 * 'character' is '\0', the function returns a pointer to the null-terminator.
 *
 * @param str Character string to be scanned
 * @param character Character to be located (passed as an int, converted to char)
 * @return Pointer to the first occurrence of the character in the string,
 *         or NULL if the character is not found
 */
char* strchr(const char* str, int character) {
    while (*str != '\0') {
        if (*str == (char)character) {
            return (char*)str;
        }
        str++;
    }
    
    // If the character is '\0', return pointer to the null terminator
    if ((char)character == '\0') {
        return (char*)str;
    }
    
    return NULL;
}

/**
 * Locates the last occurrence of a character in a string
 * 
 * This function searches for the last occurrence of the character specified
 * by 'character' in the null-terminated string pointed to by 'str'.
 * The terminating null character is considered part of the string, so if
 * 'character' is '\0', the function returns a pointer to the null-terminator.
 *
 * @param str Character string to be scanned
 * @param character Character to be located (passed as an int, converted to char)
 * @return Pointer to the last occurrence of the character in the string,
 *         or NULL if the character is not found
 */
char* strrchr(const char* str, int character) {
    const char* last_occurrence = NULL;
    
    // Find the last occurrence of character
    while (*str != '\0') {
        if (*str == (char)character) {
            last_occurrence = str;
        }
        str++;
    }
    
    // Check if the character to be found is '\0'
    if ((char)character == '\0') {
        return (char*)str;
    }
    
    return (char*)last_occurrence;
}