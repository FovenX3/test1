#!/usr/bin/env python3
"""
MVS Capture Viewer
Receives packed 3-bit RGB frames from Pico over USB and displays them.

Usage:
    python mvs_viewer.py [--port /dev/ttyACM0] [--scale 2]
"""

import argparse
import sys
import time
import serial
import serial.tools.list_ports
import pygame

# Frame constants (must match Pico)
FRAME_WIDTH = 320
FRAME_HEIGHT = 224
FRAME_SIZE_BYTES = FRAME_WIDTH * FRAME_HEIGHT * 2  # 2 bytes per pixel (RGB565)

# Sync header
SYNC_HEADER = bytes([0x55, 0xAA, 0xAA, 0x55])  # Little endian: 0xAA55, 0x55AA


def find_pico_port():
    """Auto-detect Pico CDC port."""
    ports = serial.tools.list_ports.comports()
    for port in ports:
        # Look for Pico or CDC device
        if 'ACM' in port.device or 'usbmodem' in port.device:
            return port.device
        if port.vid == 0xCAFE and port.pid == 0x4001:
            return port.device
    return None


def sync_to_frame(ser):
    """Find frame sync header in stream."""
    buffer = bytearray(4)
    sync_bytes = 0
    while sync_bytes < 1000000:  # Give up after ~1MB of searching
        byte = ser.read(1)
        if not byte:
            continue
        buffer.pop(0)
        buffer.append(byte[0])
        sync_bytes += 1
        if bytes(buffer) == SYNC_HEADER:
            return True
    return False


def read_frame(ser):
    """Read a complete frame, return None if sync lost."""
    frame_data = ser.read(FRAME_SIZE_BYTES)
    if len(frame_data) != FRAME_SIZE_BYTES:
        # Short read - need to resync
        return None
    return frame_data


def unpack_frame(frame_data):
    """Unpack RGB565 pixels to RGB tuples."""
    pixels = []
    for i in range(0, len(frame_data), 2):
        # RGB565: RRRRRGGG GGGBBBBB (little-endian)
        lo = frame_data[i]
        hi = frame_data[i + 1]
        rgb565 = lo | (hi << 8)

        # Extract components
        r5 = (rgb565 >> 11) & 0x1F
        g6 = (rgb565 >> 5) & 0x3F
        b5 = rgb565 & 0x1F

        # Expand to 8-bit
        r8 = (r5 << 3) | (r5 >> 2)
        g8 = (g6 << 2) | (g6 >> 4)
        b8 = (b5 << 3) | (b5 >> 2)

        pixels.append((r8, g8, b8))
    return pixels


def main():
    parser = argparse.ArgumentParser(description='MVS Capture Viewer')
    parser.add_argument('--port', '-p', help='Serial port (auto-detect if not specified)')
    parser.add_argument('--scale', '-s', type=int, default=2, help='Display scale (default: 2)')
    parser.add_argument('--baud', '-b', type=int, default=115200, help='Baud rate (default: 115200)')
    args = parser.parse_args()

    # Find port
    port = args.port or find_pico_port()
    if not port:
        print("Error: Could not find Pico. Specify port with --port")
        print("Available ports:")
        for p in serial.tools.list_ports.comports():
            print(f"  {p.device}: {p.description}")
        sys.exit(1)

    print(f"Connecting to {port}...")

    # Open serial
    try:
        ser = serial.Serial(port, args.baud, timeout=1)
    except serial.SerialException as e:
        print(f"Error opening port: {e}")
        sys.exit(1)

    # Initialize pygame
    pygame.init()
    screen_width = FRAME_WIDTH * args.scale
    screen_height = FRAME_HEIGHT * args.scale
    screen = pygame.display.set_mode((screen_width, screen_height))
    pygame.display.set_caption('MVS Capture')

    # Create surface for frame
    frame_surface = pygame.Surface((FRAME_WIDTH, FRAME_HEIGHT))

    # Stats
    frame_count = 0
    start_time = time.time()
    last_fps_time = start_time
    fps = 0

    print("Waiting for frames...")

    running = True
    resync_count = 0
    
    # Tuning parameters (adjustable with keyboard)
    v_offset = 0  # Vertical offset adjustment
    h_offset = 0  # Horizontal offset adjustment
    
    print("\nControls:")
    print("  W/S: Adjust vertical offset")
    print("  A/D: Adjust horizontal offset")
    print("  R: Reset offsets")
    print("  Q/ESC: Quit")
    print()
    
    while running:
        # Handle pygame events
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                running = False
            elif event.type == pygame.KEYDOWN:
                if event.key == pygame.K_ESCAPE or event.key == pygame.K_q:
                    running = False
                elif event.key == pygame.K_w:
                    v_offset -= 1
                    print(f"V offset: {v_offset}, H offset: {h_offset}")
                elif event.key == pygame.K_s:
                    v_offset += 1
                    print(f"V offset: {v_offset}, H offset: {h_offset}")
                elif event.key == pygame.K_a:
                    h_offset -= 1
                    print(f"V offset: {v_offset}, H offset: {h_offset}")
                elif event.key == pygame.K_d:
                    h_offset += 1
                    print(f"V offset: {v_offset}, H offset: {h_offset}")
                elif event.key == pygame.K_r:
                    v_offset = 0
                    h_offset = 0
                    print("Offsets reset")

        # Sync to frame
        if not sync_to_frame(ser):
            print("Lost sync, reconnecting...")
            continue

        # Read packed frame data
        packed_data = read_frame(ser)
        if packed_data is None:
            resync_count += 1
            if resync_count % 10 == 0:
                print(f"Resyncing... ({resync_count} total)")
            continue
        
        resync_count = 0  # Reset on good frame

        # Unpack pixels
        pixels = unpack_frame(packed_data)

        # Update surface with offset adjustment
        for y in range(FRAME_HEIGHT):
            for x in range(FRAME_WIDTH):
                # Apply offsets (wrap around)
                src_y = (y + v_offset) % FRAME_HEIGHT
                src_x = (x + h_offset) % FRAME_WIDTH
                src_idx = src_y * FRAME_WIDTH + src_x
                frame_surface.set_at((x, y), pixels[src_idx])

        # Scale and display
        scaled = pygame.transform.scale(frame_surface, (screen_width, screen_height))
        screen.blit(scaled, (0, 0))
        pygame.display.flip()

        # Update stats
        frame_count += 1
        current_time = time.time()
        if current_time - last_fps_time >= 1.0:
            fps = frame_count / (current_time - last_fps_time)
            frame_count = 0
            last_fps_time = current_time
            pygame.display.set_caption(f'MVS Capture - {fps:.1f} fps | V:{v_offset} H:{h_offset}')

    # Cleanup
    ser.close()
    pygame.quit()
    print("Done.")


if __name__ == '__main__':
    main()