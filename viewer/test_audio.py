#!/usr/bin/env python3
"""
Automated audio capture and analysis for YM2610 testing.
Run this script to capture audio and get a full analysis report.
"""

import sys
import os
import glob
import time
import wave
import struct
import numpy as np
import serial
from datetime import datetime

# Config
SAMPLE_RATE = 55500
CAPTURE_SECONDS = 3
OUTPUT_DIR = os.path.dirname(os.path.abspath(__file__))

def find_pico():
    """Find Pico serial port"""
    patterns = ['/dev/tty.usbmodem*', '/dev/ttyACM*', '/dev/ttyUSB*']
    for pattern in patterns:
        ports = glob.glob(pattern)
        if ports:
            return ports[0]
    return None

def capture_audio(port, seconds):
    """Capture audio from Pico and return as numpy array"""
    samples_needed = SAMPLE_RATE * seconds * 2  # stereo
    bytes_needed = samples_needed * 2  # 16-bit

    print(f"Connecting to {port}...")
    ser = serial.Serial(port, 115200, timeout=2)
    time.sleep(0.3)
    ser.reset_input_buffer()

    # Wait for READY (no timeout, like audio_capture.py)
    print("Waiting for device...")
    while True:
        line = ser.readline().decode('utf-8', errors='ignore').strip()
        if 'READY' in line:
            print("  READY")
            break

    # Trigger capture
    print(f"Capturing {seconds}s of audio...")
    ser.write(b'x')

    # Wait for STREAM_START
    while True:
        line = ser.readline().decode('utf-8', errors='ignore').strip()
        if 'STREAM_START' in line:
            print("  STREAM_START")
            break

    # Capture raw audio
    raw_data = b''
    start = time.time()
    while len(raw_data) < bytes_needed:
        chunk = ser.read(min(4096, bytes_needed - len(raw_data)))
        if chunk:
            raw_data += chunk
        if time.time() - start > seconds + 5:
            break

    ser.close()

    if len(raw_data) < bytes_needed:
        return None, f"Only captured {len(raw_data)}/{bytes_needed} bytes"

    # Convert to numpy
    samples = np.frombuffer(raw_data[:bytes_needed], dtype=np.int16)
    return samples, None

