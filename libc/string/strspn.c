#include <../include/string.h>

/**
 * Gets the length of the initial portion of str containing only characters in accept
 * 
 * This function calculates the length of the maximum initial segment of the string
 * pointed to by str that consists entirely of characters from the string pointed to by accept.
 * 
 * @param str Pointer to the null-terminated string to be scanned
 * @param accept Pointer to the null-terminated string containing the characters to match
 * @return The number of characters in the initial segment of str that consist only of characters from accept
 */
size_t strspn(const char* str, const char* accept) {
    size_t count = 0;
    
    // Count characters that are in the accept set
    while (str[count] != '\0') {
        // If current character is not in accept, stop counting
        if (strchr(accept, str[count]) == NULL) {
            break;
        }
        count++;  // Increment counter for each matching character
    }
    
    return count;
}

/**
 * Gets the length of the initial portion of str containing no characters from reject
 * 
 * This function calculates the length of the maximum initial segment of the string
 * pointed to by str that consists entirely of characters not from the string pointed to by reject.
 * 
 * @param str Pointer to the null-terminated string to be scanned
 * @param reject Pointer to the null-terminated string containing the characters to avoid
 * @return The number of characters in the initial segment of str that do not appear in reject
 */
size_t strcspn(const char* str, const char* reject) {
    size_t count = 0;
    
    // Count characters until we find one that's in the reject set
    while (str[count] != '\0') {
        // If current character is in reject, stop counting
        if (strchr(reject, str[count]) != NULL) {
            break;
        }
        count++;  // Increment counter for each non-matching character
    }
    
    return count;
}