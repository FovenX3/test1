#!/bin/bash
# Automated audio test script for YM2610 capture
# Usage: ./scripts/test_audio.sh [--rebuild] [--flash]

set -e
cd "$(dirname "$0")/.."

REBUILD=false
FLASH=false

# Parse args
for arg in "$@"; do
    case $arg in
        --rebuild) REBUILD=true ;;
        --flash) FLASH=true ;;
        *) echo "Unknown arg: $arg"; exit 1 ;;
    esac
done

# Rebuild if requested
if [ "$REBUILD" = true ]; then
    echo "=== Building firmware ==="
    cd build
    make audio_capture
    cd ..
fi

# Flash if requested
if [ "$FLASH" = true ]; then
    echo "=== Flashing firmware ==="
    picotool load -f build/src/audio_capture.uf2
    echo "Waiting for device to boot..."
    sleep 5
fi

# Always reset Pico to ensure clean state
echo "=== Resetting Pico ==="
picotool reboot -f 2>/dev/null || true
sleep 2

# Run audio test
echo "=== Running audio test ==="
cd viewer
source venv/bin/activate
python3 test_audio.py

# Find and display latest results
LATEST_WAV=$(ls -t test_*.wav 2>/dev/null | head -1)
LATEST_PNG=$(ls -t test_*.png 2>/dev/null | head -1)

echo ""
echo "=== Results ==="
echo "WAV: $LATEST_WAV"
echo "PNG: $LATEST_PNG"
echo ""
echo "To view plot: open $LATEST_PNG"
