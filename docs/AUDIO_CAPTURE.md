# MVS Audio Capture

## Hardware Setup

**RP2350B ADC pins:** GPIO 40-47 (not GPIO 26-29 like RP2040/RP2350A)

### Bias Circuit (Required)
MVS audio is AC-coupled and needs DC bias for the ADC:

```
3.3V ---[10k]---+--- GPIO 40
                |
GND ----[10k]---+
                |
MVS Audio --||--+  (10µF capacitor)
```

- Bias voltage: ~1.65V (mid-rail)
- Expected ADC readings: ~2048 centered, 1400-2600 range with audio

### Optional Noise Filter
Add small ceramic cap at GPIO 40 to reduce high-frequency noise:
```
GPIO 40 ---[100nF]--- GND
```

## Key Findings

1. **ADC Clock Divider:** `adc_set_clkdiv(999)` gives correct pitch
2. **Pico 3.3V rail is noisy** - avoid large filter caps on bias (kills audio)
3. **Software low-pass filter** helps reduce noise (3-tap moving average)
4. **Ground sharing** between MVS and Pico is required

## Files

- `src/audio_test.c` - Firmware (DMA-based ADC capture, USB CDC streaming)
- `viewer/audio_test.py` - Python receiver with playback and filtering
- `scripts/run_audio_test.sh` - Launcher script

## Usage

```bash
# Build and flash
cd build && make audio_test
picotool load -f src/audio_test.uf2

# Run receiver
./scripts/run_audio_test.sh

# Save to WAV file
./scripts/run_audio_test.sh --save output.wav
```
