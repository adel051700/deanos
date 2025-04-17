#!/bin/bash

# Set compiler and build tools
COMPILER="i686-elf-gcc"
ASSEMBLER="i686-elf-as"

# Set directory names
ISO_DIR="isodir"
BOOT_DIR="$ISO_DIR/boot"
GRUB_DIR="$BOOT_DIR/grub"

# Create directories if they don't exist
mkdir -p $BOOT_DIR
mkdir -p $GRUB_DIR

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${YELLOW}Building DeanOS...${NC}"

# Compile assembly files
echo -e "${GREEN}Assembling boot.s...${NC}"
$ASSEMBLER boot.s -o boot.o
if [ $? -ne 0 ]; then
    echo -e "${RED}Failed to assemble boot.s${NC}"
    exit 1
fi

# Compile C files
echo -e "${GREEN}Compiling kernel.c...${NC}"
$COMPILER -c kernel.c -o kernel.o -std=gnu99 -ffreestanding -O2 -Wall -Wextra
if [ $? -ne 0 ]; then
    echo -e "${RED}Failed to compile kernel.c${NC}"
    exit 1
fi

echo -e "${GREEN}Compiling crti.c...${NC}"
$COMPILER -c crti.c -o crti.o -std=gnu99 -ffreestanding -O2 -Wall -Wextra
if [ $? -ne 0 ]; then
    echo -e "${RED}Failed to compile crti.c${NC}"
    exit 1
fi

# Link the kernel
echo -e "${GREEN}Linking kernel...${NC}"
$COMPILER -T linker.ld -o deanos.bin -ffreestanding -O2 -nostdlib boot.o crti.o kernel.o -lgcc
if [ $? -ne 0 ]; then
    echo -e "${RED}Failed to link kernel${NC}"
    exit 1
fi

# Copy files to ISO directory
echo -e "${GREEN}Copying files to ISO directory...${NC}"
cp deanos.bin $BOOT_DIR/
cp grub.cfg $GRUB_DIR/

# Create ISO image
echo -e "${GREEN}Creating ISO image...${NC}"
grub-mkrescue -o deanos.iso $ISO_DIR
if [ $? -ne 0 ]; then
    echo -e "${RED}Failed to create ISO image${NC}"
    exit 1
fi

echo -e "${GREEN}Build completed successfully!${NC}"