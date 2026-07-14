# DeanOS Makefile

# Output directories
BUILD_DIR = build
KERNEL_BUILD_DIR = $(BUILD_DIR)/kernel
LIBC_BUILD_DIR = $(BUILD_DIR)/libc
ARCH_BUILD_DIR = $(BUILD_DIR)/arch/i386
USER_BUILD_DIR = $(BUILD_DIR)/user
LWIP_BUILD_DIR = $(BUILD_DIR)/lwip
LWIP_DIR = third_party/lwip
LWIPDIR = $(LWIP_DIR)/src
include $(LWIPDIR)/Filelists.mk
# Filelists.mk exports COREFILES CORE4FILES NETIFFILES (paths under $(LWIPDIR)/...)
# slipif.c requires sio_send/sio_open/sio_tryread serial stubs we don't provide.
LWIP_SRCS = $(COREFILES) $(CORE4FILES) $(filter-out $(LWIPDIR)/netif/slipif.c,$(NETIFFILES))
LWIP_PORT_SRCS = kernel/lwip_port/lwip_glue.c \
                 kernel/lwip_port/deanos_netif.c \
                 kernel/lwip_port/ksock_udp.c \
                 kernel/lwip_port/ksock_dns.c \
                 kernel/lwip_port/ksock_tcp.c
LWIP_OBJS = $(patsubst $(LWIP_DIR)/src/%.c,$(LWIP_BUILD_DIR)/%.o,$(LWIP_SRCS))
LWIP_OBJS += $(patsubst kernel/lwip_port/%.c,$(LWIP_BUILD_DIR)/port/%.o,$(LWIP_PORT_SRCS))
DESTDIR = isodir
DATE = $(shell date +%d-%m-%Y)
MAJOR = 0
MINOR = 8
VERSION = $(MAJOR)_$(MINOR)
ISO_DIR = isos
ISO_NAME = deanos-$(DATE)-$(VERSION).iso
ISO_PATH = $(ISO_DIR)/$(ISO_NAME)
# Cross-compiler settings
CFLAGS?=-O2 -g
CPPFLAGS?=
LDFLAGS?=
LIBS?=

# Compiler/Assembler/Linker
CC = /home/adel/opt/cross/bin/i686-elf-gcc
AS = /home/adel/opt/cross/bin/i686-elf-as
LD = /home/adel/opt/cross/bin/i686-elf-ld

# Includes
CFLAGS:=$(CFLAGS) -ffreestanding -Wall -Wextra -fno-pie -fno-stack-protector
CPPFLAGS:=$(CPPFLAGS) -Ikernel/include -Ilibc/include -Ikernel/lwip_port -Ithird_party/lwip/src/include
LDFLAGS:=$(LDFLAGS) -n -nostdlib
LIBS:=$(LIBS) -lgcc

# Kernel Files (source paths)
KERNEL_SRCS = \
kernel/kernel.c \
kernel/kernel_init.c \
kernel/framebuffer.c \
kernel/tty.c \
kernel/font8x16.c \
kernel/idt.c \
kernel/gdt.c \
kernel/io.c \
kernel/interrupt.c \
kernel/keyboard.c \
kernel/mouse.c \
kernel/shell.c \
kernel/signal.c \
kernel/rtc.c \
kernel/pit.c \
kernel/pmm.c \
kernel/paging.c \
kernel/kheap.c \
kernel/pic.c \
kernel/irq.c \
kernel/log.c \
kernel/serial.c \
kernel/blockdev.c \
kernel/ata.c \
kernel/mbr.c \
kernel/syscall.c \
kernel/task.c \
kernel/tss.c \
kernel/usermode.c \
kernel/vfs.c \
kernel/ramfs.c \
kernel/minfs.c \
kernel/fat32.c \
kernel/elf.c \
kernel/pci.c \
kernel/e1000.c \
kernel/rtl8139.c \
kernel/stack_protector.c \
kernel/random.c \
kernel/devrandom.c \
kernel/fault.c \
kernel/context_switch.s

ARCH_C_SRCS = \
arch/i386/boot/crti.c

ARCH_ASM_SRCS = \
arch/i386/boot/boot.s \
arch/i386/interrupt.s \
arch/i386/gdt.s


