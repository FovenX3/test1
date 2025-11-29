#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "mvs_sync.pio.h"

// =============================================================================
// Pin Configuration
// =============================================================================

#define PIN_R0 0   // RGB data: GPIO 0-14 (15 bits)
#define PIN_GND 15 // Dummy bit for 16-bit alignment
#define PIN_CSYNC 16
#define PIN_PCLK 17

// =============================================================================
// MVS Timing Constants
// =============================================================================

#define H_THRESHOLD 288
#define NEO_H_TOTAL 384
#define NEO_H_ACTIVE_START 27 // Reduced from 57 to account for CSYNC sync

#define FRAME_WIDTH 320
#define FRAME_HEIGHT 224

// =============================================================================
// Buffer Configuration
// =============================================================================

// Pico 2 has 520KB RAM - plenty of room!
// Full frame needs: (224 + 22 offset + 10 margin) × 384 × 16 / 32 = 49,152 words
// Frame buffer: 320 × 224 = 71,680 bytes (~72KB)
// Available: ~520KB - 72KB - 20KB stack = 428KB = 107,000 words
#define RAW_BUFFER_WORDS 50000 // ~200KB - plenty of margin for Pico 2

// Grayscale frame buffer (1 byte per pixel for testing)
#define FRAME_SIZE_BYTES (FRAME_WIDTH * FRAME_HEIGHT)

#define SYNC_WORD_1 0xAA55
#define SYNC_WORD_2 0x55AA

static uint32_t raw_buffer[RAW_BUFFER_WORDS];
static uint8_t frame_buffer[FRAME_WIDTH * FRAME_HEIGHT]; // 8-bit grayscale for testing

static int dma_chan;

static volatile bool frame_captured = false;
static uint32_t frame_count = 0;
static uint32_t capture_offset_lines = 16; // Reduced to fit full active area in buffer

// =============================================================================
// Debug LED Functions
// =============================================================================

static inline void led_init(void)
{
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
}

static inline void led_on(void)
{
    gpio_put(PICO_DEFAULT_LED_PIN, 1);
}

static inline void led_off(void)
{
    gpio_put(PICO_DEFAULT_LED_PIN, 0);
}

// Blink pattern: N short blinks, then pause
// Use to identify which stage we reached
static void led_blink_code(int code)
{
    for (int i = 0; i < code; i++)
    {
        led_on();
        sleep_ms(150);
        led_off();
        sleep_ms(150);
    }
    sleep_ms(500);
}

// Rapid blink = stuck/error
static void led_error_loop(int code)
{
    while (true)
    {
        led_blink_code(code);
        sleep_ms(1000);
    }
}

// =============================================================================
// VSYNC Detection State Machine
// =============================================================================

typedef enum
{
    SYNC_WAIT_FIRST_VSYNC,
    SYNC_WAIT_SECOND_VSYNC,
    SYNC_WAIT_FIRST_HSYNC_AFTER_VSYNC,
    SYNC_READY_TO_CAPTURE
} sync_state_t;

static inline void drain_sync_fifo(PIO pio, uint sm)
{
    while (!pio_sm_is_rx_fifo_empty(pio, sm))
    {
        pio_sm_get(pio, sm);
    }
}

// Non-blocking check with timeout
static bool read_sync_fifo_timeout(PIO pio, uint sm, uint32_t *value, uint32_t timeout_ms)
{
    absolute_time_t timeout = make_timeout_time_ms(timeout_ms);

    while (pio_sm_is_rx_fifo_empty(pio, sm))
    {
        if (absolute_time_diff_us(get_absolute_time(), timeout) <= 0)
        {
            return false; // Timeout
        }
        tight_loop_contents();
    }
    *value = pio_sm_get(pio, sm);
    return true;
}

static inline uint32_t read_sync_fifo(PIO pio, uint sm)
{
    while (pio_sm_is_rx_fifo_empty(pio, sm))
    {
        tight_loop_contents();
    }
    return pio_sm_get(pio, sm);
}

// Wait for frame boundary with timeout - returns false if no signal
static bool wait_for_frame_start_timeout(PIO pio, uint sm_sync, uint32_t timeout_ms)
{
    sync_state_t state = SYNC_WAIT_FIRST_VSYNC;
    uint32_t equ_count = 0;
    absolute_time_t timeout = make_timeout_time_ms(timeout_ms);

    while (state != SYNC_READY_TO_CAPTURE)
    {
        // Check timeout
        if (absolute_time_diff_us(get_absolute_time(), timeout) <= 0)
        {
            return false;
        }

        // Non-blocking FIFO check
        if (pio_sm_is_rx_fifo_empty(pio, sm_sync))
        {
            tight_loop_contents();
            continue;
        }

        uint32_t h_ctr = pio_sm_get(pio, sm_sync);
        bool is_short_pulse = (h_ctr <= H_THRESHOLD);

        switch (state)
        {
        case SYNC_WAIT_FIRST_VSYNC:
            if (is_short_pulse)
            {
                equ_count++;
            }
            else if (equ_count >= 8)
            {
                state = SYNC_WAIT_SECOND_VSYNC;
                equ_count = 0;
            }
            else
            {
                equ_count = 0;
            }
            break;

        case SYNC_WAIT_SECOND_VSYNC:
            if (is_short_pulse)
            {
                equ_count++;
            }
            else if (equ_count >= 8)
            {
                drain_sync_fifo(pio, sm_sync);
                state = SYNC_WAIT_FIRST_HSYNC_AFTER_VSYNC;
                equ_count = 0;
            }
            else
            {
                equ_count = 0;
            }
            break;

        case SYNC_WAIT_FIRST_HSYNC_AFTER_VSYNC:
            if (is_short_pulse)
            {
                equ_count++;
            }
            else
            {
                state = SYNC_READY_TO_CAPTURE;
            }
            break;

        case SYNC_READY_TO_CAPTURE:
            break;
        }
    }
    return true;
}

