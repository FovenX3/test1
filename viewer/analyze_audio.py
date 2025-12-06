#!/usr/bin/env python3
"""
Analyze captured audio WAV file
"""

import sys
import wave
import struct
import numpy as np

def analyze(filename):
    with wave.open(filename, 'rb') as w:
        channels = w.getnchannels()
        sample_width = w.getsampwidth()
        sample_rate = w.getframerate()
        n_frames = w.getnframes()
        duration = n_frames / sample_rate

        print(f"=== Audio Analysis: {filename} ===\n")
        print(f"Channels:    {channels}")
        print(f"Sample rate: {sample_rate} Hz")
        print(f"Bit depth:   {sample_width * 8} bits")
        print(f"Duration:    {duration:.2f} seconds")
        print(f"Frames:      {n_frames}")
        print()

        # Read all data
        raw = w.readframes(n_frames)

    # Convert to numpy array
    if sample_width == 2:
        samples = np.frombuffer(raw, dtype=np.int16)
    else:
        samples = np.frombuffer(raw, dtype=np.int8)

    # Split channels
    if channels == 2:
        left = samples[0::2]
        right = samples[1::2]
    else:
        left = samples
        right = samples

    print("=== Amplitude Statistics ===\n")
    print(f"Left channel:")
    print(f"  Min:    {left.min()}")
    print(f"  Max:    {left.max()}")
    print(f"  Mean:   {left.mean():.1f}")
    print(f"  StdDev: {left.std():.1f}")
    print(f"  RMS:    {np.sqrt(np.mean(left.astype(np.float64)**2)):.1f}")
    print()
    print(f"Right channel:")
    print(f"  Min:    {right.min()}")
    print(f"  Max:    {right.max()}")
    print(f"  Mean:   {right.mean():.1f}")
    print(f"  StdDev: {right.std():.1f}")
    print(f"  RMS:    {np.sqrt(np.mean(right.astype(np.float64)**2)):.1f}")
    print()

    # Check for clipping
    max_val = 32767 if sample_width == 2 else 127
    left_clip = np.sum(np.abs(left) >= max_val)
    right_clip = np.sum(np.abs(right) >= max_val)
    print(f"=== Clipping Detection ===\n")
    print(f"Left clipped samples:  {left_clip} ({100*left_clip/len(left):.2f}%)")
    print(f"Right clipped samples: {right_clip} ({100*right_clip/len(right):.2f}%)")
    print()

    # DC offset
    print(f"=== DC Offset ===\n")
    print(f"Left DC offset:  {left.mean():.1f} ({100*left.mean()/max_val:.2f}%)")
    print(f"Right DC offset: {right.mean():.1f} ({100*right.mean()/max_val:.2f}%)")
    print()

    # Show first few samples
    print(f"=== First 20 Samples ===\n")
    print("  #     Left   Right")
    for i in range(min(20, len(left))):
        print(f"{i:3d}:  {left[i]:6d}  {right[i]:6d}")
    print()

    # Simple frequency analysis (zero crossings)
    left_zc = np.sum(np.diff(np.signbit(left))) / 2
    right_zc = np.sum(np.diff(np.signbit(right))) / 2
    left_freq = left_zc / duration / 2
    right_freq = right_zc / duration / 2
    print(f"=== Estimated Dominant Frequency (zero-crossing) ===\n")
    print(f"Left:  ~{left_freq:.0f} Hz")
    print(f"Right: ~{right_freq:.0f} Hz")
    print()

    # Dynamic range
    left_peak = max(abs(left.min()), abs(left.max()))
    right_peak = max(abs(right.min()), abs(right.max()))
    left_db = 20 * np.log10(left_peak / max_val) if left_peak > 0 else -96
    right_db = 20 * np.log10(right_peak / max_val) if right_peak > 0 else -96
    print(f"=== Peak Levels ===\n")
    print(f"Left peak:  {left_peak} ({left_db:.1f} dBFS)")
    print(f"Right peak: {right_peak} ({right_db:.1f} dBFS)")

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python3 analyze_audio.py <file.wav>")
        sys.exit(1)
    analyze(sys.argv[1])