# LibC Files (source paths)
LIBC_SRCS = \
libc/stdio/itoa.c \
libc/stdio/printf.c \
libc/stdlib/malloc.c \
libc/stdlib/atoi.c \
libc/stdlib/rand.c \
libc/unistd/syscalls.c \
libc/netdb/resolve.c \
libc/string/memset.c \
libc/string/memcmp.c \
libc/string/strlen.c \
libc/string/strcpy.c \
libc/string/strcat.c \
libc/string/strchr.c \
libc/string/strspn.c \
libc/string/strpbrk.c

# Userspace-linkable libc (built separately from the kernel's own LIBC_OBJS
# above — reuses the same sources, since they're already thin int 0x80
# wrappers, except malloc.c which calls the kernel-only kmalloc()).
USER_CFLAGS = -O2 -g -ffreestanding -Wall -Wextra -fno-pie -fno-stack-protector
USER_CPPFLAGS = -Ikernel/include -Ilibc/include
LIBC_USER_BUILD_DIR = $(BUILD_DIR)/libc_user
LIBC_USER_SRCS = \
libc/stdio/itoa.c \
libc/stdio/printf.c \
libc/stdlib/atoi.c \
libc/stdlib/rand.c \
libc/stdlib/malloc_user.c \
libc/unistd/syscalls.c \
libc/netdb/resolve.c \
libc/string/memset.c \
libc/string/memcmp.c \
libc/string/strlen.c \
libc/string/strcpy.c \
libc/string/strcat.c \
libc/string/strchr.c \
libc/string/strspn.c \
libc/string/strpbrk.c
LIBC_USER_OBJS = $(patsubst libc/%.c,$(LIBC_USER_BUILD_DIR)/%.o,$(LIBC_USER_SRCS))
LIBC_USER_ARCHIVE = $(LIBC_USER_BUILD_DIR)/libc_user.a
AR = /home/adel/opt/cross/bin/i686-elf-ar

