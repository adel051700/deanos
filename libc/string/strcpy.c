#include <../include/string.h>

/**
 * Copies a string from source to destination
 * 
 * This function copies the null-terminated string pointed to by 'source'
 * to the buffer pointed to by 'destination'. The strings may not overlap.
 * The terminating null character from source is also copied to destination.
 *
 * @param destination Pointer to the destination array where the content will be copied
 * @param source Pointer to the null-terminated string to be copied
 * @return Pointer to the destination string
 */
char* strcpy(char* destination, const char* source) {
    char* ptr = destination;
    
    while (*source) {
        *destination++ = *source++;
    }
    
    *destination = '\0';  // Add null terminator
    
    return ptr;
}

/**
 * Copies up to num characters from source to destination
 * 
 * This function copies at most 'num' characters from the string pointed to by 'source'
 * to the buffer pointed to by 'destination'. If the end of the source string (the null character)
 * is found before 'num' characters have been copied, destination is padded with zeros until
 * 'num' characters have been written.
 *
 * @param destination Pointer to the destination array where the content will be copied
 * @param source Pointer to the string to be copied
 * @param num Maximum number of characters to be copied
 * @return Pointer to the destination string
 */
char* strncpy(char* destination, const char* source, size_t num) {
    char* ptr = destination;
    size_t i;
    
    // Copy characters from source until either num characters copied or end of source reached
    for (i = 0; i < num && source[i] != '\0'; i++) {
        destination[i] = source[i];
    }
    
    // If fewer than num characters copied, pad with null characters
    for (; i < num; i++) {
        destination[i] = '\0';
    }
    
    return ptr;
}