# DeanOS Makefile

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

# Kernel Files
KERNEL_OBJS=\
kernel/kernel.o \
kernel/kernel_init.o \
kernel/framebuffer.o \
kernel/tty.o \
kernel/font8x16.o \
arch/i386/boot/crti.o \
arch/i386/boot/boot.o

# LibC Files
LIBC_OBJS=\
libc/string/memset.o \
libc/string/memcmp.o \
libc/string/strlen.o \
libc/string/strcpy.o \
libc/string/strcat.o \
libc/string/strchr.o \
libc/string/strspn.o \
libc/string/strpbrk.o

.PHONY: all clean install
.SUFFIXES: .o .c .s .a

all: deanos.bin

deanos.bin: $(KERNEL_OBJS) $(LIBC_OBJS) arch/i386/boot/linker.ld
	$(CC) -T arch/i386/boot/linker.ld -o $@ $(CFLAGS) $(KERNEL_OBJS) $(LIBC_OBJS) $(LDFLAGS) $(LIBS)

.c.o:
	$(CC) -MD -c $< -o $@ $(CFLAGS) $(CPPFLAGS)

.s.o:
	$(AS) $< -o $@

clean:
	rm -f deanos.bin
	rm -f $(KERNEL_OBJS) $(LIBC_OBJS) *.o */*.o */*/*.o
	rm -f $(KERNEL_OBJS:.o=.d) $(LIBC_OBJS:.o=.d) *.d */*.d */*/*.d

install: deanos.bin
	mkdir -p $(DESTDIR)/boot
	cp $< $(DESTDIR)/boot

-include $(KERNEL_OBJS:.o=.d)
-include $(LIBC_OBJS:.o=.d)