# Convert source paths to object paths in build directory
KERNEL_OBJS = $(patsubst kernel/%.c,$(KERNEL_BUILD_DIR)/%.o,$(filter kernel/%.c,$(KERNEL_SRCS)))
KERNEL_OBJS += build/kernel/context_switch.o
LIBC_OBJS = $(patsubst libc/%.c,$(LIBC_BUILD_DIR)/%.o,$(filter libc/%.c,$(LIBC_SRCS)))
ARCH_C_OBJS = $(patsubst arch/i386/%.c,$(ARCH_BUILD_DIR)/%.o,$(ARCH_C_SRCS))
ARCH_ASM_OBJS = $(patsubst arch/i386/%.s,$(ARCH_BUILD_DIR)/%.o,$(ARCH_ASM_SRCS))
USER_ELFS = $(USER_BUILD_DIR)/anim.elf $(USER_BUILD_DIR)/forktest.elf $(USER_BUILD_DIR)/execvetest.elf $(USER_BUILD_DIR)/waittest.elf $(USER_BUILD_DIR)/waitstress.elf $(USER_BUILD_DIR)/waitstressbg.elf $(USER_BUILD_DIR)/catfd.elf $(USER_BUILD_DIR)/sigtest.elf $(USER_BUILD_DIR)/mmaptest.elf $(USER_BUILD_DIR)/shmtest.elf $(USER_BUILD_DIR)/nxstacktest.elf $(USER_BUILD_DIR)/randtest.elf $(USER_BUILD_DIR)/argvtest.elf $(USER_BUILD_DIR)/syscalltest.elf $(USER_BUILD_DIR)/fstest.elf $(USER_BUILD_DIR)/dmesgtest.elf $(USER_BUILD_DIR)/blktest.elf $(USER_BUILD_DIR)/nettest.elf $(USER_BUILD_DIR)/ls.elf $(USER_BUILD_DIR)/cat.elf $(USER_BUILD_DIR)/touch.elf $(USER_BUILD_DIR)/mkdir.elf $(USER_BUILD_DIR)/write.elf $(USER_BUILD_DIR)/rm.elf $(USER_BUILD_DIR)/stat.elf $(USER_BUILD_DIR)/id.elf $(USER_BUILD_DIR)/pwd.elf $(USER_BUILD_DIR)/chmod.elf $(USER_BUILD_DIR)/chown.elf $(USER_BUILD_DIR)/echo.elf $(USER_BUILD_DIR)/uptime.elf $(USER_BUILD_DIR)/time.elf $(USER_BUILD_DIR)/tz.elf $(USER_BUILD_DIR)/sh.elf $(USER_BUILD_DIR)/tasks.elf $(USER_BUILD_DIR)/dmesg.elf $(USER_BUILD_DIR)/kill.elf $(USER_BUILD_DIR)/cls.elf $(USER_BUILD_DIR)/disk.elf $(USER_BUILD_DIR)/vm.elf $(USER_BUILD_DIR)/color.elf $(USER_BUILD_DIR)/net.elf $(USER_BUILD_DIR)/blk.elf
USER_BLOB_OBJS = $(USER_BUILD_DIR)/anim_blob.o $(USER_BUILD_DIR)/forktest_blob.o $(USER_BUILD_DIR)/execvetest_blob.o $(USER_BUILD_DIR)/waittest_blob.o $(USER_BUILD_DIR)/waitstress_blob.o $(USER_BUILD_DIR)/waitstressbg_blob.o $(USER_BUILD_DIR)/catfd_blob.o $(USER_BUILD_DIR)/sigtest_blob.o $(USER_BUILD_DIR)/mmaptest_blob.o $(USER_BUILD_DIR)/shmtest_blob.o $(USER_BUILD_DIR)/faulttest_blob.o $(USER_BUILD_DIR)/nxstacktest_blob.o $(USER_BUILD_DIR)/randtest_blob.o $(USER_BUILD_DIR)/argvtest_blob.o $(USER_BUILD_DIR)/syscalltest_blob.o $(USER_BUILD_DIR)/fstest_blob.o $(USER_BUILD_DIR)/dmesgtest_blob.o $(USER_BUILD_DIR)/blktest_blob.o $(USER_BUILD_DIR)/nettest_blob.o $(USER_BUILD_DIR)/ls_blob.o $(USER_BUILD_DIR)/cat_blob.o $(USER_BUILD_DIR)/touch_blob.o $(USER_BUILD_DIR)/mkdir_blob.o $(USER_BUILD_DIR)/write_blob.o $(USER_BUILD_DIR)/rm_blob.o $(USER_BUILD_DIR)/stat_blob.o $(USER_BUILD_DIR)/id_blob.o $(USER_BUILD_DIR)/pwd_blob.o $(USER_BUILD_DIR)/chmod_blob.o $(USER_BUILD_DIR)/chown_blob.o $(USER_BUILD_DIR)/echo_blob.o $(USER_BUILD_DIR)/uptime_blob.o $(USER_BUILD_DIR)/time_blob.o $(USER_BUILD_DIR)/tz_blob.o $(USER_BUILD_DIR)/sh_blob.o $(USER_BUILD_DIR)/tasks_blob.o $(USER_BUILD_DIR)/dmesg_blob.o $(USER_BUILD_DIR)/kill_blob.o $(USER_BUILD_DIR)/cls_blob.o $(USER_BUILD_DIR)/disk_blob.o $(USER_BUILD_DIR)/vm_blob.o $(USER_BUILD_DIR)/color_blob.o $(USER_BUILD_DIR)/net_blob.o $(USER_BUILD_DIR)/blk_blob.o

# All object files - BOOT.S MUST BE FIRST for multiboot header!
ALL_OBJS = $(ARCH_BUILD_DIR)/boot/boot.o $(ARCH_BUILD_DIR)/interrupt.o $(ARCH_BUILD_DIR)/gdt.o $(ARCH_C_OBJS) $(KERNEL_OBJS) $(LWIP_OBJS) $(LIBC_OBJS) $(USER_BLOB_OBJS)

.PHONY: all clean install directories iso run run-net run-net-rtl
.SUFFIXES: .o .c .s

all: deanos.bin

deanos.bin: directories $(ALL_OBJS) arch/i386/boot/linker.ld
	$(CC) -T arch/i386/boot/linker.ld -o $@ $(CFLAGS) $(ALL_OBJS) $(LDFLAGS) $(LIBS)
	@echo "Checking multiboot header..."
	@if grub-file --is-x86-multiboot2 deanos.bin; then \
		echo "✓ Multiboot2 header found!"; \
	else \
		echo "✗ WARNING: No multiboot header found!"; \
		echo "First 64 bytes of binary:"; \
		hexdump -C deanos.bin | head -4; \
	fi

