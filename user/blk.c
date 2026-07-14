/* /bin/blk — block device tool, ported from the kernel-shell builtin
 * (see docs/superpowers/specs/2026-07-14-kernel-shell-retirement-design.md). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/blk.h>

static void print_hex_byte(unsigned v) {
    static const char* hex = "0123456789ABCDEF";
    char buf[3];
    buf[0] = hex[(v >> 4) & 0xF];
    buf[1] = hex[v & 0xF];
    buf[2] = '\0';
    printf("%s", buf);
}

static int is_decimal(const char* s) {
    if (!s || !*s) return 0;
    for (const char* p = s; *p; ++p) if (*p < '0' || *p > '9') return 0;
    return 1;
}

static void cmd_list(void) {
    printf("ID  NAME  BSIZE  BLOCKS  FLAGS\n");
    printf("--  ----  -----  ------  -----\n");
    int any = 0;
    for (unsigned i = 0; ; ++i) {
        struct blk_info d;
        if (blk_info(i, &d) != 0) break;
        any = 1;
        printf("%d   %s    %d    %d    ", (int)d.id, d.name, (int)d.block_size, (int)d.block_count);
        if (d.flags & BLOCKDEV_FLAG_READONLY) printf("RO ");
        if (d.flags & BLOCKDEV_FLAG_ATAPI) printf("ATAPI ");
        if (d.flags & BLOCKDEV_FLAG_PARTITION) printf("PART ");
        if (d.flags == 0) printf("-");
        printf("\n");
    }
    if (!any) printf("(no block devices)\n");
}

static void cmd_read(unsigned dev, unsigned lba) {
    struct blk_info d;
    if (blk_info(dev, &d) != 0) { printf("blk: invalid device id\n"); return; }
    if (d.block_size > 2048u) { printf("blk: block size too large for shell dump\n"); return; }

    uint8_t sector[2048];
    if (blk_read(dev, lba, sector) != 0) { printf("blk: read failed\n"); return; }

    printf("blk: first 64 bytes\n");
    for (unsigned i = 0; i < 64; ++i) {
        print_hex_byte(sector[i]);
        printf(((i & 0x0Fu) == 0x0Fu) ? "\n" : " ");
    }
    if (d.block_size >= 512u) {
        printf("sig[510..511]=");
        print_hex_byte(sector[510]);
        printf(" ");
        print_hex_byte(sector[511]);
        printf("\n");
    }
}

static void cmd_write(unsigned dev, unsigned lba, unsigned seed) {
    if (seed > 255u) { printf("blk: seed-byte must be 0..255\n"); return; }
    struct blk_info d;
    if (blk_info(dev, &d) != 0) { printf("blk: invalid device id\n"); return; }
    if (d.block_size > 2048u) { printf("blk: block size too large for shell write test\n"); return; }
    if (d.flags & BLOCKDEV_FLAG_ATAPI) { printf("blk: refusing writes to ATAPI device\n"); return; }

    uint8_t tx[2048], rx[2048];
    for (unsigned i = 0; i < d.block_size; ++i) { tx[i] = (uint8_t)((seed + i) & 0xFFu); rx[i] = 0; }

    if (blk_write(dev, lba, tx) != 0) { printf("blk: write failed\n"); return; }
    if (blk_read(dev, lba, rx) != 0) { printf("blk: readback failed\n"); return; }

    int mismatch = -1;
    for (unsigned i = 0; i < d.block_size; ++i) {
        if (rx[i] != tx[i]) { mismatch = (int)i; break; }
    }
    if (mismatch >= 0) {
        printf("blk: verify mismatch at byte %d (expected ", mismatch);
        print_hex_byte(tx[mismatch]);
        printf(", got ");
        print_hex_byte(rx[mismatch]);
        printf(")\n");
        return;
    }
    printf("blk: write+readback verify ok\n");
}

static void cmd_cache(void) {
    struct blk_cache_stats st;
    if (blk_cache_stats(&st) != 0) { printf("blk: cache stats failed\n"); return; }
    printf("blk cache: entries=%d hits=%d misses=%d evictions=%d writebacks=%d "
           "async_submitted=%d async_completed=%d async_failed=%d async_pending=%d\n",
           (int)st.entries, (int)st.hits, (int)st.misses, (int)st.evictions, (int)st.writebacks,
           (int)st.async_submitted, (int)st.async_completed, (int)st.async_failed, (int)st.async_pending);
}

int main(int argc, char** argv) {
    if (argc < 2 || strcmp(argv[1], "list") == 0) { cmd_list(); return 0; }

    if (strcmp(argv[1], "read") == 0) {
        if (argc < 4 || !is_decimal(argv[2]) || !is_decimal(argv[3])) {
            printf("usage: blk read <dev> <lba>\n");
            return 1;
        }
        cmd_read((unsigned)atoi(argv[2]), (unsigned)atoi(argv[3]));
        return 0;
    }

    if (strcmp(argv[1], "write") == 0) {
        if (argc < 5 || !is_decimal(argv[2]) || !is_decimal(argv[3]) || !is_decimal(argv[4])) {
            printf("usage: blk write <dev> <lba> <seed-byte>\n");
            return 1;
        }
        cmd_write((unsigned)atoi(argv[2]), (unsigned)atoi(argv[3]), (unsigned)atoi(argv[4]));
        return 0;
    }

    if (strcmp(argv[1], "cache") == 0 || strcmp(argv[1], "async") == 0) { cmd_cache(); return 0; }

    if (strcmp(argv[1], "flush") == 0) {
        int dev = (argc >= 3 && is_decimal(argv[2])) ? atoi(argv[2]) : -1;
        if (blk_flush(dev) != 0) {
            printf(dev < 0 ? "blk: flush failed\n" : "blk: device flush failed\n");
            return 1;
        }
        printf(dev < 0 ? "blk: all cached blocks flushed\n" : "blk: device cache flushed\n");
        return 0;
    }

    printf("usage: blk list | blk read <dev> <lba> | blk write <dev> <lba> <seed-byte> | "
           "blk cache | blk async | blk flush [dev]\n");
    return 1;
}
