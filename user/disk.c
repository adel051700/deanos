/* /bin/disk — disk management tool, ported from the kernel-shell builtin
 * (see docs/superpowers/specs/2026-07-14-disk-syscalls-design.md). */
#include <stdio.h>
#include <string.h>
#include <sys/disk.h>
#include <sys/blk.h>

/* Mirror the kernel's token resolution (all-decimal = device index) so
 * messages and composed partition names use the real device name. */
static int resolve_dev_name(const char* tok, char* out, size_t outsz) {
    if (!tok || !*tok) return -1;
    int all_digits = 1;
    for (const char* s = tok; *s; s++) {
        if (*s < '0' || *s > '9') { all_digits = 0; break; }
    }
    if (!all_digits) {
        if (strlen(tok) + 1 > outsz) return -1;
        strcpy(out, tok);
        return 0;
    }
    unsigned idx = 0;
    for (const char* s = tok; *s; s++) idx = idx * 10u + (unsigned)(*s - '0');
    struct blk_info bi;
    if (blk_info(idx, &bi) < 0) return -1;
    if (strlen(bi.name) + 1 > outsz) return -1;
    strcpy(out, bi.name);
    return 0;
}

static void usage(void) {
    printf("usage:\n");
    printf("  disk parts\n");
    printf("  disk init <disk>\n");
    printf("  disk initfat32 <disk>\n");
    printf("  disk mkfs <partition>\n");
    printf("  disk mkfsfat32 <partition>\n");
    printf("  disk mount <partition>\n");
    printf("  disk markdirty <partition>\n");
    printf("  disk setup <disk>\n");
    printf("  disk mountfat32 <partition>\n");
    printf("  disk setupfat32 <disk>\n");
}

static void print_hex8(unsigned v) {
    char buf[8];
    itoa((int)(v & 0xFFu), buf, 16);
    if ((v & 0xFFu) < 0x10u) printf("0");
    printf("%s", buf);
}

static void cmd_parts(void) {
    disk_ctl(DISK_CTL_RESCAN, 0);

    struct disk_part_info p;
    if (disk_part_info(0, &p) != 0) {
        printf("disk: no MBR partitions detected\n");
        return;
    }

    printf("DEV     PARENT  TYPE  START   BLOCKS\n");
    printf("------  ------  ----  ------  ------\n");
    for (unsigned i = 0; disk_part_info(i, &p) == 0; i++) {
        printf("%s   %s    ", p.name, p.parent);
        print_hex8(p.type);
        printf("    %d    %d\n", (int)p.start_lba, (int)p.block_count);
    }
}

static int find_part(const char* name, struct disk_part_info* out) {
    for (unsigned i = 0; disk_part_info(i, out) == 0; i++) {
        if (strcmp(out->name, name) == 0) return 0;
    }
    return -1;
}

static int do_setup(const char* devname, int fat32) {
    const char* what = fat32 ? "setupfat32" : "setup";

    int rc = disk_ctl(fat32 ? DISK_CTL_INIT_FAT32 : DISK_CTL_INIT, devname);
    if (rc == -19) { printf("disk: unknown device\n"); return 1; } /* -ENODEV */
    if (rc < 0) {
        printf("disk: %s failed while creating MBR\n", what);
        return 1;
    }

    char pname[20];
    strcpy(pname, devname);
    strcat(pname, "p1");

    struct disk_part_info p;
    if (find_part(pname, &p) < 0) {
        printf("disk: %s could not find new partition\n", what);
        return 1;
    }
    rc = disk_ctl(fat32 ? DISK_CTL_MKFS_FAT32 : DISK_CTL_MKFS_MINFS, pname);
    if (rc == -19) { printf("disk: unknown device\n"); return 1; } /* -ENODEV */
    if (rc < 0) {
        printf("disk: %s format failed\n", what);
        return 1;
    }
    rc = disk_ctl(fat32 ? DISK_CTL_MOUNT_FAT32 : DISK_CTL_MOUNT_MINFS, pname);
    if (rc == -19) { printf("disk: unknown device\n"); return 1; } /* -ENODEV */
    if (rc < 0) {
        printf("disk: %s mount failed\n", what);
        return 1;
    }
    printf("disk: ready at /mnt/%s\n", pname);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2 || strcmp(argv[1], "help") == 0) {
        usage();
        return 0;
    }

    const char* cmd = argv[1];
    const char* dev = (argc > 2) ? argv[2] : 0;

    if (strcmp(cmd, "parts") == 0) { cmd_parts(); return 0; }

    if (!dev) {
        usage();
        return 1;
    }

    char devname[16];
    if (resolve_dev_name(dev, devname, sizeof(devname)) < 0) {
        printf("disk: unknown device\n");
        return 1;
    }

    if (strcmp(cmd, "init") == 0 || strcmp(cmd, "initfat32") == 0) {
        int fat32 = (cmd[4] != '\0');
        int rc = disk_ctl(fat32 ? DISK_CTL_INIT_FAT32 : DISK_CTL_INIT, devname);
        if (rc == -19) { printf("disk: unknown device\n"); return 1; } /* -ENODEV */
        if (rc < 0) {
            printf("disk: failed to write MBR\n");
            return 1;
        }
        printf("disk: created single %s partition on %s (not formatted)\n",
               fat32 ? "FAT32" : "Linux", devname);
        return 0;
    }
    if (strcmp(cmd, "mkfs") == 0) {
        int rc = disk_ctl(DISK_CTL_MKFS_MINFS, devname);
        if (rc == -19) { printf("disk: unknown device\n"); return 1; } /* -ENODEV */
        if (rc < 0) { printf("disk: mkfs failed\n"); return 1; }
        printf("disk: minfs formatted on %s\n", devname);
        return 0;
    }
    if (strcmp(cmd, "mkfsfat32") == 0) {
        int rc = disk_ctl(DISK_CTL_MKFS_FAT32, devname);
        if (rc == -19) { printf("disk: unknown device\n"); return 1; } /* -ENODEV */
        if (rc < 0) { printf("disk: mkfsfat32 failed\n"); return 1; }
        printf("disk: FAT32 formatted on %s\n", devname);
        return 0;
    }
    if (strcmp(cmd, "mount") == 0) {
        int rc = disk_ctl(DISK_CTL_MOUNT_MINFS, devname);
        if (rc == -19) { printf("disk: unknown device\n"); return 1; } /* -ENODEV */
        if (rc < 0) { printf("disk: mount failed\n"); return 1; }
        printf("disk: mounted /mnt/%s\n", devname);
        return 0;
    }
    if (strcmp(cmd, "mountfat32") == 0) {
        int rc = disk_ctl(DISK_CTL_MOUNT_FAT32, devname);
        if (rc == -19) { printf("disk: unknown device\n"); return 1; } /* -ENODEV */
        if (rc < 0) { printf("disk: FAT32 mount failed\n"); return 1; }
        printf("disk: mounted FAT32 at /mnt/%s\n", devname);
        return 0;
    }
    if (strcmp(cmd, "markdirty") == 0) {
        int rc = disk_ctl(DISK_CTL_MARKDIRTY, devname);
        if (rc == -19) { printf("disk: unknown device\n"); return 1; } /* -ENODEV */
        if (rc < 0) {
            printf("disk: markdirty failed (mount minfs first)\n");
            return 1;
        }
        printf("disk: recovery marker set DIRTY on %s\n", devname);
        return 0;
    }
    if (strcmp(cmd, "setup") == 0)      return do_setup(devname, 0);
    if (strcmp(cmd, "setupfat32") == 0) return do_setup(devname, 1);

    usage();
    return 1;
}
