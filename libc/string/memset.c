#include "../include/string.h"

/**
 * Sets the first 'num' bytes of the block of memory pointed by 'ptr'
 * to the specified 'value' (interpreted as an unsigned char).
 *
 * @param ptr   Pointer to the block of memory to fill
 * @param value Value to be set (converted to unsigned char)
 * @param num   Number of bytes to be set to the value
 * @return      The original pointer 'ptr'
 */
void* memset(void* ptr, int value, size_t num) {
    // Cast void pointer to unsigned char pointer for byte-by-byte access
    unsigned char* p = (unsigned char*)ptr;
    
    // Convert the value to an unsigned char (only lowest 8 bits used)
    unsigned char val = (unsigned char)value;
    
    // Set each byte in the memory block to the specified value
    for (size_t i = 0; i < num; i++) {
        p[i] = val;
    }
    
    // Return the original pointer as per memset specification
    return ptr;
}