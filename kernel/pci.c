#include "include/kernel/pci.h"
#include "include/kernel/io.h"
#include "include/kernel/log.h"

#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

#define PCI_CMD_IO_SPACE   0x0001u
#define PCI_CMD_MEM_SPACE  0x0002u
#define PCI_CMD_BUS_MASTER 0x0004u

static uint32_t pci_make_addr(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    return (1u << 31) |
           ((uint32_t)bus << 16) |
           ((uint32_t)slot << 11) |
           ((uint32_t)function << 8) |
           (offset & 0xFCu);
}

uint32_t pci_read_config32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    outl(PCI_CONFIG_ADDR, pci_make_addr(bus, slot, function, offset));
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_read_config16(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    uint32_t v = pci_read_config32(bus, slot, function, offset);
    return (uint16_t)((v >> ((offset & 2u) * 8u)) & 0xFFFFu);
}

uint8_t pci_read_config8(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    uint32_t v = pci_read_config32(bus, slot, function, offset);
    return (uint8_t)((v >> ((offset & 3u) * 8u)) & 0xFFu);
}

void pci_write_config32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDR, pci_make_addr(bus, slot, function, offset));
    outl(PCI_CONFIG_DATA, value);
}

void pci_write_config16(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint16_t value) {
    uint32_t current = pci_read_config32(bus, slot, function, offset);
    uint32_t shift = (offset & 2u) * 8u;
    uint32_t mask = 0xFFFFu << shift;
    current = (current & ~mask) | ((uint32_t)value << shift);
    pci_write_config32(bus, slot, function, offset, current);
}

static int pci_read_device_info(uint8_t bus, uint8_t slot, uint8_t function,
                                uint16_t vendor_id, uint16_t device_id,
                                pci_device_info_t* out) {
    if (!out) return -1;

    out->bus = bus;
    out->slot = slot;
    out->function = function;
    out->vendor_id = vendor_id;
    out->device_id = device_id;
    out->revision = pci_read_config8(bus, slot, function, 0x08);
    out->prog_if = pci_read_config8(bus, slot, function, 0x09);
    out->subclass = pci_read_config8(bus, slot, function, 0x0A);
    out->class_code = pci_read_config8(bus, slot, function, 0x0B);
    out->header_type = pci_read_config8(bus, slot, function, 0x0E);
    out->irq_line = pci_read_config8(bus, slot, function, 0x3C);
    out->io_base = 0;
    out->mmio_base = 0;

    for (uint8_t i = 0; i < 6; ++i) {
        uint32_t bar = pci_read_config32(bus, slot, function, (uint8_t)(0x10 + i * 4));
        out->bars[i] = bar;
        if ((bar & 0x1u) != 0u) {
            if (out->io_base == 0) {
                out->io_base = bar & ~0x3u;
            }
        } else {
            if (out->mmio_base == 0 && bar != 0u) {
                out->mmio_base = bar & ~0xFu;
            }
        }
    }

    return 0;
}

int pci_find_device(uint16_t vendor_id, uint16_t device_id, pci_device_info_t* out) {
    for (uint16_t bus = 0; bus < 256; ++bus) {
        for (uint8_t slot = 0; slot < 32; ++slot) {
            uint16_t vendor0 = pci_read_config16((uint8_t)bus, slot, 0, 0x00);
            if (vendor0 == 0xFFFFu) continue;

            uint8_t header_type = pci_read_config8((uint8_t)bus, slot, 0, 0x0E);
            uint8_t function_count = (header_type & 0x80u) ? 8u : 1u;

            for (uint8_t function = 0; function < function_count; ++function) {
                uint16_t vendor = pci_read_config16((uint8_t)bus, slot, function, 0x00);
                if (vendor == 0xFFFFu) continue;

                uint16_t device = pci_read_config16((uint8_t)bus, slot, function, 0x02);
                if (vendor == vendor_id && device == device_id) {
                    return pci_read_device_info((uint8_t)bus, slot, function, vendor, device, out);
                }
            }
        }
    }

    return -1;
}

int pci_enable_bus_mastering(const pci_device_info_t* dev) {
    if (!dev) return -1;

    uint16_t cmd = pci_read_config16(dev->bus, dev->slot, dev->function, 0x04);
    cmd |= (uint16_t)(PCI_CMD_BUS_MASTER | PCI_CMD_MEM_SPACE | PCI_CMD_IO_SPACE);
    pci_write_config16(dev->bus, dev->slot, dev->function, 0x04, cmd);
    return 0;
}

void pci_initialize(void) {
}

