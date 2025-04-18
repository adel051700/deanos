#include <../include/string.h>

/**
 * Compares the first 'num' bytes of the memory areas 'ptr1' and 'ptr2'.
 *
 * @param ptr1  Pointer to the first memory block
 * @param ptr2  Pointer to the second memory block
 * @param num   Number of bytes to compare
 * @return      < 0 if ptr1 < ptr2, 0 if ptr1 == ptr2, > 0 if ptr1 > ptr2
 */
int memcmp(const void* ptr1, const void* ptr2, size_t num) {
    // Cast void pointers to unsigned char pointers for byte-by-byte comparison
    const unsigned char* p1 = (const unsigned char*)ptr1;
    const unsigned char* p2 = (const unsigned char*)ptr2;
    
    // Compare each byte until we find a difference or reach the end
    for (size_t i = 0; i < num; i++) {
        if (p1[i] != p2[i]) {
            // Return the difference between the bytes that differ
            // Since we use unsigned char, the comparison works for binary data
            return p1[i] - p2[i];
        }
    }
    
    return 0;  // Memory blocks are equal up to num bytes
}

/**
 * Compares two null-terminated strings.
 *
 * @param str1  First string to be compared
 * @param str2  Second string to be compared
 * @return      < 0 if str1 < str2, 0 if str1 == str2, > 0 if str1 > str2
 */
int strcmp(const char* str1, const char* str2) {
    // Compare characters until we find a difference or reach the end of a string
    while (*str1 && (*str1 == *str2)) {
        str1++;  // Move to next character in first string
        str2++;  // Move to next character in second string
    }
    
    // Return the difference between the first non-matching characters
    // or the difference between '\0' and the remaining character
    // Cast to unsigned char to handle extended ASCII correctly
    return *(const unsigned char*)str1 - *(const unsigned char*)str2;
}

/**
 * Compares up to 'num' characters of two null-terminated strings.
 *
 * @param str1  First string to be compared
 * @param str2  Second string to be compared
 * @param num   Maximum number of characters to compare
 * @return      < 0 if str1 < str2, 0 if str1 == str2, > 0 if str1 > str2
 */
int strncmp(const char* str1, const char* str2, size_t num) {
    // Compare up to num characters or until strings differ
    for (size_t i = 0; i < num; i++) {
        if (str1[i] != str2[i]) {
            // Return the difference between the first non-matching characters
            // Cast to unsigned char to handle extended ASCII correctly
            return (int)(unsigned char)str1[i] - (int)(unsigned char)str2[i];
        }
        
        if (str1[i] == '\0') {
            // Both strings ended at the same position (before num characters)
            return 0;
        }
    }
    
    return 0;  // First num characters are equal (or num was 0)
}

/**
 * Copies memory area from source to destination.
 * The memory areas must not overlap.
 *
 * @param destination   Pointer to the destination array
 * @param source        Pointer to the source of data to be copied
 * @param num           Number of bytes to copy
 * @return              The destination pointer
 */
void* memcpy(void* destination, const void* source, size_t num) {
    // Cast void pointers to byte pointers for byte-by-byte copying
    unsigned char* dest = (unsigned char*)destination;
    const unsigned char* src = (const unsigned char*)source;
    
    // Copy each byte from source to destination
    // Note: This assumes non-overlapping memory regions
    for (size_t i = 0; i < num; i++) {
        dest[i] = src[i];
    }
    
    // Return the destination pointer per standard specification
    return destination;
}

/**
 * Copies memory area from source to destination, handling overlapping memory correctly.
 *
 * @param destination   Pointer to the destination array
 * @param source        Pointer to the source of data to be copied
 * @param num           Number of bytes to copy
 * @return              The destination pointer
 */
void* memmove(void* destination, const void* source, size_t num) {
    // Cast void pointers to byte pointers for byte-by-byte copying
    unsigned char* dest = (unsigned char*)destination;
    const unsigned char* src = (const unsigned char*)source;
    
    // Handle copying direction based on the relative positions of source and destination
    if (dest < src) {
        // If destination is before source in memory, copy forward (left to right)
        // This is safe because we won't overwrite source data before reading it
        for (size_t i = 0; i < num; i++) {
            dest[i] = src[i];
        }
    } else if (dest > src) {
        // If destination is after source in memory, copy backward (right to left)
        // This prevents overwriting source data that hasn't been copied yet
        for (size_t i = num; i > 0; i--) {
            dest[i-1] = src[i-1];
        }
    }
    // If dest == src, no copying is needed
    
    // Return the destination pointer per standard specification
    return destination;
}