# Create build directories
directories:
	@mkdir -p $(KERNEL_BUILD_DIR)
	@mkdir -p $(LIBC_BUILD_DIR)/stdio
	@mkdir -p $(LIBC_BUILD_DIR)/stdlib
	@mkdir -p $(LIBC_BUILD_DIR)/unistd
	@mkdir -p $(LIBC_BUILD_DIR)/netdb
	@mkdir -p $(LIBC_BUILD_DIR)/string
	@mkdir -p $(ARCH_BUILD_DIR)/boot
	@mkdir -p $(USER_BUILD_DIR)
	@mkdir -p $(LWIP_BUILD_DIR)
	@mkdir -p $(LIBC_USER_BUILD_DIR)/stdio
	@mkdir -p $(LIBC_USER_BUILD_DIR)/stdlib
	@mkdir -p $(LIBC_USER_BUILD_DIR)/unistd
	@mkdir -p $(LIBC_USER_BUILD_DIR)/netdb
	@mkdir -p $(LIBC_USER_BUILD_DIR)/string

# Compile lwIP core source files
$(LWIP_BUILD_DIR)/%.o: $(LWIP_DIR)/src/%.c | directories
	@mkdir -p $(dir $@)
	$(CC) -MD -c $< -o $@ $(CFLAGS) $(CPPFLAGS) -Wno-unused-parameter -Wno-address

# Compile lwIP port/glue files (kernel/lwip_port/*.c)
$(LWIP_BUILD_DIR)/port/%.o: kernel/lwip_port/%.c | directories
	@mkdir -p $(dir $@)
	$(CC) -MD -c $< -o $@ $(CFLAGS) $(CPPFLAGS) -Wno-unused-parameter

# Compile C files from kernel directory.
# Kernel C code is built with stack canaries; the freestanding runtime for them
# lives in kernel/stack_protector.c. (libc/lwip/arch stay unprotected.)
$(KERNEL_BUILD_DIR)/%.o: kernel/%.c | directories
	$(CC) -MD -c $< -o $@ $(CFLAGS) -fstack-protector-strong $(CPPFLAGS)

$(KERNEL_BUILD_DIR)/%.o: kernel/%.s | directories
	$(CC) -MD -c $< -o $@ $(CFLAGS) $(CPPFLAGS)

# Compile C files from libc directory
$(LIBC_BUILD_DIR)/%.o: libc/%.c | directories
	$(CC) -MD -c $< -o $@ $(CFLAGS) $(CPPFLAGS)

# Compile C files for the userspace-linkable libc, and archive it
$(LIBC_USER_BUILD_DIR)/%.o: libc/%.c | directories
	$(CC) -MD -c $< -o $@ $(USER_CFLAGS) $(USER_CPPFLAGS)

$(LIBC_USER_ARCHIVE): $(LIBC_USER_OBJS) | directories
	$(AR) rcs $@ $(LIBC_USER_OBJS)

$(USER_BUILD_DIR)/crt0.o: user/crt0.s | directories
	$(AS) $< -o $@

# Compile C files from arch directory
$(ARCH_BUILD_DIR)/%.o: arch/i386/%.c | directories
	$(CC) -MD -c $< -o $@ $(CFLAGS) $(CPPFLAGS)

# Assemble assembly files from arch directory
$(ARCH_BUILD_DIR)/%.o: arch/i386/%.s | directories
	$(AS) $< -o $@

$(USER_BUILD_DIR)/anim.o: user/anim.s | directories
	$(AS) $< -o $@

$(USER_BUILD_DIR)/forktest.o: user/forktest.s | directories
	$(AS) $< -o $@

$(USER_BUILD_DIR)/execvetest.o: user/execvetest.s | directories
	$(AS) $< -o $@

$(USER_BUILD_DIR)/waittest.o: user/waittest.s | directories
	$(AS) $< -o $@

$(USER_BUILD_DIR)/waitstress.o: user/waitstress.s | directories
	$(AS) $< -o $@

$(USER_BUILD_DIR)/waitstressbg.o: user/waitstressbg.s | directories
	$(AS) $< -o $@

$(USER_BUILD_DIR)/catfd.o: user/catfd.s | directories
	$(AS) $< -o $@

