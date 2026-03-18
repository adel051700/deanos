#include "include/kernel/ata.h"

#include "include/kernel/blockdev.h"
#include "include/kernel/io.h"
#include "include/kernel/log.h"

#include <stdint.h>

#define ATA_MAX_DEVICES 4

#define ATA_PRIMARY_IO   0x1F0
#define ATA_PRIMARY_CTRL 0x3F6
#define ATA_SECONDARY_IO 0x170
#define ATA_SECONDARY_CTRL 0x376

#define ATA_REG_DATA       0
#define ATA_REG_FEATURES   1
#define ATA_REG_SECCOUNT0  2
#define ATA_REG_LBA0       3
#define ATA_REG_LBA1       4
#define ATA_REG_LBA2       5
#define ATA_REG_HDDEVSEL   6
#define ATA_REG_COMMAND    7
#define ATA_REG_STATUS     7

#define ATA_SR_ERR  0x01
#define ATA_SR_DRQ  0x08
#define ATA_SR_DF   0x20
#define ATA_SR_DRDY 0x40
#define ATA_SR_BSY  0x80

#define ATA_CMD_IDENTIFY         0xEC
#define ATA_CMD_IDENTIFY_PACKET  0xA1
#define ATA_CMD_READ_PIO         0x20
#define ATA_CMD_WRITE_PIO        0x30
#define ATA_CMD_CACHE_FLUSH      0xE7
#define ATA_CMD_PACKET           0xA0

/*
 * Flushing on every write is very expensive under emulators and can add
 * seconds to tiny writes. Keep this off for speed; add explicit sync support
 * later if stronger durability guarantees are needed.
 */
#define ATA_FLUSH_ON_EACH_WRITE 0

typedef struct ata_device {
    uint16_t io_base;
    uint16_t ctrl_base;
    uint8_t slave;
    uint8_t present;
    uint8_t atapi;
    uint32_t sectors_28;
    char model[41];
} ata_device_t;

static ata_device_t g_ata[ATA_MAX_DEVICES];

static void ata_io_wait(void) {
    io_wait();
    io_wait();
    io_wait();
    io_wait();
}

static uint8_t ata_status(const ata_device_t* d) {
    return inb((uint16_t)(d->io_base + ATA_REG_STATUS));
}

static int ata_wait_not_bsy(const ata_device_t* d, uint32_t spins) {
    while (spins--) {
        if ((ata_status(d) & ATA_SR_BSY) == 0)
            return 1;
    }
    return 0;
}

static int ata_wait_drq(const ata_device_t* d, uint32_t spins) {
    while (spins--) {
        uint8_t s = ata_status(d);
        if (s & ATA_SR_ERR) return 0;
        if (s & ATA_SR_DF) return 0;
        if ((s & ATA_SR_BSY) == 0 && (s & ATA_SR_DRQ))
            return 1;
    }
    return 0;
}

static int ata_wait_ready(const ata_device_t* d, uint32_t spins) {
    while (spins--) {
        uint8_t s = ata_status(d);
        if (s & ATA_SR_ERR) return 0;
        if (s & ATA_SR_DF) return 0;
        if ((s & ATA_SR_BSY) == 0)
            return 1;
    }
    return 0;
}

static void ata_select(const ata_device_t* d) {
    outb((uint16_t)(d->io_base + ATA_REG_HDDEVSEL), (uint8_t)(0xA0u | (d->slave << 4)));
    ata_io_wait();
}

static void ata_identify_model(char out[41], const uint16_t* id) {
    for (int i = 0; i < 40; i += 2) {
        uint16_t w = id[27 + (i / 2)];
        out[i] = (char)(w >> 8);
        out[i + 1] = (char)(w & 0xFF);
    }
    out[40] = '\0';

    for (int i = 39; i >= 0; --i) {
        if (out[i] == ' ' || out[i] == '\0') out[i] = '\0';
        else break;
    }
}