static bool wait_for_vsync_timeout(PIO pio, uint sm_sync, uint32_t timeout_ms)
{
    uint32_t equ_count = 0;
    absolute_time_t timeout = make_timeout_time_ms(timeout_ms);

    while (true)
    {
        if (absolute_time_diff_us(get_absolute_time(), timeout) <= 0)
        {
            return false;
        }

        if (pio_sm_is_rx_fifo_empty(pio, sm_sync))
        {
            tight_loop_contents();
            continue;
        }

        uint32_t h_ctr = pio_sm_get(pio, sm_sync);
        bool is_short_pulse = (h_ctr <= H_THRESHOLD);

        if (is_short_pulse)
        {
            equ_count++;
        }
        else
        {
            if (equ_count >= 8)
            {
                return true;
            }
            equ_count = 0;
        }
    }
}

// Wait for VSYNC, then wait for first stable HSYNC - more deterministic
static bool wait_for_vsync_and_hsync(PIO pio, uint sm_sync, uint32_t timeout_ms)
{
    uint32_t equ_count = 0;
    absolute_time_t timeout = make_timeout_time_ms(timeout_ms);
    bool in_vsync = false;

    while (true)
    {
        if (absolute_time_diff_us(get_absolute_time(), timeout) <= 0)
        {
            return false;
        }

        if (pio_sm_is_rx_fifo_empty(pio, sm_sync))
        {
            tight_loop_contents();
            continue;
        }

        uint32_t h_ctr = pio_sm_get(pio, sm_sync);
        bool is_short_pulse = (h_ctr <= H_THRESHOLD);

        if (!in_vsync)
        {
            // Looking for VSYNC (8+ short pulses)
            if (is_short_pulse)
            {
                equ_count++;
            }
            else
            {
                if (equ_count >= 8)
                {
                    in_vsync = true;
                    equ_count = 0;
                    // Drain any queued pulses to get closer to real-time
                    drain_sync_fifo(pio, sm_sync);
                }
                else
                {
                    equ_count = 0;
                }
            }
        }
        else
        {
            // In VSYNC region, wait for first normal HSYNC
            if (is_short_pulse)
            {
                equ_count++; // Still in equalizing pulses
            }
            else
            {
                // First normal HSYNC after VSYNC - this is our trigger point!
                return true;
            }
        }
    }
}

// =============================================================================
// Frame Processing
// =============================================================================

// Extract pixels from raw capture buffer
// TEST MODE: Only using R0-R4 (GPIO 0-4) to verify wiring
// Output: 8-bit grayscale (5-bit red expanded to 8-bit)
static void process_frame(uint32_t *raw_buf, uint8_t *frame_buf, uint32_t words_captured)
{
    uint32_t raw_bit_idx = capture_offset_lines * NEO_H_TOTAL * 16;
    uint32_t out_idx = 0;

    for (uint32_t line = 0; line < FRAME_HEIGHT; line++)
    {
        raw_bit_idx += NEO_H_ACTIVE_START * 16;

        for (uint32_t x = 0; x < FRAME_WIDTH; x++)
        {
            uint32_t word_idx = raw_bit_idx / 32;
            uint32_t bit_idx = raw_bit_idx % 32;

            uint8_t pixel = 0;
            if (word_idx < words_captured)
            {
                uint32_t raw_val = raw_buf[word_idx] >> bit_idx;
                if (bit_idx > 16 && (word_idx + 1) < words_captured)
                {
                    raw_val |= raw_buf[word_idx + 1] << (32 - bit_idx);
                }

                // Extract R0-R4 (bits 0-4) and expand to 8-bit
                uint8_t r5 = raw_val & 0x1F;
                pixel = (r5 << 3) | (r5 >> 2); // Expand 5-bit to 8-bit
            }

            frame_buf[out_idx++] = pixel;
            raw_bit_idx += 16;
        }

        raw_bit_idx += (NEO_H_TOTAL - NEO_H_ACTIVE_START - FRAME_WIDTH) * 16;
    }
}

// =============================================================================
// USB Transmission
// =============================================================================

// Use TinyUSB directly for better throughput (stdio_usb uses it internally)
#include "tusb.h"

