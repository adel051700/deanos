#include "include/kernel/pic.h"
#include "include/kernel/io.h"

#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1

#define ICW1_INIT  0x10
#define ICW1_ICW4  0x01
#define ICW4_8086  0x01
#define PIC_EOI    0x20

static inline void pic_remap(uint8_t off1, uint8_t off2) {
    uint8_t m1 = inb(PIC1_DATA);
    uint8_t m2 = inb(PIC2_DATA);

    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4); io_wait();

    outb(PIC1_DATA, off1); io_wait();
    outb(PIC2_DATA, off2); io_wait();

    outb(PIC1_DATA, 0x04); io_wait(); // tell Master about Slave at IRQ2
    outb(PIC2_DATA, 0x02); io_wait(); // tell Slave its cascade identity

    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    outb(PIC1_DATA, m1); io_wait();
    outb(PIC2_DATA, m2); io_wait();
}

void pic_init(void) {
    pic_remap(0x20, 0x28);

    // Mask all then unmask PIT(0) and KBD(1)
    outb(PIC1_DATA, 0xFF); io_wait();
    outb(PIC2_DATA, 0xFF); io_wait();
    // Unmask 0,1
    uint8_t m1 = inb(PIC1_DATA);
    m1 &= ~(1u << 0);
    m1 &= ~(1u << 1);
    outb(PIC1_DATA, m1);
}

void pic_mask_irq(uint8_t irq) {
    if (irq < 8) {
        uint8_t m = inb(PIC1_DATA) | (1u << irq);
        outb(PIC1_DATA, m);
    } else {
        irq -= 8;
        uint8_t m = inb(PIC2_DATA) | (1u << irq);
        outb(PIC2_DATA, m);
    }
}

void pic_unmask_irq(uint8_t irq) {
    if (irq < 8) {
        uint8_t m = inb(PIC1_DATA) & ~(1u << irq);
        outb(PIC1_DATA, m);
    } else {
        irq -= 8;
        uint8_t m = inb(PIC2_DATA) & ~(1u << irq);
        outb(PIC2_DATA, m);
    }
}

void pic_eoi(uint8_t irq) {
    if (irq >= 8) outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}