#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/blk.h>

static void write_str(int fd, const char* s) {
    size_t len = 0;
    while (s[len]) len++;
    write(fd, s, len);
}

static void write_int(int fd, int v) {
    char buf[12];
    int i = 11;
    buf[i] = '\0';
    int neg = v < 0;
    unsigned uv = neg ? (unsigned)(-(v + 1)) + 1u : (unsigned)v;
    do {
        buf[--i] = (char)('0' + (uv % 10u));
        uv /= 10u;
    } while (uv != 0u);
    if (neg) buf[--i] = '-';
    write_str(fd, &buf[i]);
}

static void write_uint(int fd, unsigned v) {
    char buf[12];
    int i = 11;
    buf[i] = '\0';
    do {
        buf[--i] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u);
    write_str(fd, &buf[i]);
}

static void write_field_int(int fd, const char* label, int v) {
    write_str(fd, label);
    write_int(fd, v);
    write_str(fd, "\n");
}

static void write_field_uint(int fd, const char* label, unsigned v) {
    write_str(fd, label);
    write_uint(fd, v);
    write_str(fd, "\n");
}

static uint8_t rbuf[2048];
static uint8_t wbuf[2048];
static uint8_t vbuf[2048];

int main(void) {
    int fd = open("/blktest.out", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return 1;

    struct blk_info info;
    unsigned idx = 0;
    unsigned dev_count = 0;
    int writable_dev = -1;
    unsigned writable_block_size = 0;

    while (blk_info(idx, &info) == 0) {
        write_str(fd, "dev[");
        write_uint(fd, idx);
        write_str(fd, "].name=");
        write_str(fd, info.name);
        write_str(fd, "\n");
        write_str(fd, "dev[");
        write_uint(fd, idx);
        write_str(fd, "].block_size=");
        write_uint(fd, info.block_size);
        write_str(fd, "\n");
        write_str(fd, "dev[");
        write_uint(fd, idx);
        write_str(fd, "].flags=");
        write_uint(fd, info.flags);
        write_str(fd, "\n");

        if (writable_dev < 0 && !(info.flags & BLOCKDEV_FLAG_ATAPI)
            && info.block_size > 0u && info.block_size <= 2048u) {
            writable_dev = (int)idx;
            writable_block_size = info.block_size;
        }

        idx++;
        dev_count++;
    }
    write_field_uint(fd, "dev_count=", dev_count);

    int read_rc = blk_read(0, 0, rbuf);
    write_field_int(fd, "read_dev0_rc=", read_rc);

    struct blk_cache_stats cs;
    int cache_rc = blk_cache_stats(&cs);
    write_field_int(fd, "cache_stats_rc=", cache_rc);

    int flush_rc = blk_flush(-1);
    write_field_int(fd, "flush_all_rc=", flush_rc);

    if (writable_dev >= 0) {
        for (unsigned i = 0; i < writable_block_size; i++) {
            wbuf[i] = (uint8_t)(i & 0xFFu);
        }
        int write_rc = blk_write((unsigned)writable_dev, 0, wbuf);
        write_field_int(fd, "write_rc=", write_rc);

        int verify_rc = blk_read((unsigned)writable_dev, 0, vbuf);
        write_field_int(fd, "verify_read_rc=", verify_rc);

        int match = 1;
        for (unsigned i = 0; i < writable_block_size; i++) {
            if (vbuf[i] != wbuf[i]) { match = 0; break; }
        }
        write_field_int(fd, "write_verify_match=", match);
    } else {
        write_str(fd, "write_test_skipped=1\n");
    }

    close(fd);
    return 0;
}
