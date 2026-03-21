# DeanOS Makefile

# Output directories
BUILD_DIR = build
KERNEL_BUILD_DIR = $(BUILD_DIR)/kernel
LIBC_BUILD_DIR = $(BUILD_DIR)/libc
ARCH_BUILD_DIR = $(BUILD_DIR)/arch/i386
USER_BUILD_DIR = $(BUILD_DIR)/user
DESTDIR = isodir
DATE = $(shell date +%d-%m-%Y)
MAJOR = 0
MINOR = 6
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
CPPFLAGS:=$(CPPFLAGS) -Ikernel/include -Ilibc/include
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
kernel/elf.c \
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
libc/unistd/syscalls.c \
libc/string/memset.c \
libc/string/memcmp.c \
libc/string/strlen.c \
libc/string/strcpy.c \
libc/string/strcat.c \
libc/string/strchr.c \
libc/string/strspn.c \
libc/string/strpbrk.c

# Convert source paths to object paths in build directory
KERNEL_OBJS = $(patsubst kernel/%.c,$(KERNEL_BUILD_DIR)/%.o,$(filter kernel/%.c,$(KERNEL_SRCS)))
KERNEL_OBJS += build/kernel/context_switch.o
LIBC_OBJS = $(patsubst libc/%.c,$(LIBC_BUILD_DIR)/%.o,$(filter libc/%.c,$(LIBC_SRCS)))
ARCH_C_OBJS = $(patsubst arch/i386/%.c,$(ARCH_BUILD_DIR)/%.o,$(ARCH_C_SRCS))
ARCH_ASM_OBJS = $(patsubst arch/i386/%.s,$(ARCH_BUILD_DIR)/%.o,$(ARCH_ASM_SRCS))
USER_ELFS = $(USER_BUILD_DIR)/anim.elf $(USER_BUILD_DIR)/forktest.elf $(USER_BUILD_DIR)/execvetest.elf $(USER_BUILD_DIR)/waittest.elf $(USER_BUILD_DIR)/waitstress.elf $(USER_BUILD_DIR)/waitstressbg.elf $(USER_BUILD_DIR)/catfd.elf $(USER_BUILD_DIR)/sigtest.elf $(USER_BUILD_DIR)/mmaptest.elf $(USER_BUILD_DIR)/shmtest.elf
USER_BLOB_OBJS = $(USER_BUILD_DIR)/anim_blob.o $(USER_BUILD_DIR)/forktest_blob.o $(USER_BUILD_DIR)/execvetest_blob.o $(USER_BUILD_DIR)/waittest_blob.o $(USER_BUILD_DIR)/waitstress_blob.o $(USER_BUILD_DIR)/waitstressbg_blob.o $(USER_BUILD_DIR)/catfd_blob.o $(USER_BUILD_DIR)/sigtest_blob.o $(USER_BUILD_DIR)/mmaptest_blob.o $(USER_BUILD_DIR)/shmtest_blob.o

# All object files - BOOT.S MUST BE FIRST for multiboot header!
ALL_OBJS = $(ARCH_BUILD_DIR)/boot/boot.o $(ARCH_BUILD_DIR)/interrupt.o $(ARCH_BUILD_DIR)/gdt.o $(ARCH_C_OBJS) $(KERNEL_OBJS) $(LIBC_OBJS) $(USER_BLOB_OBJS)

.PHONY: all clean install directories iso run
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
	@mkdir -p $(LIBC_BUILD_DIR)/string
	@mkdir -p $(ARCH_BUILD_DIR)/boot
	@mkdir -p $(USER_BUILD_DIR)

# Compile C files from kernel directory
$(KERNEL_BUILD_DIR)/%.o: kernel/%.c | directories
	$(CC) -MD -c $< -o $@ $(CFLAGS) $(CPPFLAGS)

$(KERNEL_BUILD_DIR)/%.o: kernel/%.s | directories
	$(CC) -MD -c $< -o $@ $(CFLAGS) $(CPPFLAGS)

# Compile C files from libc directory
$(LIBC_BUILD_DIR)/%.o: libc/%.c | directories
	$(CC) -MD -c $< -o $@ $(CFLAGS) $(CPPFLAGS)

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

# Include dependency files
-include $(ALL_OBJS:.o=.d)