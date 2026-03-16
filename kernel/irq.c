#include "include/kernel/irq.h"
#include "include/kernel/pic.h"
#include "include/kernel/log.h"

static irq_handler_t irq_handlers[16] = {0};

void irq_install_handler(uint8_t irq, irq_handler_t handler) {
    if (irq < 16) {
        irq_handlers[irq] = handler;
        if (irq >= 8) {
            /* Slave PIC lines require master IRQ2 (cascade) unmasked. */
            pic_unmask_irq(2);
        }
        pic_unmask_irq(irq);
    }
}

void irq_uninstall_handler(uint8_t irq) {
    if (irq < 16) {
        irq_handlers[irq] = 0;
        pic_mask_irq(irq);
    }
}

void irq_dispatch(uint8_t irq, struct registers* regs) {
    if (irq < 16 && irq_handlers[irq]) {
        irq_handlers[irq](regs);
    } else {
        klog("irq: unhandled");
    }
    pic_eoi(irq);
    (void)regs;
}