#!/usr/bin/env python3
"""
MVS Audio Capture Receiver

Receives stereo audio from RP2350B via USB and plays it through speakers.
Also displays a simple waveform visualization.

Usage:
    python audio_test.py [--port /dev/tty.usbmodem*] [--no-play] [--save output.wav]

Requirements:
    pip install pyserial pyaudio numpy

On macOS you may need:
    brew install portaudio
    pip install pyaudio
"""

import argparse
import struct
import sys
import time
import threading
import queue
from collections import deque

import serial
import serial.tools.list_ports

try:
    import numpy as np
    HAS_NUMPY = True
except ImportError:
    HAS_NUMPY = False
    print("Warning: numpy not installed, visualization disabled")

try:
    import pyaudio
    HAS_PYAUDIO = True
except ImportError:
    HAS_PYAUDIO = False
    print("Warning: pyaudio not installed, audio playback disabled")

# Protocol constants (must match Pico)
SYNC_HEADER = bytes([0xAA, 0x55, 0x55, 0xAA])
SAMPLE_RATE = 48000
SAMPLES_PER_BUFFER = 512


def find_pico_port():
    """Auto-detect Pico CDC port."""
    ports = serial.tools.list_ports.comports()
    for port in ports:
        if 'ACM' in port.device or 'usbmodem' in port.device:
            return port.device
        if port.vid == 0x2E8A:  # Raspberry Pi vendor ID
            return port.device
    return None


def sync_to_packet(ser):
    """Find sync header in stream."""
    buffer = bytearray(4)
    bytes_searched = 0
    max_bytes = 20000  # ~10 packets worth

    while bytes_searched < max_bytes:
        # Read in chunks for efficiency
        chunk = ser.read(256)
        if not chunk:
            continue
        for byte in chunk:
            buffer.pop(0)
            buffer.append(byte)
            bytes_searched += 1
            if bytes(buffer) == SYNC_HEADER:
                return True
    return False


def read_packet(ser):
    """Read a complete audio packet."""
    # Read sample count (2 bytes, little-endian)
    count_bytes = ser.read(2)
    if len(count_bytes) != 2:
        return None

    sample_count = struct.unpack('<H', count_bytes)[0]

    # Read samples (mono 16-bit = 2 bytes per sample)
    data_size = sample_count * 2  # mono * 16-bit
    sample_data = ser.read(data_size)
    if len(sample_data) != data_size:
        return None

    return sample_count, sample_data


def convert_to_audio(sample_data, sample_count):
    """Convert 12-bit ADC samples to 16-bit audio (mono) with low-pass filter."""
    if sample_count == 0:
        return b''

    # Unpack as 16-bit values (only lower 12 bits are valid)
    samples = struct.unpack(f'<{sample_count}H', sample_data)

    if len(samples) == 0:
        return b''

    # Mask to 12-bit (in case of any corruption)
    samples = [s & 0xFFF for s in samples]

    # Auto-center: use average of this buffer as DC offset
    dc_offset = sum(samples) // len(samples)

    # Convert to signed and scale
    audio_samples = []
    for sample in samples:
        # Center and scale (12-bit to 16-bit signed)
        signed_sample = ((sample - dc_offset) * 16)
        # Clamp to valid range
        signed_sample = max(-32768, min(32767, signed_sample))
        audio_samples.append(signed_sample)

    # Simple low-pass filter (3-tap moving average)
    if len(audio_samples) >= 3:
        filtered = []
        filtered.append(audio_samples[0])
        for i in range(1, len(audio_samples) - 1):
            avg = (audio_samples[i-1] + audio_samples[i] * 2 + audio_samples[i+1]) // 4
            filtered.append(avg)
        filtered.append(audio_samples[-1])
        audio_samples = filtered

    return struct.pack(f'<{len(audio_samples)}h', *audio_samples)


class AudioPlayer:
    """Threaded audio playback using PyAudio."""

    def __init__(self, sample_rate=48000, channels=1):
        self.sample_rate = sample_rate
        self.channels = channels
        self.audio_queue = queue.Queue(maxsize=20)
        self.running = False
        self.thread = None

        if HAS_PYAUDIO:
            self.pa = pyaudio.PyAudio()
            self.stream = self.pa.open(
                format=pyaudio.paInt16,
                channels=channels,
                rate=sample_rate,
                output=True,
                frames_per_buffer=SAMPLES_PER_BUFFER
            )
        else:
            self.pa = None
            self.stream = None

    def start(self):
        if not HAS_PYAUDIO:
            return
        self.running = True
        self.thread = threading.Thread(target=self._playback_thread, daemon=True)
        self.thread.start()

    def stop(self):
        self.running = False
        if self.thread:
            self.thread.join(timeout=1.0)
        if self.stream:
            self.stream.stop_stream()
            self.stream.close()
        if self.pa:
            self.pa.terminate()

    def queue_audio(self, audio_data):
        if not HAS_PYAUDIO:
            return
        try:
            self.audio_queue.put_nowait(audio_data)
        except queue.Full:
            # Drop oldest to prevent latency buildup
            try:
                self.audio_queue.get_nowait()
                self.audio_queue.put_nowait(audio_data)
            except queue.Empty:
                pass

    def _playback_thread(self):
        while self.running:
            try:
                audio_data = self.audio_queue.get(timeout=0.1)
                self.stream.write(audio_data)
            except queue.Empty:
                continue


