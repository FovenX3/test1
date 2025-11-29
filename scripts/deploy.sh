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

# Auto-detect Pico mount point
echo "=== DETECTING PICO ==="
PICO_MOUNT=""
PICO_TYPE=""

# Check for Pico 2 (RP2350) first
if [ -d "/Volumes/RP2350" ]; then
    PICO_MOUNT="/Volumes/RP2350"
    PICO_TYPE="Pico 2 (RP2350)"
elif [ -d "/media/$(whoami)/RP2350" ]; then
    PICO_MOUNT="/media/$(whoami)/RP2350"
    PICO_TYPE="Pico 2 (RP2350)"
elif [ -d "/mnt/d/RP2350" ]; then
    PICO_MOUNT="/mnt/d/RP2350"
    PICO_TYPE="Pico 2 (RP2350)"
# Check for Pico 1 (RP2040) - backwards compatibility
elif [ -d "/Volumes/RPI-RP2" ]; then
    PICO_MOUNT="/Volumes/RPI-RP2"
    PICO_TYPE="Pico 1 (RP2040)"
elif [ -d "/media/$(whoami)/RPI-RP2" ]; then
    PICO_MOUNT="/media/$(whoami)/RPI-RP2"
    PICO_TYPE="Pico 1 (RP2040)"
elif [ -d "/mnt/d/RPI-RP2" ]; then
    PICO_MOUNT="/mnt/d/RPI-RP2"
    PICO_TYPE="Pico 1 (RP2040)"
fi

if [ -n "$PICO_MOUNT" ]; then
    echo "✓ Found $PICO_TYPE at: $PICO_MOUNT"
fi

echo ""

if [ -z "$PICO_MOUNT" ]; then
    echo "⚠ Pico not detected!"
    echo ""
    echo "To deploy manually:"
    echo "1. Put Pico in BOOTSEL mode (hold BOOTSEL, connect USB)"
    echo "2. Pico 2 will mount as: RP2350"
    echo "   Pico 1 will mount as: RPI-RP2"
    echo "3. Copy: cp $UF2_FILE /Volumes/RP2350/ (or /Volumes/RPI-RP2/)"
    exit 0
fi

# Deploy
echo "=== DEPLOYING ==="
cp "$UF2_FILE" "$PICO_MOUNT/" || {
    echo "✗ Deployment failed!"
    exit 1
}

sync

echo "✓ UF2 deployed successfully to $PICO_TYPE"
echo "✓ Pico will reboot automatically"
echo ""
echo "=== DONE ==="
echo ""
echo "Start the viewer with:"
echo "  python3 viewer/main.py"
echo ""
echo "Or monitor USB CDC with:"
echo "  screen /dev/tty.usbmodem* 115200"
echo ""