$(USER_BUILD_DIR)/sigtest.o: user/sigtest.s | directories
	$(AS) $< -o $@

$(USER_BUILD_DIR)/mmaptest.o: user/mmaptest.s | directories
	$(AS) $< -o $@

$(USER_BUILD_DIR)/shmtest.o: user/shmtest.s | directories
	$(AS) $< -o $@

$(USER_BUILD_DIR)/faulttest.o: user/faulttest.s | directories
	$(AS) $< -o $@

$(USER_BUILD_DIR)/nxstacktest.o: user/nxstacktest.s | directories
	$(AS) $< -o $@

$(USER_BUILD_DIR)/randtest.o: user/randtest.s | directories
	$(AS) $< -o $@

$(USER_BUILD_DIR)/anim.elf: $(USER_BUILD_DIR)/anim.o user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/anim.o -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/anim_blob.o: $(USER_BUILD_DIR)/anim.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/forktest.elf: $(USER_BUILD_DIR)/forktest.o user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/forktest.o -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/forktest_blob.o: $(USER_BUILD_DIR)/forktest.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/execvetest.elf: $(USER_BUILD_DIR)/execvetest.o user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/execvetest.o -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/execvetest_blob.o: $(USER_BUILD_DIR)/execvetest.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/waittest.elf: $(USER_BUILD_DIR)/waittest.o user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/waittest.o -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/waittest_blob.o: $(USER_BUILD_DIR)/waittest.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/waitstress.elf: $(USER_BUILD_DIR)/waitstress.o user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/waitstress.o -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/waitstress_blob.o: $(USER_BUILD_DIR)/waitstress.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/waitstressbg.elf: $(USER_BUILD_DIR)/waitstressbg.o user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/waitstressbg.o -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/waitstressbg_blob.o: $(USER_BUILD_DIR)/waitstressbg.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/catfd.elf: $(USER_BUILD_DIR)/catfd.o user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/catfd.o -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/catfd_blob.o: $(USER_BUILD_DIR)/catfd.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/sigtest.elf: $(USER_BUILD_DIR)/sigtest.o user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/sigtest.o -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/sigtest_blob.o: $(USER_BUILD_DIR)/sigtest.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/mmaptest.elf: $(USER_BUILD_DIR)/mmaptest.o user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/mmaptest.o -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/mmaptest_blob.o: $(USER_BUILD_DIR)/mmaptest.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/shmtest.elf: $(USER_BUILD_DIR)/shmtest.o user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/shmtest.o -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/shmtest_blob.o: $(USER_BUILD_DIR)/shmtest.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/faulttest.elf: $(USER_BUILD_DIR)/faulttest.o user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/faulttest.o -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/faulttest_blob.o: $(USER_BUILD_DIR)/faulttest.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/nxstacktest.elf: $(USER_BUILD_DIR)/nxstacktest.o user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/nxstacktest.o -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/nxstacktest_blob.o: $(USER_BUILD_DIR)/nxstacktest.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/randtest.elf: $(USER_BUILD_DIR)/randtest.o user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/randtest.o -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/randtest_blob.o: $(USER_BUILD_DIR)/randtest.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/argvtest.o: user/argvtest.c | directories
	$(CC) -c $< -o $@ $(USER_CFLAGS) $(USER_CPPFLAGS)

$(USER_BUILD_DIR)/argvtest.elf: $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/argvtest.o $(LIBC_USER_ARCHIVE) user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/argvtest.o $(LIBC_USER_ARCHIVE) -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/argvtest_blob.o: $(USER_BUILD_DIR)/argvtest.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/syscalltest.o: user/syscalltest.c | directories
	$(CC) -c $< -o $@ $(USER_CFLAGS) $(USER_CPPFLAGS)

$(USER_BUILD_DIR)/syscalltest.elf: $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/syscalltest.o $(LIBC_USER_ARCHIVE) user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/syscalltest.o $(LIBC_USER_ARCHIVE) -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/syscalltest_blob.o: $(USER_BUILD_DIR)/syscalltest.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/fstest.o: user/fstest.c | directories
	$(CC) -c $< -o $@ $(USER_CFLAGS) $(USER_CPPFLAGS)

