#!/bin/bash

LOGFILE="captured_output.txt"
BAUD_RATE=115200

# Allow passing device as argument
if [ -n "$1" ]; then
    DEVICE="$1"
else
    # Auto-detect Pico USB serial devices
    echo "=== Detecting Pico Serial Device ==="
    echo ""

    # Find all USB serial devices
    if [[ "$OSTYPE" == "darwin"* ]]; then
        # macOS
        DEVICES=$(ls -1 /dev/tty.usbmodem* 2>/dev/null || true)
    else
        # Linux
        DEVICES=$(ls -1 /dev/ttyACM* /dev/ttyUSB* 2>/dev/null || true)
    fi

    if [ -z "$DEVICES" ]; then
        echo "ERROR: No USB serial devices found!"
        echo ""
        echo "Make sure:"
        echo "  1. Pico is connected via USB"
        echo "  2. It's running firmware with USB serial enabled"
        echo ""
        echo "You can manually specify the device:"
        echo "  $0 /dev/ttyACM0"
        exit 1
    fi

    # Count devices
    DEVICE_COUNT=$(echo "$DEVICES" | wc -l)

    if [ "$DEVICE_COUNT" -eq 1 ]; then
        DEVICE="$DEVICES"
        echo "Found device: $DEVICE"
    else
        echo "Found multiple devices:"
        echo "$DEVICES" | nl
        echo ""
        echo "Using first device: $(echo "$DEVICES" | head -1)"
        DEVICE=$(echo "$DEVICES" | head -1)
    fi
fi

echo ""
echo "=== Frame Capture with Screen Logging ==="
echo "Device: $DEVICE"
echo "Baud rate: $BAUD_RATE"
echo "Output: $LOGFILE"
echo ""

# Verify device exists
if [ ! -e "$DEVICE" ]; then
    echo "ERROR: Device not found: $DEVICE"
    exit 1
fi

# Remove old capture files
rm -f "$LOGFILE" screenlog.0

echo "Instructions:"
echo "  1. Screen will start with logging enabled"
echo "  2. Wait until you see '=== Frame output complete ==='"
echo "  3. Press Ctrl-A then K to quit screen"
echo "  4. Press Y to confirm"
echo ""
echo "Starting ..."

echo ""

# Start screen with logging enabled
# -L turns on logging (creates screenlog.0)
screen -L "$DEVICE" "$BAUD_RATE"

# After screen exits, move the log file and extract the PBM
echo ""
if [ -f screenlog.0 ]; then
    mv screenlog.0 "$LOGFILE"
    echo "Screen closed. Output saved to: $LOGFILE"
    echo ""

    # Check if extract_pbm.sh exists and run it
    EXTRACT_SCRIPT="$(dirname "$0")/extract_pbm.sh"
    if [ -f "$EXTRACT_SCRIPT" ]; then
        echo "Extracting PBM..."
        "$EXTRACT_SCRIPT"
    else
        echo "Note: extract_pbm.sh not found, skipping image extraction"
        echo "You can manually process $LOGFILE if needed"
    fi
else
    echo "ERROR: No screenlog.0 file created"
    echo "This may mean screen didn't start properly or was terminated"
    exit 1
fi
