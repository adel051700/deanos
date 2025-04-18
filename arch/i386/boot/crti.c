#include <stdint.h>
#include <stddef.h>

/* These symbols come from the linker script */
extern void (*__ctors_start)(void);
extern void (*__ctors_end)(void);

/* Function that calls all global constructors */
void call_constructors(void) {
    /* Get the addresses as array of function pointers */
    void (**constructor)(void) = &__ctors_start;
    
    /* Call each constructor in order */
    while (constructor < &__ctors_end) {
        void (*func)(void) = *constructor;
        if (func != NULL) {
            func();
        }
        constructor++;
    }
}