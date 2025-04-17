#!/bin/bash

# Set QEMU options
QEMU="qemu-system-i386"
ISO="deanos.iso"

# Colors for output
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}=== Building DeanOS ===${NC}"
# First run the build script
./build.sh

# Check if build was successful
if [ $? -ne 0 ]; then
    echo -e "${RED}Build failed, not running QEMU${NC}"
    exit 1
fi

# Check if ISO exists
if [ ! -f "$ISO" ]; then
    echo -e "${RED}Error: $ISO not found even though build reported success${NC}"
    exit 1
fi

# Additional QEMU options
MEMORY=128M
BOOT_ORDER=d # Boot from CD-ROM
ADDITIONAL_OPTIONS="-monitor stdio" # Monitor via stdio

echo -e "${BLUE}=== Running DeanOS in QEMU ===${NC}"
echo -e "${GREEN}Press Ctrl+Alt+G to release mouse focus${NC}"
echo -e "${GREEN}Type 'quit' in the QEMU monitor to exit${NC}"

# Run QEMU with the ISO
$QEMU -cdrom "$ISO" -m "$MEMORY" -boot "$BOOT_ORDER" $ADDITIONAL_OPTIONS

# Check if QEMU exited successfully
if [ $? -ne 0 ]; then
    echo -e "${RED}QEMU exited with an error${NC}"
    exit 1
fi

echo -e "${GREEN}QEMU session ended${NC}"