def analyze_audio(samples):
    """Analyze audio and return report dict"""
    left = samples[0::2]
    right = samples[1::2]

    report = {}

    # Basic stats
    report['duration_sec'] = len(left) / SAMPLE_RATE
    report['sample_rate'] = SAMPLE_RATE
    report['total_samples'] = len(left)

    # Amplitude stats
    report['left_min'] = int(np.min(left))
    report['left_max'] = int(np.max(left))
    report['left_mean'] = float(np.mean(left))
    report['left_rms'] = float(np.sqrt(np.mean(left.astype(np.float64)**2)))

    report['right_min'] = int(np.min(right))
    report['right_max'] = int(np.max(right))
    report['right_mean'] = float(np.mean(right))
    report['right_rms'] = float(np.sqrt(np.mean(right.astype(np.float64)**2)))

    # DC offset (as percentage of full scale)
    report['left_dc_offset_pct'] = abs(report['left_mean']) / 32768 * 100
    report['right_dc_offset_pct'] = abs(report['right_mean']) / 32768 * 100

    # Peak level in dBFS
    peak = max(abs(report['left_min']), report['left_max'],
               abs(report['right_min']), report['right_max'])
    report['peak_dbfs'] = 20 * np.log10(peak / 32768) if peak > 0 else -100

    # RMS level in dBFS
    rms_avg = (report['left_rms'] + report['right_rms']) / 2
    report['rms_dbfs'] = 20 * np.log10(rms_avg / 32768) if rms_avg > 0 else -100

    # Clipping detection
    clip_threshold = 32700
    report['left_clips'] = int(np.sum(np.abs(left) > clip_threshold))
    report['right_clips'] = int(np.sum(np.abs(right) > clip_threshold))

    # Silence detection (RMS < -50 dBFS)
    report['is_silence'] = report['rms_dbfs'] < -50

    # FFT analysis - find dominant frequencies
    fft_left = np.abs(np.fft.rfft(left))
    freqs = np.fft.rfftfreq(len(left), 1/SAMPLE_RATE)

    # Find top 5 frequency peaks (ignore DC)
    fft_left[0] = 0  # Remove DC
    peak_indices = np.argsort(fft_left)[-5:][::-1]
    report['dominant_freqs'] = [(float(freqs[i]), float(fft_left[i])) for i in peak_indices]

    # Spectral centroid (brightness indicator)
    spectral_sum = np.sum(fft_left)
    if spectral_sum > 0:
        report['spectral_centroid_hz'] = float(np.sum(freqs * fft_left) / spectral_sum)
    else:
        report['spectral_centroid_hz'] = 0

    # Check for suspicious patterns
    report['issues'] = []

    if report['is_silence']:
        report['issues'].append("SILENCE: Audio appears to be silent")

    if report['left_dc_offset_pct'] > 1:
        report['issues'].append(f"DC_OFFSET_LEFT: {report['left_dc_offset_pct']:.1f}%")
    if report['right_dc_offset_pct'] > 1:
        report['issues'].append(f"DC_OFFSET_RIGHT: {report['right_dc_offset_pct']:.1f}%")

    if report['left_clips'] > 0:
        report['issues'].append(f"CLIPPING_LEFT: {report['left_clips']} samples")
    if report['right_clips'] > 0:
        report['issues'].append(f"CLIPPING_RIGHT: {report['right_clips']} samples")

    # Check for stuck bits (constant value)
    if np.std(left) < 10:
        report['issues'].append("STUCK_LEFT: Left channel has very low variance")
    if np.std(right) < 10:
        report['issues'].append("STUCK_RIGHT: Right channel has very low variance")

    # Check for extreme asymmetry between channels
    if report['left_rms'] > 0 and report['right_rms'] > 0:
        ratio = max(report['left_rms'], report['right_rms']) / min(report['left_rms'], report['right_rms'])
        if ratio > 10:
            report['issues'].append(f"CHANNEL_IMBALANCE: {ratio:.1f}x difference")

    # Audio quality score (simple heuristic)
    score = 100
    if report['is_silence']:
        score = 0
    else:
        if report['left_dc_offset_pct'] > 1:
            score -= 10
        if report['right_dc_offset_pct'] > 1:
            score -= 10
        if report['left_clips'] > 0:
            score -= 20
        if report['right_clips'] > 0:
            score -= 20
        if report['rms_dbfs'] < -40:
            score -= 15  # Too quiet
        if report['spectral_centroid_hz'] < 500:
            score -= 10  # Possibly muffled/wrong

    report['quality_score'] = max(0, score)

    return report

