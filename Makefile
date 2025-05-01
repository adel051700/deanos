# DeanOS Makefile

# Output directories
BUILD_DIR = build
KERNEL_BUILD_DIR = $(BUILD_DIR)/kernel
LIBC_BUILD_DIR = $(BUILD_DIR)/libc
ARCH_BUILD_DIR = $(BUILD_DIR)/arch/i386

# Cross-compiler settings
CFLAGS?=-O2 -g
CPPFLAGS?=
LDFLAGS?=
LIBS?=

# Compiler/Assembler/Linker
CC = i686-elf-gcc
AS = i686-elf-as

# Includes
CFLAGS:=$(CFLAGS) -ffreestanding -Wall -Wextra
CPPFLAGS:=$(CPPFLAGS) -Ikernel/include -Ilibc/include
LDFLAGS:=$(LDFLAGS)
LIBS:=$(LIBS) -nostdlib -lgcc

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
kernel/shell.c

ARCH_SRCS = \
arch/i386/boot/crti.s \
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
KERNEL_OBJS = $(patsubst kernel/%.c,$(KERNEL_BUILD_DIR)/%.o,$(KERNEL_SRCS))
LIBC_OBJS = $(patsubst libc/%.c,$(LIBC_BUILD_DIR)/%.o,$(LIBC_SRCS))
ARCH_OBJS = $(patsubst arch/i386/%.s,$(ARCH_BUILD_DIR)/%.o,$(filter %.s,$(ARCH_SRCS))) \
            $(patsubst arch/i386/%.c,$(ARCH_BUILD_DIR)/%.o,$(filter %.c,$(ARCH_SRCS)))

# All object files
ALL_OBJS = $(KERNEL_OBJS) $(LIBC_OBJS) $(ARCH_OBJS)

.PHONY: all clean install directories
.SUFFIXES: .o .c .s .a

all: directories deanos.bin

# Create build directories
directories:
	@mkdir -p $(KERNEL_BUILD_DIR)
	@mkdir -p $(LIBC_BUILD_DIR)/stdio
	@mkdir -p $(LIBC_BUILD_DIR)/string
	@mkdir -p $(ARCH_BUILD_DIR)/boot

deanos.bin: $(ALL_OBJS) arch/i386/boot/linker.ld
	$(CC) -T arch/i386/boot/linker.ld -o $@ $(CFLAGS) $(ALL_OBJS) $(LDFLAGS) $(LIBS)

# Compile C files from kernel directory
$(KERNEL_BUILD_DIR)/%.o: kernel/%.c
	@mkdir -p $(dir $@)
	$(CC) -MD -c $< -o $@ $(CFLAGS) $(CPPFLAGS)

# Compile C files from libc directory
$(LIBC_BUILD_DIR)/%.o: libc/%.c
	@mkdir -p $(dir $@)
	$(CC) -MD -c $< -o $@ $(CFLAGS) $(CPPFLAGS)

# Compile C files from arch directory
$(ARCH_BUILD_DIR)/%.o: arch/i386/%.c
	@mkdir -p $(dir $@)
	$(CC) -MD -c $< -o $@ $(CFLAGS) $(CPPFLAGS)

# Assemble assembly files from arch directory
$(ARCH_BUILD_DIR)/%.o: arch/i386/%.s
	@mkdir -p $(dir $@)
	$(AS) $< -o $@

clean:
	rm -rf $(BUILD_DIR)
	rm -f deanos.bin
	rm -f deanos.iso
	rm -rf isodir

install: deanos.bin
	mkdir -p $(DESTDIR)/boot
	cp $< $(DESTDIR)/boot

# Include dependency files
-include $(ALL_OBJS:.o=.d)