static int ata_probe_device(ata_device_t* d) {
    uint16_t identify[256];

    ata_select(d);
    outb((uint16_t)(d->io_base + ATA_REG_SECCOUNT0), 0);
    outb((uint16_t)(d->io_base + ATA_REG_LBA0), 0);
    outb((uint16_t)(d->io_base + ATA_REG_LBA1), 0);
    outb((uint16_t)(d->io_base + ATA_REG_LBA2), 0);
    outb((uint16_t)(d->io_base + ATA_REG_COMMAND), ATA_CMD_IDENTIFY);

    uint8_t status = ata_status(d);
    if (status == 0) return 0;

    if (!ata_wait_not_bsy(d, 1000000u)) return 0;

    uint8_t lba1 = inb((uint16_t)(d->io_base + ATA_REG_LBA1));
    uint8_t lba2 = inb((uint16_t)(d->io_base + ATA_REG_LBA2));
    if (lba1 == 0x14 && lba2 == 0xEB) {
        d->atapi = 1;
    }

    if (d->atapi) {
        outb((uint16_t)(d->io_base + ATA_REG_COMMAND), ATA_CMD_IDENTIFY_PACKET);
    }

    if (!ata_wait_drq(d, 1000000u)) return 0;

    insw((uint16_t)(d->io_base + ATA_REG_DATA), identify, 256);

    d->present = 1;
    d->sectors_28 = ((uint32_t)identify[61] << 16) | identify[60];
    ata_identify_model(d->model, identify);
    return 1;
}

static int ata_pio_read_blocks(void* ctx, uint64_t lba, uint32_t count, void* buffer) {
    ata_device_t* d = (ata_device_t*)ctx;
    if (!d || !buffer || count == 0) return -1;
    if (d->atapi) return -2;
    if (lba > 0x0FFFFFFFULL) return -3;

    uint8_t* dst = (uint8_t*)buffer;

    for (uint32_t s = 0; s < count; ++s) {
        uint32_t cur = (uint32_t)(lba + s);
        uint8_t head = (uint8_t)((cur >> 24) & 0x0F);

        if (!ata_wait_not_bsy(d, 1000000u)) return -4;

        outb((uint16_t)(d->io_base + ATA_REG_HDDEVSEL), (uint8_t)(0xE0u | (d->slave << 4) | head));
        outb((uint16_t)(d->io_base + ATA_REG_FEATURES), 0x00);
        outb((uint16_t)(d->io_base + ATA_REG_SECCOUNT0), 1);
        outb((uint16_t)(d->io_base + ATA_REG_LBA0), (uint8_t)(cur & 0xFF));
        outb((uint16_t)(d->io_base + ATA_REG_LBA1), (uint8_t)((cur >> 8) & 0xFF));
        outb((uint16_t)(d->io_base + ATA_REG_LBA2), (uint8_t)((cur >> 16) & 0xFF));
        outb((uint16_t)(d->io_base + ATA_REG_COMMAND), ATA_CMD_READ_PIO);

        if (!ata_wait_drq(d, 1000000u)) return -5;

        insw((uint16_t)(d->io_base + ATA_REG_DATA), dst + (s * 512u), 256);
        ata_io_wait();
    }

    return 0;
}

