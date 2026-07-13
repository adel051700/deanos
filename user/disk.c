/* /bin/disk — disk management tool, ported from the kernel-shell builtin
 * (see docs/superpowers/specs/2026-07-14-disk-syscalls-design.md). */
#include <stdio.h>
#include <string.h>
#include <sys/disk.h>

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

static int do_setup(const char* dev, int fat32) {
    const char* what = fat32 ? "setupfat32" : "setup";

    if (disk_ctl(fat32 ? DISK_CTL_INIT_FAT32 : DISK_CTL_INIT, dev) < 0) {
        printf("disk: %s failed while creating MBR\n", what);
        return 1;
    }

    char pname[20];
    if (strlen(dev) > 15) {
        printf("disk: unknown device\n");
        return 1;
    }
    strcpy(pname, dev);
    strcat(pname, "p1");

    struct disk_part_info p;
    if (find_part(pname, &p) < 0) {
        printf("disk: %s could not find new partition\n", what);
        return 1;
    }
    if (disk_ctl(fat32 ? DISK_CTL_MKFS_FAT32 : DISK_CTL_MKFS_MINFS, pname) < 0) {
        printf("disk: %s format failed\n", what);
        return 1;
    }
    if (disk_ctl(fat32 ? DISK_CTL_MOUNT_FAT32 : DISK_CTL_MOUNT_MINFS, pname) < 0) {
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

    if (strcmp(cmd, "init") == 0 || strcmp(cmd, "initfat32") == 0) {
        int fat32 = (cmd[4] != '\0');
        if (disk_ctl(fat32 ? DISK_CTL_INIT_FAT32 : DISK_CTL_INIT, dev) < 0) {
            printf("disk: failed to write MBR\n");
            return 1;
        }
        printf("disk: created single %s partition on %s (not formatted)\n",
               fat32 ? "FAT32" : "Linux", dev);
        return 0;
    }
    if (strcmp(cmd, "mkfs") == 0) {
        if (disk_ctl(DISK_CTL_MKFS_MINFS, dev) < 0) { printf("disk: mkfs failed\n"); return 1; }
        printf("disk: minfs formatted on %s\n", dev);
        return 0;
    }
    if (strcmp(cmd, "mkfsfat32") == 0) {
        if (disk_ctl(DISK_CTL_MKFS_FAT32, dev) < 0) { printf("disk: mkfsfat32 failed\n"); return 1; }
        printf("disk: FAT32 formatted on %s\n", dev);
        return 0;
    }
    if (strcmp(cmd, "mount") == 0) {
        if (disk_ctl(DISK_CTL_MOUNT_MINFS, dev) < 0) { printf("disk: mount failed\n"); return 1; }
        printf("disk: mounted /mnt/%s\n", dev);
        return 0;
    }
    if (strcmp(cmd, "mountfat32") == 0) {
        if (disk_ctl(DISK_CTL_MOUNT_FAT32, dev) < 0) { printf("disk: FAT32 mount failed\n"); return 1; }
        printf("disk: mounted FAT32 at /mnt/%s\n", dev);
        return 0;
    }
    if (strcmp(cmd, "markdirty") == 0) {
        if (disk_ctl(DISK_CTL_MARKDIRTY, dev) < 0) {
            printf("disk: markdirty failed (mount minfs first)\n");
            return 1;
        }
        printf("disk: recovery marker set DIRTY on %s\n", dev);
        return 0;
    }
    if (strcmp(cmd, "setup") == 0)      return do_setup(dev, 0);
    if (strcmp(cmd, "setupfat32") == 0) return do_setup(dev, 1);

    usage();
    return 1;
}