class WaveformDisplay:
    """Simple terminal-based waveform display."""

    def __init__(self, width=60, height=10):
        self.width = width
        self.height = height
        self.history = deque(maxlen=width)

    def update(self, sample_data, sample_count):
        if not HAS_NUMPY:
            return

        # Unpack samples (mono)
        samples = np.frombuffer(sample_data, dtype=np.uint16).astype(np.float32)

        # Auto-center and calculate RMS
        dc_offset = np.mean(samples)
        centered = samples - dc_offset
        rms = np.sqrt(np.mean(centered ** 2)) / 2048  # Normalize to 12-bit range

        self.history.append(min(1.0, rms * 4))  # Scale up for visibility

    def render(self):
        if len(self.history) == 0:
            return ""

        # Single channel (mono)
        return "Audio: " + self._render_bar(list(self.history)[-1])

    def _render_bar(self, level):
        bar_width = int(level * (self.width - 3))
        bar = "#" * bar_width + "-" * (self.width - 3 - bar_width)
        return f"[{bar}]"


def main():
    parser = argparse.ArgumentParser(description='MVS Audio Capture Receiver')
    parser.add_argument('--port', '-p', help='Serial port (auto-detect if not specified)')
    parser.add_argument('--no-play', action='store_true', help='Disable audio playback')
    parser.add_argument('--save', '-s', metavar='FILE', help='Save audio to WAV file')
    parser.add_argument('--baud', '-b', type=int, default=115200, help='Baud rate')
    parser.add_argument('--quiet', '-q', action='store_true', help='Minimal output')
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

    try:
        ser = serial.Serial(port, args.baud, timeout=0.1)
        ser.reset_input_buffer()  # Clear any stale data
    except serial.SerialException as e:
        print(f"Error opening port: {e}")
        sys.exit(1)

    # Setup audio playback
    player = None
    if not args.no_play and HAS_PYAUDIO:
        print("Starting audio playback (mono)...")
        player = AudioPlayer(SAMPLE_RATE, 1)
        player.start()

    # Setup WAV file saving
    wav_file = None
    wav_samples = []
    if args.save:
        print(f"Recording to {args.save}...")

    # Setup waveform display
    waveform = WaveformDisplay()

    print("\nWaiting for audio data... (Ctrl+C to stop)\n")

    packet_count = 0
    start_time = time.time()
    last_display_time = start_time
    resync_count = 0

    try:
        while True:
            # Sync to packet
            if not sync_to_packet(ser):
                print("Lost sync, searching...")
                resync_count += 1
                continue

            # Read packet
            result = read_packet(ser)
            if result is None:
                resync_count += 1
                continue

            sample_count, sample_data = result
            packet_count += 1

            # Convert and play audio
            audio_data = convert_to_audio(sample_data, sample_count)

            if player:
                player.queue_audio(audio_data)

            if args.save:
                wav_samples.append(audio_data)

            # Update waveform display
            waveform.update(sample_data, sample_count)

            # Display status every ~100ms
            now = time.time()
            if not args.quiet and now - last_display_time >= 0.1:
                elapsed = now - start_time
                rate = packet_count / elapsed if elapsed > 0 else 0

                # Clear previous lines and redraw
                sys.stdout.write("\033[2A\033[J")  # Move up 2 lines and clear
                print(f"Packets: {packet_count:6d} | Rate: {rate:5.1f} pkt/s | Resyncs: {resync_count}")
                print(waveform.render())
                sys.stdout.flush()

                last_display_time = now

    except KeyboardInterrupt:
        print("\n\nStopping...")

    finally:
        # Cleanup
        ser.close()

        if player:
            player.stop()

        # Save WAV file
        if args.save and wav_samples:
            import wave
            print(f"Saving {len(wav_samples)} packets to {args.save}...")
            with wave.open(args.save, 'wb') as wf:
                wf.setnchannels(1)  # Mono
                wf.setsampwidth(2)  # 16-bit
                wf.setframerate(SAMPLE_RATE)
                wf.writeframes(b''.join(wav_samples))
            print(f"Saved {args.save}")

    print("Done.")


if __name__ == '__main__':
    main()
