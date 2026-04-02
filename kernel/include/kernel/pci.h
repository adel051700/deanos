#ifndef _KERNEL_PCI_H
#define _KERNEL_PCI_H

#include <stdint.h>

typedef struct pci_device_info {
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision;
    uint8_t header_type;
    uint8_t irq_line;
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t bars[6];
    uint32_t io_base;
    uint32_t mmio_base;
} pci_device_info_t;

void pci_initialize(void);

uint32_t pci_read_config32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset);
uint16_t pci_read_config16(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset);
uint8_t pci_read_config8(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset);
void pci_write_config32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint32_t value);
void pci_write_config16(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint16_t value);

int pci_find_device(uint16_t vendor_id, uint16_t device_id, pci_device_info_t* out);
int pci_enable_bus_mastering(const pci_device_info_t* dev);

#endif

