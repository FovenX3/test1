#!/usr/bin/env python3
"""Quick ADC measurement - shows min/max/avg over 2 seconds"""
import serial
import serial.tools.list_ports
import struct
import time

SYNC_HEADER = bytes([0xAA, 0x55, 0x55, 0xAA])

def find_port():
    for port in serial.tools.list_ports.comports():
        if 'usbmodem' in port.device or port.vid == 0x2E8A:
            return port.device
    return None

port = find_port()
if not port:
    print("No Pico found")
    exit(1)

ser = serial.Serial(port, 115200, timeout=0.1)
ser.reset_input_buffer()

print(f"Measuring on {port} for 2 seconds...\n")

all_samples = []
valid_packets = 0
invalid_packets = 0
start = time.time()

while time.time() - start < 2.0:
    # Sync - read byte by byte
    buf = bytearray(4)
    synced = False
    for _ in range(5000):
        b = ser.read(1)
        if not b:
            continue
        buf.pop(0)
        buf.append(b[0])
        if bytes(buf) == SYNC_HEADER:
            synced = True
            break
    
    if not synced:
        continue
    
    # Read packet
    count_bytes = ser.read(2)
    if len(count_bytes) != 2:
        continue
        
    count = struct.unpack('<H', count_bytes)[0]
    
    # Validate count (should be 512 for our firmware)
    if count == 0 or count > 1024:
        invalid_packets += 1
        continue
    
    data = ser.read(count * 2)
    if len(data) != count * 2:
        invalid_packets += 1
        continue
        
    samples = struct.unpack(f'<{count}H', data)
    
    # Validate samples (12-bit ADC = 0-4095)
    valid = [s for s in samples if s <= 4095]
    if len(valid) < len(samples) * 0.9:  # >10% invalid = bad packet
        invalid_packets += 1
        continue
    
    all_samples.extend(valid)
    valid_packets += 1

ser.close()

print(f"Valid packets: {valid_packets}, Invalid: {invalid_packets}")

if all_samples:
    min_v = min(all_samples)
    max_v = max(all_samples)
    avg_v = sum(all_samples) / len(all_samples)
    pp = max_v - min_v
    
    print(f"Samples: {len(all_samples)}")
    print(f"Min:     {min_v:4d} ({min_v * 3.3 / 4096:.3f}V)")
    print(f"Max:     {max_v:4d} ({max_v * 3.3 / 4096:.3f}V)")
    print(f"Avg:     {avg_v:4.0f} ({avg_v * 3.3 / 4096:.3f}V)")
    print(f"Peak-Peak: {pp:4d} ({pp * 3.3 / 4096:.3f}V)")
else:
    print("No valid samples received")