$(USER_BUILD_DIR)/fstest.elf: $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/fstest.o $(LIBC_USER_ARCHIVE) user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/fstest.o $(LIBC_USER_ARCHIVE) -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/fstest_blob.o: $(USER_BUILD_DIR)/fstest.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/dmesgtest.o: user/dmesgtest.c | directories
	$(CC) -c $< -o $@ $(USER_CFLAGS) $(USER_CPPFLAGS)

$(USER_BUILD_DIR)/dmesgtest.elf: $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/dmesgtest.o $(LIBC_USER_ARCHIVE) user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/dmesgtest.o $(LIBC_USER_ARCHIVE) -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/dmesgtest_blob.o: $(USER_BUILD_DIR)/dmesgtest.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/blktest.o: user/blktest.c | directories
	$(CC) -c $< -o $@ $(USER_CFLAGS) $(USER_CPPFLAGS)

$(USER_BUILD_DIR)/blktest.elf: $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/blktest.o $(LIBC_USER_ARCHIVE) user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/blktest.o $(LIBC_USER_ARCHIVE) -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/blktest_blob.o: $(USER_BUILD_DIR)/blktest.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/nettest.o: user/nettest.c | directories
	$(CC) -c $< -o $@ $(USER_CFLAGS) $(USER_CPPFLAGS)

$(USER_BUILD_DIR)/nettest.elf: $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/nettest.o $(LIBC_USER_ARCHIVE) user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/nettest.o $(LIBC_USER_ARCHIVE) -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/nettest_blob.o: $(USER_BUILD_DIR)/nettest.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/ls.o: user/ls.c | directories
	$(CC) -c $< -o $@ $(USER_CFLAGS) $(USER_CPPFLAGS)

$(USER_BUILD_DIR)/ls.elf: $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/ls.o $(LIBC_USER_ARCHIVE) user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/ls.o $(LIBC_USER_ARCHIVE) -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/ls_blob.o: $(USER_BUILD_DIR)/ls.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/cat.o: user/cat.c | directories
	$(CC) -c $< -o $@ $(USER_CFLAGS) $(USER_CPPFLAGS)

$(USER_BUILD_DIR)/cat.elf: $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/cat.o $(LIBC_USER_ARCHIVE) user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/cat.o $(LIBC_USER_ARCHIVE) -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/cat_blob.o: $(USER_BUILD_DIR)/cat.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/touch.o: user/touch.c | directories
	$(CC) -c $< -o $@ $(USER_CFLAGS) $(USER_CPPFLAGS)

$(USER_BUILD_DIR)/touch.elf: $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/touch.o $(LIBC_USER_ARCHIVE) user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/touch.o $(LIBC_USER_ARCHIVE) -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/touch_blob.o: $(USER_BUILD_DIR)/touch.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/mkdir.o: user/mkdir.c | directories
	$(CC) -c $< -o $@ $(USER_CFLAGS) $(USER_CPPFLAGS)

$(USER_BUILD_DIR)/mkdir.elf: $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/mkdir.o $(LIBC_USER_ARCHIVE) user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/mkdir.o $(LIBC_USER_ARCHIVE) -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/mkdir_blob.o: $(USER_BUILD_DIR)/mkdir.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/write.o: user/write.c | directories
	$(CC) -c $< -o $@ $(USER_CFLAGS) $(USER_CPPFLAGS)

$(USER_BUILD_DIR)/write.elf: $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/write.o $(LIBC_USER_ARCHIVE) user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/write.o $(LIBC_USER_ARCHIVE) -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/write_blob.o: $(USER_BUILD_DIR)/write.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/rm.o: user/rm.c | directories
	$(CC) -c $< -o $@ $(USER_CFLAGS) $(USER_CPPFLAGS)

$(USER_BUILD_DIR)/rm.elf: $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/rm.o $(LIBC_USER_ARCHIVE) user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/rm.o $(LIBC_USER_ARCHIVE) -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/rm_blob.o: $(USER_BUILD_DIR)/rm.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/stat.o: user/stat.c | directories
	$(CC) -c $< -o $@ $(USER_CFLAGS) $(USER_CPPFLAGS)

$(USER_BUILD_DIR)/stat.elf: $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/stat.o $(LIBC_USER_ARCHIVE) user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/stat.o $(LIBC_USER_ARCHIVE) -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/stat_blob.o: $(USER_BUILD_DIR)/stat.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/id.o: user/id.c | directories
	$(CC) -c $< -o $@ $(USER_CFLAGS) $(USER_CPPFLAGS)