static int ata_pio_write_blocks(void* ctx, uint64_t lba, uint32_t count, const void* buffer) {
    ata_device_t* d = (ata_device_t*)ctx;
    if (!d || !buffer || count == 0) return -1;
    if (d->atapi) return -2;
    if (lba > 0x0FFFFFFFULL) return -3;

    const uint8_t* src = (const uint8_t*)buffer;

    for (uint32_t s = 0; s < count; ++s) {
        uint32_t cur = (uint32_t)(lba + s);
        uint8_t head = (uint8_t)((cur >> 24) & 0x0F);

        if (!ata_wait_not_bsy(d, 1000000u)) return -4;

        outb((uint16_t)(d->io_base + ATA_REG_HDDEVSEL), (uint8_t)(0xE0u | (d->slave << 4) | head));
        outb((uint16_t)(d->io_base + ATA_REG_FEATURES), 0x00);
        outb((uint16_t)(d->io_base + ATA_REG_SECCOUNT0), 1);
        outb((uint16_t)(d->io_base + ATA_REG_LBA0), (uint8_t)(cur & 0xFF));
        outb((uint16_t)(d->io_base + ATA_REG_LBA1), (uint8_t)((cur >> 8) & 0xFF));
        outb((uint16_t)(d->io_base + ATA_REG_LBA2), (uint8_t)((cur >> 16) & 0xFF));
        outb((uint16_t)(d->io_base + ATA_REG_COMMAND), ATA_CMD_WRITE_PIO);

        if (!ata_wait_drq(d, 1000000u)) return -5;

        outsw((uint16_t)(d->io_base + ATA_REG_DATA), src + (s * 512u), 256);
        ata_io_wait();

        if (!ata_wait_ready(d, 1000000u)) return -6;
    }

    if (ATA_FLUSH_ON_EACH_WRITE) {
        outb((uint16_t)(d->io_base + ATA_REG_COMMAND), ATA_CMD_CACHE_FLUSH);
        if (!ata_wait_ready(d, 1000000u)) return -7;
    }

    return 0;
}

static int ata_atapi_packet_read(ata_device_t* d, uint32_t lba, uint32_t count, void* buffer) {
    if (!d || !buffer || count == 0) return -1;

    uint8_t* dst = (uint8_t*)buffer;
    for (uint32_t s = 0; s < count; ++s) {
        uint8_t pkt[12] = {0};
        pkt[0] = 0xA8; /* READ(12) */
        uint32_t cur = lba + s;
        pkt[2] = (uint8_t)((cur >> 24) & 0xFF);
        pkt[3] = (uint8_t)((cur >> 16) & 0xFF);
        pkt[4] = (uint8_t)((cur >> 8) & 0xFF);
        pkt[5] = (uint8_t)(cur & 0xFF);
        pkt[9] = 1; /* transfer length = 1 logical block */

        if (!ata_wait_not_bsy(d, 1000000u)) return -2;

        outb((uint16_t)(d->io_base + ATA_REG_HDDEVSEL), (uint8_t)(0xA0u | (d->slave << 4)));
        outb((uint16_t)(d->io_base + ATA_REG_FEATURES), 0x00);
        outb((uint16_t)(d->io_base + ATA_REG_LBA1), 0x00);
        outb((uint16_t)(d->io_base + ATA_REG_LBA2), 0x08); /* 2048 bytes */
        outb((uint16_t)(d->io_base + ATA_REG_COMMAND), ATA_CMD_PACKET);

        if (!ata_wait_drq(d, 1000000u)) return -3;
        outsw((uint16_t)(d->io_base + ATA_REG_DATA), pkt, 6);

        if (!ata_wait_drq(d, 1000000u)) return -4;

        uint16_t xfer = (uint16_t)(((uint16_t)inb((uint16_t)(d->io_base + ATA_REG_LBA2)) << 8) |
                                   (uint16_t)inb((uint16_t)(d->io_base + ATA_REG_LBA1)));
        if (xfer == 0) xfer = 2048;
        if (xfer != 2048) return -5;

        insw((uint16_t)(d->io_base + ATA_REG_DATA), dst + (s * 2048u), xfer / 2u);
        ata_io_wait();

        if (!ata_wait_ready(d, 1000000u)) return -6;
    }

    return 0;
}

