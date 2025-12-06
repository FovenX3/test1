#!/usr/bin/env python3
"""Plot audio waveform to visualize distortion"""

import sys
import wave
import numpy as np
import matplotlib.pyplot as plt

def plot(filename):
    with wave.open(filename, 'rb') as w:
        channels = w.getnchannels()
        sample_width = w.getsampwidth()
        sample_rate = w.getframerate()
        n_frames = w.getnframes()
        raw = w.readframes(n_frames)

    samples = np.frombuffer(raw, dtype=np.int16)

    if channels == 2:
        left = samples[0::2]
        right = samples[1::2]
    else:
        left = right = samples

    # Plot first 2000 samples (~36ms at 55.5kHz)
    n = min(2000, len(left))
    t = np.arange(n) / sample_rate * 1000  # ms

    fig, axes = plt.subplots(3, 1, figsize=(12, 8))

    # Left channel
    axes[0].plot(t, left[:n], 'b-', linewidth=0.5)
    axes[0].set_ylabel('Left')
    axes[0].set_title(f'Audio Waveform - {filename}')
    axes[0].grid(True, alpha=0.3)

    # Right channel
    axes[1].plot(t, right[:n], 'r-', linewidth=0.5)
    axes[1].set_ylabel('Right')
    axes[1].grid(True, alpha=0.3)

    # Zoomed view - first 100 samples
    axes[2].plot(np.arange(100), left[:100], 'b.-', label='Left', markersize=3)
    axes[2].plot(np.arange(100), right[:100], 'r.-', label='Right', markersize=3)
    axes[2].set_xlabel('Sample #')
    axes[2].set_ylabel('Amplitude')
    axes[2].set_title('First 100 samples (zoomed)')
    axes[2].legend()
    axes[2].grid(True, alpha=0.3)

    plt.tight_layout()
    plt.savefig(filename.replace('.wav', '_plot.png'), dpi=150)
    print(f"Saved: {filename.replace('.wav', '_plot.png')}")
    plt.show()

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python3 plot_audio.py <file.wav>")
        sys.exit(1)
    plot(sys.argv[1])
