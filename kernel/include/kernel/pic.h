#ifndef _KERNEL_PIC_H
#define _KERNEL_PIC_H

#include <stdint.h>

// Initialize and remap 8259 PICs to vectors 0x20-0x2F
void pic_init(void);

// Mask/unmask individual IRQ lines [0..15]
void pic_mask_irq(uint8_t irq);
void pic_unmask_irq(uint8_t irq);

// Send End-Of-Interrupt for a handled IRQ
void pic_eoi(uint8_t irq);

#endif