$(USER_BUILD_DIR)/id.elf: $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/id.o $(LIBC_USER_ARCHIVE) user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/id.o $(LIBC_USER_ARCHIVE) -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/id_blob.o: $(USER_BUILD_DIR)/id.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/pwd.o: user/pwd.c | directories
	$(CC) -c $< -o $@ $(USER_CFLAGS) $(USER_CPPFLAGS)

$(USER_BUILD_DIR)/pwd.elf: $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/pwd.o $(LIBC_USER_ARCHIVE) user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/pwd.o $(LIBC_USER_ARCHIVE) -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/pwd_blob.o: $(USER_BUILD_DIR)/pwd.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/chmod.o: user/chmod.c | directories
	$(CC) -c $< -o $@ $(USER_CFLAGS) $(USER_CPPFLAGS)

$(USER_BUILD_DIR)/chmod.elf: $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/chmod.o $(LIBC_USER_ARCHIVE) user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/chmod.o $(LIBC_USER_ARCHIVE) -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/chmod_blob.o: $(USER_BUILD_DIR)/chmod.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/chown.o: user/chown.c | directories
	$(CC) -c $< -o $@ $(USER_CFLAGS) $(USER_CPPFLAGS)

$(USER_BUILD_DIR)/chown.elf: $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/chown.o $(LIBC_USER_ARCHIVE) user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/chown.o $(LIBC_USER_ARCHIVE) -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/chown_blob.o: $(USER_BUILD_DIR)/chown.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/echo.o: user/echo.c | directories
	$(CC) -c $< -o $@ $(USER_CFLAGS) $(USER_CPPFLAGS)

$(USER_BUILD_DIR)/echo.elf: $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/echo.o $(LIBC_USER_ARCHIVE) user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/echo.o $(LIBC_USER_ARCHIVE) -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/echo_blob.o: $(USER_BUILD_DIR)/echo.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/uptime.o: user/uptime.c | directories
	$(CC) -c $< -o $@ $(USER_CFLAGS) $(USER_CPPFLAGS)

$(USER_BUILD_DIR)/uptime.elf: $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/uptime.o $(LIBC_USER_ARCHIVE) user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/uptime.o $(LIBC_USER_ARCHIVE) -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/uptime_blob.o: $(USER_BUILD_DIR)/uptime.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/time.o: user/time.c | directories
	$(CC) -c $< -o $@ $(USER_CFLAGS) $(USER_CPPFLAGS)

$(USER_BUILD_DIR)/time.elf: $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/time.o $(LIBC_USER_ARCHIVE) user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/time.o $(LIBC_USER_ARCHIVE) -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/time_blob.o: $(USER_BUILD_DIR)/time.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/tz.o: user/tz.c | directories
	$(CC) -c $< -o $@ $(USER_CFLAGS) $(USER_CPPFLAGS)

$(USER_BUILD_DIR)/tz.elf: $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/tz.o $(LIBC_USER_ARCHIVE) user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/tz.o $(LIBC_USER_ARCHIVE) -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/tz_blob.o: $(USER_BUILD_DIR)/tz.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/sh.o: user/sh.c | directories
	$(CC) -c $< -o $@ $(USER_CFLAGS) $(USER_CPPFLAGS)

$(USER_BUILD_DIR)/sh.elf: $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/sh.o $(LIBC_USER_ARCHIVE) user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/sh.o $(LIBC_USER_ARCHIVE) -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/sh_blob.o: $(USER_BUILD_DIR)/sh.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/tasks.o: user/tasks.c | directories
	$(CC) -c $< -o $@ $(USER_CFLAGS) $(USER_CPPFLAGS)

$(USER_BUILD_DIR)/tasks.elf: $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/tasks.o $(LIBC_USER_ARCHIVE) user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/tasks.o $(LIBC_USER_ARCHIVE) -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/tasks_blob.o: $(USER_BUILD_DIR)/tasks.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/dmesg.o: user/dmesg.c | directories
	$(CC) -c $< -o $@ $(USER_CFLAGS) $(USER_CPPFLAGS)

