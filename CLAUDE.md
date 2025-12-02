# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

NeoPico-HD captures 15-bit RGB video from Neo Geo MVS arcade hardware and outputs 240p HDMI at 60fps using a Raspberry Pi Pico 2 (RP2350). The project is production-ready.

## Build Commands

```bash
# Build (requires PICO_SDK_PATH environment variable)
./scripts/build.sh

# Or manually:
mkdir build && cd build && cmake .. && make neopico_hd

# Build specific targets
make neopico_hd  # Main app (MVS capture + DVI output)
make neopico_usb # USB streaming variant (for PC viewer)
make dvi_test    # DVI color bar test
make gpio_test   # GPIO wiring test

# Deploy to Pico (auto-detects BOOTSEL mode)
./scripts/deploy.sh

# Or flash directly
picotool load -f build/src/main_dvi.uf2
```

## Architecture

**Dual-core design with shared framebuffer:**

- **Core 0**: MVS video capture - waits for VSYNC, reads pixels from PIO FIFO, converts RGB555→RGB565, writes to framebuffer
- **Core 1**: DVI output - runs scanline callback at 60Hz, reads from shared framebuffer via DMA

Key files:
- `src/main.c` - Main application (DVI output), dual-core setup, capture loop
- `src/main_usb.c` - USB streaming variant (streams to PC viewer)
- `src/mvs_capture.pio` - PIO state machines for sync detection and pixel capture
- `lib/PicoDVI/` - DVI output library (git submodule)

## Hardware Configuration

```
GPIO 0-4:   MVS R0-R4 (Red)      GPIO 16-21: DVI data pairs
GPIO 5-9:   MVS B0-B4 (Blue)     GPIO 22:    MVS CSYNC
GPIO 10-14: MVS G0-G4 (Green)    GPIO 26-27: DVI clock pair
GPIO 15:    GND (16-bit align)   GPIO 28:    MVS PCLK (6 MHz)
```

## Critical Constraints

1. **PIO timing is sensitive** - `mvs_capture.pio` contains timing-critical code; changes require hardware testing
2. **240p timing is fixed** - DVI runs at 640×240 @ 60Hz with 126MHz pixel clock
3. **Memory budget** - ~150KB framebuffer on 520KB SRAM; avoid large buffers
4. **No frame buffering** - Line-by-line capture for <1 frame latency
5. **Core independence** - Capture (Core 0) and output (Core 1) must not block each other

## Debugging

```bash
# USB serial monitor
screen /dev/tty.usbmodem* 115200

# Python frame viewer (requires pygame, pyserial)
cd viewer && python3 main.py
```

## Technical Documentation

Detailed timing specs and implementation notes are in `docs/`:
- `MVS_CAPTURE.md` - MVS signal specifications
- `MVS_DIGITAL_VIDEO.md` - Timing analysis
- `PROJECT_STATUS.md` - Architecture overview and milestones
