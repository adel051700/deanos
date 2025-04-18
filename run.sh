#!/bin/bash

# Set QEMU options
QEMU="qemu-system-i386"
BIN="deanos.iso"

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Set directory names
ISO_DIR="isodir"
BOOT_DIR="$ISO_DIR/boot"
GRUB_DIR="$BOOT_DIR/grub"

# Create directories if they don't exist
mkdir -p $ISO_DIR
mkdir -p $BOOT_DIR
mkdir -p $GRUB_DIR

cp deanos.bin isodir/boot/
cp grub.cfg isodir/boot/grub/
grub-mkrescue -o deanos.iso isodir

# Check if BIN exists
if [ ! -f "$BIN" ]; then
    echo -e "${RED}Error: $BIN not found even though build reported success${NC}"
    exit 1
fi

# Additional QEMU options
MEMORY=128M
BOOT_ORDER=d # Boot from CD-ROM
ADDITIONAL_OPTIONS="-monitor stdio" # Monitor via stdio

echo -e "${BLUE}=== Running DeanOS in QEMU ===${NC}"
echo -e "${GREEN}Press Ctrl+Alt+G to release mouse focus${NC}"
echo -e "${GREEN}Type 'quit' in the QEMU monitor to exit${NC}"

# Run QEMU with the BIN
$QEMU -cdrom "$BIN"

# Check if QEMU exited successfully
if [ $? -ne 0 ]; then
    echo -e "${RED}QEMU exited with an error${NC}"
    exit 1
fi

echo -e "${GREEN}QEMU session ended${NC}"