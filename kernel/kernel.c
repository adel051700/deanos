#include "include/kernel/idt.h"
#include "include/kernel/gdt.h"
#include "include/kernel/keyboard.h"
#include "include/kernel/mouse.h"
#include "include/kernel/shell.h"
#include "include/kernel/rtc.h"
#include "include/kernel/task.h"
#include "include/kernel/log.h"
#include "include/kernel/pit.h"
#include "include/kernel/serial.h"
#include "include/kernel/syscall.h"
#include "include/kernel/tss.h"
#include "include/kernel/vfs.h"
#include "include/kernel/ramfs.h"
#include "include/kernel/minfs.h"
#include "include/kernel/fat32.h"
#include "include/kernel/elf.h"
#include "include/kernel/blockdev.h"
#include "include/kernel/ata.h"
#include "include/kernel/mbr.h"
#include "include/kernel/paging.h"
#include "include/kernel/net.h"

#include <stdint.h>

static void shell_task(void) {
    while (1) {
        /* Drain all pending keystrokes so commands run immediately. */
        while (keyboard_data_available()) {
            char c = keyboard_getchar();
            if (c != 0)
                shell_process_char(c);
        }
        __asm__ __volatile__("hlt; nop");
    }
}


void kernel_main(void) {
    serial_initialize();
    klog_init();
    serial_write_buf("Serial initialized.\n", 20);

    gdt_initialize();
    tss_initialize(0x10, 0);    /* kernel SS=0x10, ESP0 updated per-task */
    idt_initialize();
    syscall_initialize();
    pit_initialize(100);
    keyboard_initialize();
    mouse_initialize();

    blockdev_initialize();
    mbr_initialize();
    ata_initialize();
    (void)net_initialize();

    ata_probe_summary_t ata_summary = {0};
    ata_probe_get_summary(&ata_summary);
    if (ata_summary.devices_registered == 0) {
        klog("storage: no ATA/ATAPI block devices registered");
    } else {
        klog("storage: ATA/ATAPI probing complete");
    }

    int partitions_found = mbr_scan_all();
    mbr_scan_stats_t mbr_stats = {0};
    mbr_get_scan_stats(&mbr_stats);
    if (partitions_found == 0 && ata_summary.devices_registered > 0) {
        klog("storage: no MBR partitions discovered");
    }
    if (mbr_stats.invalid_mbr_signature > 0) {
        klog("storage: one or more disks had no valid MBR signature");
    }

    (void)paging_swap_initialize();

    /* Filesystem: must come after kheap (initialized in constructor) */
    vfs_initialize();
    ramfs_initialize();
    minfs_auto_mount();
    (void)fat32_auto_mount((uint32_t)-1);
    elf_install_test_programs();

    shell_initialize();
    tasking_initialize();
    if (task_create_named(shell_task, 0, TASK_DEFAULT_QUANTUM, "shell") < 0) {
        klog("shell task creation failed");
    }

    interrupts_enable();
    rtc_initialize();

    while (1) {
        __asm__ __volatile__("hlt");
    }
}
