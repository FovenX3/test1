#!/bin/bash

set -e

echo "=== NeoPico HD Deployment Script ==="
echo ""

# Build the project
echo "Building project..."
if [ ! -d "build" ]; then
    mkdir build
    cd build
    cmake ..
    cd ..
fi

cd build
make -j$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
cd ..

echo "Build complete!"
echo ""

# Check if UF2 file exists
UF2_FILE="build/src/neopico_hd.uf2"
if [ ! -f "$UF2_FILE" ]; then
    echo "Error: UF2 file not found at $UF2_FILE"
    exit 1
fi

echo "UF2 file ready: $(ls -lh $UF2_FILE | awk '{print $9, "(" $5 ")"}')"
echo ""

# Instructions for deployment
echo "=== DEPLOYMENT INSTRUCTIONS ==="
echo ""
echo "1. Put your Pico in BOOTSEL mode:"
echo "   - Unplug the Pico from USB"
echo "   - Hold the BOOTSEL button"
echo "   - While holding BOOTSEL, plug it back in"
echo ""
echo "2. Find where the Pico mounted:"
echo "   - On macOS: ls /Volumes/ | grep -i rpi"
echo "   - On Linux: lsblk | grep -i pico"
echo "   - On Windows: Check Device Manager"
echo ""
echo "3. Copy the UF2 file to the Pico mount point:"
echo "   cp $UF2_FILE /path/to/mounted/RPI-RP2"
echo ""
echo "4. The Pico will automatically reboot and start running"
echo ""
echo "5. Monitor serial output with screen:"
echo "   screen /dev/tty.usbmodem* 115200"
echo "   (Replace * with the correct number, or type 'ls /dev/tty.usbmodem' to find it)"
echo ""
echo "   To exit screen: Press Ctrl-A then Ctrl-D (or Ctrl-A then Shift-K)"
echo ""
