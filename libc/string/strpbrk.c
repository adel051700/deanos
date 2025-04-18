#include <../include/string.h>

/**
 * Locates the first occurrence of any character from accept in str
 * 
 * This function scans the string str looking for any character that 
 * matches any character in the accept string.
 * 
 * @param str Pointer to the null-terminated string to be scanned
 * @param accept Pointer to the null-terminated string containing characters to match
 * @return Pointer to the first occurrence in str of any character in accept,
 *         or NULL if no such character is found
 */
char* strpbrk(const char* str, const char* accept) {
    // Scan through the string
    while (*str != '\0') {
        // Check if current character exists in the accept string
        if (strchr(accept, *str) != NULL) {
            return (char*)str;  // Return pointer to the matching character
        }
        str++;  // Move to next character
    }
    
    // No matching character found
    return NULL;
}

/**
 * Locates the first occurrence of a substring in a string
 * 
 * This function finds the first occurrence of substring needle in the string 
 * haystack. The terminating null characters are not compared.
 * 
 * @param haystack Pointer to the null-terminated string to be scanned
 * @param needle Pointer to the null-terminated substring to search for
 * @return Pointer to the first occurrence of needle in haystack,
 *         or NULL if needle is not found
 */
char* strstr(const char* haystack, const char* needle) {
    // If needle is empty, return haystack
    if (*needle == '\0') {
        return (char*)haystack;
    }
    
    // Get the length of the needle for comparing substrings
    size_t needle_len = strlen(needle);
    
    // Search for the needle in the haystack
    while (*haystack != '\0') {
        // If first character matches and the whole needle matches
        if (*haystack == *needle && 
            strncmp(haystack, needle, needle_len) == 0) {
            return (char*)haystack;  // Return pointer to the start of match
        }
        haystack++;  // Move to next character in haystack
    }
    
    // Needle not found
    return NULL;
}