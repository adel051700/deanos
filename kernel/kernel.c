#include <stddef.h>
#include <stdint.h>
#include <kernel/tty.h>

void kernel_main(void) {
    while (1) { 
        __asm__ __volatile__("hlt"); 
    }
}