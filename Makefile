# DeanOS Makefile

# Output directories
BUILD_DIR = build
KERNEL_BUILD_DIR = $(BUILD_DIR)/kernel
LIBC_BUILD_DIR = $(BUILD_DIR)/libc
ARCH_BUILD_DIR = $(BUILD_DIR)/arch/i386
DESTDIR = isodir

# Cross-compiler settings
CFLAGS?=-O2 -g
CPPFLAGS?=
LDFLAGS?=
LIBS?=

# Compiler/Assembler/Linker
CC = i686-elf-gcc
AS = i686-elf-as
LD = i686-elf-ld

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
kernel/shell.c \
kernel/rtc.c \
kernel/pit.c \
kernel/pmm.c \
kernel/paging.c \
kernel/kheap.c \
kernel/pic.c \
kernel/irq.c \
kernel/log.c \
kernel/syscall.c

ARCH_C_SRCS = \
arch/i386/boot/crti.c

ARCH_ASM_SRCS = \
arch/i386/boot/boot.s \
arch/i386/interrupt.s \
arch/i386/gdt.s

# LibC Files (source paths)
LIBC_SRCS = \
libc/stdio/itoa.c \
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
LIBC_OBJS = $(patsubst libc/%.c,$(LIBC_BUILD_DIR)/%.o,$(filter libc/%.c,$(LIBC_SRCS)))
ARCH_C_OBJS = $(patsubst arch/i386/%.c,$(ARCH_BUILD_DIR)/%.o,$(ARCH_C_SRCS))
ARCH_ASM_OBJS = $(patsubst arch/i386/%.s,$(ARCH_BUILD_DIR)/%.o,$(ARCH_ASM_SRCS))

# All object files - BOOT.S MUST BE FIRST for multiboot header!
ALL_OBJS = $(ARCH_BUILD_DIR)/boot/boot.o $(ARCH_BUILD_DIR)/interrupt.o $(ARCH_BUILD_DIR)/gdt.o $(ARCH_C_OBJS) $(KERNEL_OBJS) $(LIBC_OBJS)

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
	@mkdir -p $(LIBC_BUILD_DIR)/string
	@mkdir -p $(ARCH_BUILD_DIR)/boot

# Compile C files from kernel directory
$(KERNEL_BUILD_DIR)/%.o: kernel/%.c | directories
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


clean:
	rm -rf $(BUILD_DIR)
	rm -f deanos.bin
	rm -f deanos.iso
	rm -rf $(DESTDIR)

install: deanos.bin
	mkdir -p $(DESTDIR)/boot/grub
	cp deanos.bin $(DESTDIR)/boot/
	cp grub.cfg $(DESTDIR)/boot/grub/

iso: install
	grub-mkrescue -o deanos.iso $(DESTDIR)

run: iso
	make clean
	make 
	make install
	make iso
	qemu-system-i386 -cdrom deanos.iso

# Include dependency files
-include $(ALL_OBJS:.o=.d)