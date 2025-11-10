#ifndef _KERNEL_MULTIBOOT_H
#define _KERNEL_MULTIBOOT_H

#include <stdint.h>

/* Multiboot2 basics */
#define MULTIBOOT2_MAGIC               0x36d76289
#define MULTIBOOT2_TAG_TYPE_END        0
#define MULTIBOOT2_TAG_TYPE_FRAMEBUFFER 8
#define MULTIBOOT2_TAG_TYPE_MMAP       6

struct multiboot_tag {
    uint32_t type;
    uint32_t size;
};

/* Framebuffer tag (you already used this) */
struct multiboot_tag_framebuffer {
    uint32_t type;
    uint32_t size;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    uint16_t reserved;
};

/* Memory map tag + entries */
struct multiboot_tag_mmap {
    uint32_t type;         // = 6
    uint32_t size;
    uint32_t entry_size;   // sizeof(multiboot_mmap_entry) (with possible padding)
    uint32_t entry_version;
    // followed by entries
};

struct multiboot_mmap_entry {
    uint64_t addr;         // base address
    uint64_t len;          // length in bytes
    uint32_t type;         // 1=available, others reserved or special
    uint32_t zero;         // reserved
};

/* E820 types we care about */
#define MB_MMAP_TYPE_AVAILABLE   1
#define MB_MMAP_TYPE_RESERVED    2
#define MB_MMAP_TYPE_ACPI_RECLAIMABLE 3
#define MB_MMAP_TYPE_NVS         4
#define MB_MMAP_TYPE_BADRAM      5

#endif /* _KERNEL_MULTIBOOT_H */