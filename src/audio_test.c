/**
 * MVS Audio Capture Test
 *
 * Captures stereo audio from MVS via ADC inputs and streams to USB.
 *
 * Hardware setup:
 *   GPIO 26 (ADC0): MVS Left audio
 *   GPIO 27 (ADC1): MVS Right audio
 *
 * Protocol:
 *   Header: 0xAA 0x55 0x55 0xAA (4 bytes)
 *   Sample count: uint16_t little-endian (2 bytes)
 *   Samples: interleaved L/R 12-bit samples as uint16_t (4 bytes per stereo sample)
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "tusb.h"

// =============================================================================
// Configuration
// =============================================================================

// ADC pins - RP2350B uses GPIO 40-47 for ADC
#define ADC_PIN_LEFT  40  // ADC0 - Left channel
#define ADC_PIN_RIGHT 41  // ADC1 - Right channel

// Sample rate - MVS audio is ~55.5 kHz but we'll use a standard rate
#define SAMPLE_RATE 48000

// Buffer size - enough for ~10ms of audio at 48kHz stereo
#define SAMPLES_PER_BUFFER 512
#define BUFFER_SIZE_BYTES (SAMPLES_PER_BUFFER * 2 * sizeof(uint16_t))  // Stereo 16-bit

// Sync header for USB protocol
#define SYNC_BYTE_0 0xAA
#define SYNC_BYTE_1 0x55
#define SYNC_BYTE_2 0x55
#define SYNC_BYTE_3 0xAA

// =============================================================================
// Buffers
// =============================================================================

// Double-buffered DMA capture (mono)
static uint16_t adc_buffer_a[SAMPLES_PER_BUFFER];
static uint16_t adc_buffer_b[SAMPLES_PER_BUFFER];
static volatile uint8_t current_capture_buffer = 0;
static volatile bool buffer_ready = false;

static int dma_chan;

// =============================================================================
// ADC + DMA Setup
// =============================================================================

static void adc_dma_handler(void) {
    // Clear interrupt
    dma_hw->ints0 = 1u << dma_chan;

    // Mark buffer as ready
    buffer_ready = true;

    // Swap buffers and restart DMA
    current_capture_buffer = !current_capture_buffer;
    uint16_t *next_buffer = current_capture_buffer ? adc_buffer_b : adc_buffer_a;

    dma_channel_set_write_addr(dma_chan, next_buffer, false);
    dma_channel_set_trans_count(dma_chan, SAMPLES_PER_BUFFER, true);
}

static void setup_adc(void) {
    // Initialize ADC
    adc_init();

    // For RP2350B, GPIO 40-47 are ADC pins
    // ADC channel = GPIO - 40 (so GPIO 40 = ADC0, GPIO 41 = ADC1, etc.)
    // adc_gpio_init expects the GPIO number, should work with 40+
    gpio_set_function(ADC_PIN_LEFT, GPIO_FUNC_NULL);  // Disable digital function
    gpio_disable_pulls(ADC_PIN_LEFT);                  // Disable pull-up/down

    // Single channel mode (no round-robin)
    adc_set_round_robin(0x00);  // Disable round-robin
    adc_select_input(0);        // ADC0 (GPIO 40 on RP2350B)

    // Configure ADC clock for desired sample rate
    // clkdiv=999 gave correct pitch at 26.4 pkt/s
    // That means actual sample rate = 26.4 * 512 = 13.5kHz
    // Playback at 48kHz makes it sound right, so keep this
    adc_set_clkdiv(999);

    // Enable FIFO, set threshold to 1 sample
    adc_fifo_setup(
        true,   // Write to FIFO
        true,   // Enable DMA request
        1,      // DREQ threshold
        false,  // Don't shift result (keep 12-bit)
        false   // Don't reduce to 8-bit
    );
}

static void setup_dma(void) {
    // Claim DMA channel
    dma_chan = dma_claim_unused_channel(true);

    // Configure DMA
    dma_channel_config cfg = dma_channel_get_default_config(dma_chan);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_16);
    channel_config_set_read_increment(&cfg, false);  // Always read from ADC FIFO
    channel_config_set_write_increment(&cfg, true);  // Write to buffer
    channel_config_set_dreq(&cfg, DREQ_ADC);         // ADC FIFO triggers DMA

    dma_channel_configure(
        dma_chan,
        &cfg,
        adc_buffer_a,           // Initial write address
        &adc_hw->fifo,          // Read from ADC FIFO
        SAMPLES_PER_BUFFER,     // Transfer count (mono samples)
        false                   // Don't start yet
    );

    // Enable DMA interrupt
    dma_channel_set_irq0_enabled(dma_chan, true);
    irq_set_exclusive_handler(DMA_IRQ_0, adc_dma_handler);
    irq_set_enabled(DMA_IRQ_0, true);
}

// =============================================================================
// USB Transmission
// =============================================================================

static void send_audio_packet(uint16_t *samples, uint32_t sample_count) {
    if (!tud_cdc_connected()) {
        return;
    }

    // Send sync header
    uint8_t header[6] = {
        SYNC_BYTE_0,
        SYNC_BYTE_1,
        SYNC_BYTE_2,
        SYNC_BYTE_3,
        (sample_count) & 0xFF,
        (sample_count >> 8) & 0xFF
    };

    // Send header - wait for space if needed
    uint32_t header_remaining = sizeof(header);
    uint8_t *header_ptr = header;
    while (header_remaining > 0) {
        uint32_t available = tud_cdc_write_available();
        if (available > 0) {
            uint32_t chunk = (header_remaining < available) ? header_remaining : available;
            uint32_t written = tud_cdc_write(header_ptr, chunk);
            header_ptr += written;
            header_remaining -= written;
        }
        tud_task();
    }

    // Send samples
    uint8_t *ptr = (uint8_t *)samples;
    uint32_t remaining = sample_count * sizeof(uint16_t);  // Mono, 16-bit

    while (remaining > 0) {
        uint32_t available = tud_cdc_write_available();
        if (available > 0) {
            uint32_t chunk = (remaining < available) ? remaining : available;
            uint32_t written = tud_cdc_write(ptr, chunk);
            ptr += written;
            remaining -= written;
        }
        tud_task();
    }

    tud_cdc_write_flush();
}

// =============================================================================
// LED Functions
// =============================================================================

static inline void led_init(void) {
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
}

static inline void led_toggle(void) {
    gpio_xor_mask(1u << PICO_DEFAULT_LED_PIN);
}

// =============================================================================
// Main
// =============================================================================

int main() {
    // Initialize stdio for USB
    stdio_init_all();
    led_init();

    // Wait for USB enumeration
    printf("MVS Audio Capture Test\n");
    printf("Waiting for USB connection...\n");

    for (int i = 0; i < 30; i++) {
        led_toggle();
        sleep_ms(100);
    }

    printf("Initializing ADC...\n");
    printf("  Left channel:  GPIO %d (ADC0)\n", ADC_PIN_LEFT);
    printf("  Right channel: GPIO %d (ADC1)\n", ADC_PIN_RIGHT);
    printf("  Sample rate:   %d Hz\n", SAMPLE_RATE);
    printf("  Buffer size:   %d samples\n", SAMPLES_PER_BUFFER);

    // Setup ADC and DMA
    setup_adc();
    setup_dma();

    printf("Starting audio capture on GPIO %d...\n", ADC_PIN_LEFT);

    // Start DMA
    dma_channel_start(dma_chan);

    // Start free-running ADC
    adc_run(true);

    uint32_t packet_count = 0;

    while (true) {
        tud_task();

        if (buffer_ready) {
            buffer_ready = false;
            uint16_t *ready_buffer = current_capture_buffer ? adc_buffer_a : adc_buffer_b;
            send_audio_packet(ready_buffer, SAMPLES_PER_BUFFER);
            packet_count++;
            if (packet_count % 50 == 0) {
                led_toggle();
            }
        }
    }

    return 0;
}
