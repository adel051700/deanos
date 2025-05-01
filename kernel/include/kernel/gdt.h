#ifndef _KERNEL_GDT_H
#define _KERNEL_GDT_H

#include <stdint.h>

// Initialize the GDT
void gdt_initialize(void);

#endif /* _KERNEL_GDT_H */