$(USER_BUILD_DIR)/dmesg.elf: $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/dmesg.o $(LIBC_USER_ARCHIVE) user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/dmesg.o $(LIBC_USER_ARCHIVE) -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/dmesg_blob.o: $(USER_BUILD_DIR)/dmesg.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/kill.o: user/kill.c | directories
	$(CC) -c $< -o $@ $(USER_CFLAGS) $(USER_CPPFLAGS)

$(USER_BUILD_DIR)/kill.elf: $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/kill.o $(LIBC_USER_ARCHIVE) user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/kill.o $(LIBC_USER_ARCHIVE) -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/kill_blob.o: $(USER_BUILD_DIR)/kill.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/cls.o: user/cls.c | directories
	$(CC) -c $< -o $@ $(USER_CFLAGS) $(USER_CPPFLAGS)

$(USER_BUILD_DIR)/cls.elf: $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/cls.o $(LIBC_USER_ARCHIVE) user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/cls.o $(LIBC_USER_ARCHIVE) -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/cls_blob.o: $(USER_BUILD_DIR)/cls.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/disk.o: user/disk.c | directories
	$(CC) -c $< -o $@ $(USER_CFLAGS) $(USER_CPPFLAGS)

$(USER_BUILD_DIR)/disk.elf: $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/disk.o $(LIBC_USER_ARCHIVE) user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/disk.o $(LIBC_USER_ARCHIVE) -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/disk_blob.o: $(USER_BUILD_DIR)/disk.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/blk.o: user/blk.c | directories
	$(CC) -c $< -o $@ $(USER_CFLAGS) $(USER_CPPFLAGS)

$(USER_BUILD_DIR)/blk.elf: $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/blk.o $(LIBC_USER_ARCHIVE) user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/blk.o $(LIBC_USER_ARCHIVE) -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/blk_blob.o: $(USER_BUILD_DIR)/blk.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/vm.o: user/vm.c | directories
	$(CC) -c $< -o $@ $(USER_CFLAGS) $(USER_CPPFLAGS)

$(USER_BUILD_DIR)/vm.elf: $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/vm.o $(LIBC_USER_ARCHIVE) user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/vm.o $(LIBC_USER_ARCHIVE) -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/vm_blob.o: $(USER_BUILD_DIR)/vm.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/color.o: user/color.c | directories
	$(CC) -c $< -o $@ $(USER_CFLAGS) $(USER_CPPFLAGS)

$(USER_BUILD_DIR)/color.elf: $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/color.o $(LIBC_USER_ARCHIVE) user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/color.o $(LIBC_USER_ARCHIVE) -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/color_blob.o: $(USER_BUILD_DIR)/color.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

$(USER_BUILD_DIR)/net.o: user/net.c | directories
	$(CC) -c $< -o $@ $(USER_CFLAGS) $(USER_CPPFLAGS)

$(USER_BUILD_DIR)/net.elf: $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/net.o $(LIBC_USER_ARCHIVE) user/linker.ld | directories
	$(CC) -T user/linker.ld -o $@ $(USER_BUILD_DIR)/crt0.o $(USER_BUILD_DIR)/net.o $(LIBC_USER_ARCHIVE) -ffreestanding -fno-pie -nostdlib -nostartfiles -Wl,-n

$(USER_BUILD_DIR)/net_blob.o: $(USER_BUILD_DIR)/net.elf | directories
	$(LD) -r -m elf_i386 -b binary $< -o $@

clean:
	rm -rf $(BUILD_DIR)
	rm -f deanos.bin
	rm -f $(ISO_NAME)
	rm -rf $(DESTDIR)
	rm -f $(ISO_PATH)

install: deanos.bin
	mkdir -p $(DESTDIR)/boot/grub
	cp deanos.bin $(DESTDIR)/boot/
	cp grub.cfg $(DESTDIR)/boot/grub/

iso: install
	mkdir -p $(ISO_DIR)
	rm -f $(ISO_PATH)
	grub-mkrescue -o $(ISO_PATH) $(DESTDIR)

run: iso
	qemu-system-i386 -cdrom $(ISO_PATH)

run-net: iso
	qemu-system-i386 -cdrom $(ISO_PATH) -netdev user,id=net0 -device e1000,netdev=net0

run-net-rtl: iso
	qemu-system-i386 -cdrom $(ISO_PATH) -netdev user,id=net0 -device rtl8139,netdev=net0

# Include dependency files
-include $(ALL_OBJS:.o=.d)