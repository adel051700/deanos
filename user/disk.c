/* /bin/disk — disk management tool, ported from the kernel-shell builtin
 * (see docs/superpowers/specs/2026-07-14-disk-syscalls-design.md). */
#include <stdio.h>
#include <string.h>
#include <sys/disk.h>
#include <sys/blk.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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
    printf("  disk install <disk> <disk>   (WIPES target, type disk name twice to confirm)\n");
}

static void print_hex8(unsigned v) {
    static const char* hex = "0123456789ABCDEF";
    char buf[3];
    buf[0] = hex[(v >> 4) & 0xF];
    buf[1] = hex[v & 0xF];
    buf[2] = '\0';
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

/* Numeric blockdev index for name -> needed by blk_read/blk_write, which
 * take an index, not a name (unlike disk_ctl). */
static int resolve_dev_index(const char* name) {
    struct blk_info bi;
    for (unsigned i = 0; blk_info(i, &bi) == 0; i++) {
        if (strcmp(bi.name, name) == 0) return (int)bi.id;
    }
    return -1;
}

#define DISK_INSTALL_COPY_CHUNK   1024u
#define DISK_INSTALL_SECTOR       512u
#define DISK_INSTALL_BOOT_CODE_LEN 440u   /* preserve MBR partition table + signature */
#define DISK_INSTALL_PART_START_LBA 2048u /* must match kernel/mbr.c's MBR_TRACK_ALIGN_LBA */

static unsigned char g_install_copybuf[DISK_INSTALL_COPY_CHUNK];

static int copy_dev_to_file(const char* devpath, const char* destpath) {
    int sfd = open(devpath, O_RDONLY);
    if (sfd < 0) return -1;
    int dfd = open(destpath, O_CREAT | O_WRONLY);
    if (dfd < 0) { close(sfd); return -1; }
    ssize_t n;
    int ok = 1;
    while ((n = read(sfd, g_install_copybuf, DISK_INSTALL_COPY_CHUNK)) > 0) {
        if (write(dfd, g_install_copybuf, (size_t)n) != n) { ok = 0; break; }
    }
    if (n < 0) ok = 0;
    close(sfd);
    close(dfd);
    return ok ? 0 : -1;
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
    if (fat32) printf("disk: FAT32 ready at /mnt/%s\n", pname);
    else printf("disk: ready at /mnt/%s\n", pname);
    return 0;
}

static int cmd_install(const char* devname) {
    int diskidx = resolve_dev_index(devname);
    if (diskidx < 0) { printf("disk: unknown device\n"); return 1; }

    struct blk_info bi;
    if (blk_info((unsigned)diskidx, &bi) < 0) {
        printf("disk: unknown device\n");
        return 1;
    }
    if (bi.flags & BLOCKDEV_FLAG_ATAPI) {
        printf("disk: refusing to install onto a CD/ATAPI device\n");
        return 1;
    }

    int rc = disk_ctl(DISK_CTL_INIT_FAT32, devname);
    if (rc == -19) { printf("disk: unknown device\n"); return 1; } /* -ENODEV */
    if (rc < 0) { printf("disk: install failed while creating MBR\n"); return 1; }

    char pname[20];
    strcpy(pname, devname);
    strcat(pname, "p1");

    struct disk_part_info p;
    if (find_part(pname, &p) < 0) {
        printf("disk: install could not find new partition\n");
        return 1;
    }

    rc = disk_ctl(DISK_CTL_MKFS_FAT32, pname);
    if (rc == -19) { printf("disk: unknown device\n"); return 1; }
    if (rc < 0) { printf("disk: install format failed\n"); return 1; }

    rc = disk_ctl(DISK_CTL_MOUNT_FAT32, pname);
    if (rc == -19) { printf("disk: unknown device\n"); return 1; }
    if (rc < 0) { printf("disk: install mount failed\n"); return 1; }

    char kpath[40], cfgpath[40];
    strcpy(kpath, "/mnt/"); strcat(kpath, pname); strcat(kpath, "/deanos.bin");
    strcpy(cfgpath, "/mnt/"); strcat(cfgpath, pname); strcat(cfgpath, "/grub.cfg");

    if (copy_dev_to_file("/dev/installkernel", kpath) < 0) {
        printf("disk: install could not copy kernel\n");
        return 1;
    }
    if (copy_dev_to_file("/dev/installgrubcfg", cfgpath) < 0) {
        printf("disk: install could not copy grub.cfg\n");
        return 1;
    }

    struct stat gcst;
    if (stat("/dev/installgrubcore", &gcst) < 0) {
        printf("disk: install missing grub boot blob\n");
        return 1;
    }
    unsigned core_sectors = ((unsigned)gcst.size + DISK_INSTALL_SECTOR - 1u) / DISK_INSTALL_SECTOR;
    if (core_sectors >= DISK_INSTALL_PART_START_LBA) {
        printf("disk: install grub image too large for the boot gap\n");
        return 1;
    }

    int gfd = open("/dev/installgrubcore", O_RDONLY);
    if (gfd < 0) { printf("disk: install could not open grub boot blob\n"); return 1; }

    unsigned char sector[DISK_INSTALL_SECTOR];
    if (read(gfd, sector, DISK_INSTALL_SECTOR) != (ssize_t)DISK_INSTALL_SECTOR) {
        printf("disk: install grub boot blob too short\n");
        close(gfd);
        return 1;
    }

    unsigned char mbr0[DISK_INSTALL_SECTOR];
    if (blk_read((unsigned)diskidx, 0, mbr0) < 0) {
        printf("disk: install could not read boot sector\n");
        close(gfd);
        return 1;
    }
    memcpy(mbr0, sector, DISK_INSTALL_BOOT_CODE_LEN);
    if (blk_write((unsigned)diskidx, 0, mbr0) < 0) {
        printf("disk: install could not write boot sector\n");
        close(gfd);
        return 1;
    }

    unsigned lba = 1;
    ssize_t n;
    while ((n = read(gfd, sector, DISK_INSTALL_SECTOR)) > 0) {
        if (n < (ssize_t)DISK_INSTALL_SECTOR) {
            memset(sector + n, 0, DISK_INSTALL_SECTOR - (size_t)n);
        }
        if (blk_write((unsigned)diskidx, lba, sector) < 0) {
            printf("disk: install could not write core image at sector %u\n", lba);
            close(gfd);
            return 1;
        }
        lba++;
    }
    close(gfd);

    /* blk_write() on the raw disk goes through blockdev.c's write-back
     * cache (128 entries); without an explicit flush the tail of the
     * grub core.img writes above can still be dirty-only in RAM when the
     * machine is powered off for a standalone boot test. */
    if (blk_flush((int)diskidx) < 0) {
        printf("disk: install could not flush writes to disk\n");
        return 1;
    }

    printf("disk: installed on %s -- boot with '-hda <image>' only, no -cdrom needed\n", devname);
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
    if (strcmp(cmd, "install") == 0) {
        if (argc < 4 || strcmp(argv[2], argv[3]) != 0) {
            printf("disk: install requires the disk name twice to confirm "
                   "(disk install <disk> <disk>) -- this WIPES the target disk\n");
            return 1;
        }
        return cmd_install(devname);
    }

    usage();
    return 1;
}