def generate_plot(samples, filename):
    """Generate waveform and spectrum plot"""
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt

    left = samples[0::2]
    right = samples[1::2]

    fig, axes = plt.subplots(2, 2, figsize=(14, 8))

    # Time domain - first 3000 samples (~54ms)
    n = min(3000, len(left))
    t = np.arange(n) / SAMPLE_RATE * 1000

    axes[0, 0].plot(t, left[:n], 'b-', linewidth=0.5, label='Left')
    axes[0, 0].plot(t, right[:n], 'r-', linewidth=0.5, alpha=0.7, label='Right')
    axes[0, 0].set_xlabel('Time (ms)')
    axes[0, 0].set_ylabel('Amplitude')
    axes[0, 0].set_title('Waveform (first 54ms)')
    axes[0, 0].legend()
    axes[0, 0].grid(True, alpha=0.3)

    # Zoomed - first 200 samples
    axes[0, 1].plot(np.arange(200), left[:200], 'b.-', markersize=2, label='Left')
    axes[0, 1].plot(np.arange(200), right[:200], 'r.-', markersize=2, alpha=0.7, label='Right')
    axes[0, 1].set_xlabel('Sample #')
    axes[0, 1].set_ylabel('Amplitude')
    axes[0, 1].set_title('Zoomed (first 200 samples)')
    axes[0, 1].legend()
    axes[0, 1].grid(True, alpha=0.3)

    # FFT spectrum
    fft_left = np.abs(np.fft.rfft(left))
    fft_right = np.abs(np.fft.rfft(right))
    freqs = np.fft.rfftfreq(len(left), 1/SAMPLE_RATE)

    # Only show up to 10kHz
    max_freq_idx = np.searchsorted(freqs, 10000)

    axes[1, 0].semilogy(freqs[:max_freq_idx], fft_left[:max_freq_idx], 'b-', linewidth=0.5, label='Left')
    axes[1, 0].semilogy(freqs[:max_freq_idx], fft_right[:max_freq_idx], 'r-', linewidth=0.5, alpha=0.7, label='Right')
    axes[1, 0].set_xlabel('Frequency (Hz)')
    axes[1, 0].set_ylabel('Magnitude')
    axes[1, 0].set_title('Frequency Spectrum (0-10kHz)')
    axes[1, 0].legend()
    axes[1, 0].grid(True, alpha=0.3)

    # Histogram of sample values
    axes[1, 1].hist(left, bins=100, alpha=0.5, label='Left', color='blue')
    axes[1, 1].hist(right, bins=100, alpha=0.5, label='Right', color='red')
    axes[1, 1].set_xlabel('Sample Value')
    axes[1, 1].set_ylabel('Count')
    axes[1, 1].set_title('Amplitude Distribution')
    axes[1, 1].legend()
    axes[1, 1].grid(True, alpha=0.3)

    plt.tight_layout()
    plt.savefig(filename, dpi=120)
    plt.close()

    return filename

def print_report(report):
    """Print analysis report"""
    print("\n" + "="*60)
    print("AUDIO ANALYSIS REPORT")
    print("="*60)

    print(f"\n[BASIC INFO]")
    print(f"  Duration:     {report['duration_sec']:.2f}s")
    print(f"  Sample Rate:  {report['sample_rate']} Hz")
    print(f"  Samples:      {report['total_samples']}")

    print(f"\n[LEVELS]")
    print(f"  Peak:         {report['peak_dbfs']:.1f} dBFS")
    print(f"  RMS:          {report['rms_dbfs']:.1f} dBFS")
    print(f"  Left range:   {report['left_min']} to {report['left_max']}")
    print(f"  Right range:  {report['right_min']} to {report['right_max']}")

    print(f"\n[DC OFFSET]")
    print(f"  Left:         {report['left_mean']:.1f} ({report['left_dc_offset_pct']:.2f}%)")
    print(f"  Right:        {report['right_mean']:.1f} ({report['right_dc_offset_pct']:.2f}%)")

    print(f"\n[SPECTRUM]")
    print(f"  Centroid:     {report['spectral_centroid_hz']:.0f} Hz")
    print(f"  Top freqs:    ", end="")
    top_freqs = [f"{f:.0f}Hz" for f, _ in report['dominant_freqs'][:3]]
    print(", ".join(top_freqs))

    print(f"\n[QUALITY]")
    print(f"  Score:        {report['quality_score']}/100")
    if report['issues']:
        print(f"  Issues:")
        for issue in report['issues']:
            print(f"    - {issue}")
    else:
        print(f"  Issues:       None detected")

    print("\n" + "="*60)

def main():
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    wav_file = os.path.join(OUTPUT_DIR, f"test_{timestamp}.wav")
    plot_file = os.path.join(OUTPUT_DIR, f"test_{timestamp}.png")

    # Find Pico
    port = find_pico()
    if not port:
        print("ERROR: Could not find Pico serial port")
        sys.exit(1)

    # Capture
    samples, error = capture_audio(port, CAPTURE_SECONDS)
    if error:
        print(f"ERROR: {error}")
        sys.exit(1)

    print(f"Captured {len(samples)} samples")

    # Save WAV
    with wave.open(wav_file, 'wb') as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(samples.tobytes())
    print(f"Saved: {wav_file}")

    # Analyze
    report = analyze_audio(samples)

    # Generate plot
    generate_plot(samples, plot_file)
    print(f"Saved: {plot_file}")

    # Print report
    print_report(report)

    return 0 if report['quality_score'] >= 50 else 1

if __name__ == '__main__':
    sys.exit(main())