static int ata_atapi_read_capacity(ata_device_t* d, uint64_t* block_count, uint32_t* block_size) {
    if (!d || !block_count || !block_size) return 0;

    uint8_t pkt[12] = {0};
    uint8_t resp[8] = {0};
    pkt[0] = 0x25; /* READ CAPACITY(10) */

    if (!ata_wait_not_bsy(d, 1000000u)) return 0;

    outb((uint16_t)(d->io_base + ATA_REG_HDDEVSEL), (uint8_t)(0xA0u | (d->slave << 4)));
    outb((uint16_t)(d->io_base + ATA_REG_FEATURES), 0x00);
    outb((uint16_t)(d->io_base + ATA_REG_LBA1), 0x08);
    outb((uint16_t)(d->io_base + ATA_REG_LBA2), 0x00);
    outb((uint16_t)(d->io_base + ATA_REG_COMMAND), ATA_CMD_PACKET);

    if (!ata_wait_drq(d, 1000000u)) return 0;
    outsw((uint16_t)(d->io_base + ATA_REG_DATA), pkt, 6);

    if (!ata_wait_drq(d, 1000000u)) return 0;
    insw((uint16_t)(d->io_base + ATA_REG_DATA), resp, 4);
    ata_io_wait();

    if (!ata_wait_ready(d, 1000000u)) return 0;

    uint32_t last_lba = ((uint32_t)resp[0] << 24) |
                        ((uint32_t)resp[1] << 16) |
                        ((uint32_t)resp[2] << 8) |
                        (uint32_t)resp[3];
    uint32_t bs = ((uint32_t)resp[4] << 24) |
                  ((uint32_t)resp[5] << 16) |
                  ((uint32_t)resp[6] << 8) |
                  (uint32_t)resp[7];

    if (bs == 0) return 0;

    *block_size = bs;
    *block_count = (uint64_t)last_lba + 1u;
    return 1;
}

static int ata_atapi_read_blocks(void* ctx, uint64_t lba, uint32_t count, void* buffer) {
    ata_device_t* d = (ata_device_t*)ctx;
    if (!d || !buffer || count == 0) return -1;
    if (!d->atapi) return -2;
    if (lba > 0xFFFFFFFFULL) return -3;
    return ata_atapi_packet_read(d, (uint32_t)lba, count, buffer);
}

void ata_initialize(void) {
    g_ata[0] = (ata_device_t){ .io_base = ATA_PRIMARY_IO,   .ctrl_base = ATA_PRIMARY_CTRL,   .slave = 0 };
    g_ata[1] = (ata_device_t){ .io_base = ATA_PRIMARY_IO,   .ctrl_base = ATA_PRIMARY_CTRL,   .slave = 1 };
    g_ata[2] = (ata_device_t){ .io_base = ATA_SECONDARY_IO, .ctrl_base = ATA_SECONDARY_CTRL, .slave = 0 };
    g_ata[3] = (ata_device_t){ .io_base = ATA_SECONDARY_IO, .ctrl_base = ATA_SECONDARY_CTRL, .slave = 1 };

    int found = 0;
    for (uint32_t i = 0; i < ATA_MAX_DEVICES; ++i) {
        ata_device_t* d = &g_ata[i];
        if (!ata_probe_device(d)) continue;

        found++;
        block_device_t b = {0};
        b.ctx = d;
        if (d->atapi) {
            uint64_t atapi_blocks = 0;
            uint32_t atapi_bsize = 0;
            if (!ata_atapi_read_capacity(d, &atapi_blocks, &atapi_bsize)) {
                atapi_blocks = 1;
                atapi_bsize = 2048;
            }
            if (atapi_bsize == 0) atapi_bsize = 2048;

            b.block_size = atapi_bsize;
            b.block_count = atapi_blocks;
            b.flags = BLOCKDEV_FLAG_READONLY | BLOCKDEV_FLAG_ATAPI;
            b.read = ata_atapi_read_blocks;
            b.write = 0;
        } else {
            b.block_size = 512;
            b.block_count = d->sectors_28;
            b.flags = 0;
            b.read = ata_pio_read_blocks;
            b.write = ata_pio_write_blocks;
        }

        b.name[0] = 'h';
        b.name[1] = 'd';
        b.name[2] = '0' + (char)i;
        b.name[3] = '\0';

        if (blockdev_register(&b) >= 0) {
            if (d->atapi) {}
            else {};
        } else {
            klog("ata: failed to register disk");
        }
    }

    if (!found) {
        klog("ata: no devices detected");
    }
}

