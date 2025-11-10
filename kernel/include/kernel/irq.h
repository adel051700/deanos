#ifndef _KERNEL_IRQ_H
#define _KERNEL_IRQ_H

#include <stdint.h>
#include "interrupt.h"  // struct registers

typedef void (*irq_handler_t)(struct registers* regs);

void irq_install_handler(uint8_t irq, irq_handler_t handler);
void irq_uninstall_handler(uint8_t irq);
void irq_dispatch(uint8_t irq, struct registers* regs);

#endif