static bool send_frame_usb(uint8_t *frame_buf)
{
    if (!tud_cdc_connected())
    {
        return false;
    }

    // Send header
    uint8_t header[4] = {
        SYNC_WORD_1 & 0xFF,
        (SYNC_WORD_1 >> 8) & 0xFF,
        SYNC_WORD_2 & 0xFF,
        (SYNC_WORD_2 >> 8) & 0xFF};

    tud_cdc_write(header, 4);

    // Send frame in chunks
    uint8_t *ptr = frame_buf;
    uint32_t remaining = FRAME_SIZE_BYTES;

    while (remaining > 0)
    {
        uint32_t available = tud_cdc_write_available();
        if (available > 0)
        {
            uint32_t chunk = (remaining < available) ? remaining : available;
            uint32_t written = tud_cdc_write(ptr, chunk);
            ptr += written;
            remaining -= written;
        }
        tud_task(); // Process USB
    }

    tud_cdc_write_flush();
    return true;
}

// =============================================================================
// DMA Configuration
// =============================================================================

static void setup_dma(PIO pio, uint sm_pixel)
{
    dma_channel_config cfg = dma_channel_get_default_config(dma_chan);
    channel_config_set_read_increment(&cfg, false);
    channel_config_set_write_increment(&cfg, true);
    channel_config_set_dreq(&cfg, pio_get_dreq(pio, sm_pixel, false));
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);

    dma_channel_configure(
        dma_chan,
        &cfg,
        raw_buffer,
        &pio->rxf[sm_pixel],
        RAW_BUFFER_WORDS,
        false);
}

// =============================================================================
// Main
// =============================================================================

int main()
{
    // Stage 1: Basic init
    stdio_init_all();
    led_init();

    // Blink 1: We're alive
    led_blink_code(1);

    // Stage 2: USB init (handled by stdio_init_all)
    // Give USB time to enumerate
    for (int i = 0; i < 20; i++)
    {
        sleep_ms(100);
        led_on();
        sleep_ms(50);
        led_off();
    }

    // Blink 2: USB initialized
    led_blink_code(2);

    // Stage 3: PIO setup
    PIO pio = pio0;

    uint offset_sync = pio_add_program(pio, &mvs_sync_4a_program);
    uint sm_sync = pio_claim_unused_sm(pio, true);
    mvs_sync_4a_program_init(pio, sm_sync, offset_sync, PIN_CSYNC, PIN_PCLK);

    uint offset_pixel = pio_add_program(pio, &mvs_pixel_capture_program);
    uint sm_pixel = pio_claim_unused_sm(pio, true);
    mvs_pixel_capture_program_init(pio, sm_pixel, offset_pixel, PIN_R0, PIN_GND, PIN_CSYNC, PIN_PCLK);

    // Blink 3: PIO initialized
    led_blink_code(3);

    // Stage 4: DMA setup
    dma_chan = dma_claim_unused_channel(true);
    setup_dma(pio, sm_pixel);

    // Blink 4: DMA initialized
    led_blink_code(4);

    // Stage 5: Wait for sync signal (with timeout)
    // Try for 5 seconds, if no signal, enter error mode
    led_on(); // LED on while waiting for sync

    bool got_sync = wait_for_frame_start_timeout(pio, sm_sync, 5000);

    if (!got_sync)
    {
        // Error: No sync signal detected
        // Blink 5 repeatedly = stuck waiting for VSYNC
        led_error_loop(5);
    }

    // Blink 6: Got sync, starting capture
    led_blink_code(6);

    // Main capture loop
    while (true)
    {
        // Toggle LED every 15 frames
        gpio_put(PICO_DEFAULT_LED_PIN, (frame_count / 15) & 1);

        // Wait for VSYNC then first HSYNC
        bool got_sync = wait_for_vsync_and_hsync(pio, sm_sync, 100);

        if (!got_sync)
        {
            continue;
        }

        // Start capture
        dma_channel_set_write_addr(dma_chan, raw_buffer, false);
        dma_channel_set_trans_count(dma_chan, RAW_BUFFER_WORDS, false);

        pio_sm_set_enabled(pio, sm_pixel, false);
        pio_sm_clear_fifos(pio, sm_pixel);
        pio_sm_restart(pio, sm_pixel);
        pio_sm_exec(pio, sm_pixel, pio_encode_jmp(offset_pixel));
        pio_sm_set_enabled(pio, sm_pixel, true);

        dma_channel_start(dma_chan);
        pio_sm_exec(pio, sm_sync, pio_encode_irq_set(false, 4));

        // Wait for frame end
        bool got_vsync = wait_for_vsync_timeout(pio, sm_sync, 100);

        if (!got_vsync)
        {
            dma_channel_abort(dma_chan);
            continue;
        }

        dma_channel_abort(dma_chan);
        uint32_t words_captured = RAW_BUFFER_WORDS - dma_channel_hw_addr(dma_chan)->transfer_count;

        process_frame(raw_buffer, frame_buffer, words_captured);

        send_frame_usb(frame_buffer);

        frame_count++;
    }

    return 0;
}