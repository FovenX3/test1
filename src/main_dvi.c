/**
 * NeoPico-HD - MVS Capture with DVI Output
 *
 * Captures video from Neo Geo MVS and outputs via DVI/HDMI.
 *
 * Pin Configuration:
 *   MVS RGB Data:  GPIO 0-14 (15 bits)
 *   MVS Dummy:     GPIO 15
 *   MVS CSYNC:     GPIO 22
 *   MVS PCLK:      GPIO 28
 *   DVI Data:      GPIO 16-21
 *   DVI Clock:     GPIO 26-27
 *   VSYS:          Connected to Spotpear VSYS
 *
 * PIO Assignment (RP2350 has 3 PIOs):
 *   PIO0: DVI output (3 state machines for TMDS)
 *   PIO1: MVS sync detection + pixel capture
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/vreg.h"
#include "hardware/sync.h"

#include "dvi.h"
#include "dvi_serialiser.h"
#include "mvs_sync.pio.h"

// =============================================================================
// Pin Configuration
// =============================================================================

#define PIN_R0 0       // RGB data: GPIO 0-14 (15 bits)
#define PIN_GND 15     // Dummy bit for 16-bit alignment
#define PIN_CSYNC 22   // Moved for DVI
#define PIN_PCLK 28    // Moved for DVI

// =============================================================================
// DVI Configuration
// =============================================================================

static const struct dvi_serialiser_cfg neopico_dvi_cfg = {
    .pio = pio0,
    .sm_tmds = {0, 1, 2},
    .pins_tmds = {16, 18, 20},
    .pins_clk = 26,
    .invert_diffpairs = true
};

#define FRAME_WIDTH 320
#define FRAME_HEIGHT 240  // DVI frame height (MVS is 224, centered)
#define MVS_HEIGHT 224
#define VREG_VSEL VREG_VOLTAGE_1_20
#define DVI_TIMING dvi_timing_640x480p_60hz

struct dvi_inst dvi0;

// =============================================================================
// MVS Timing Constants
// =============================================================================

#define H_THRESHOLD 288
#define NEO_H_TOTAL 384
#define NEO_H_ACTIVE_START 32  // Increased to skip more horizontal blanking

// =============================================================================
// Buffers
// =============================================================================

// Raw capture buffer - needs to hold full MVS frame
// MVS: 264 lines × 384 pixels × 16 bits = ~254KB = ~64000 words
#define RAW_BUFFER_WORDS 64000
static uint32_t raw_buffer[RAW_BUFFER_WORDS];

// Single frame buffer (no double buffering to save RAM)
static uint16_t mvs_frame[FRAME_WIDTH * MVS_HEIGHT];

static int dma_chan;
static volatile bool frame_ready = false;
static volatile uint32_t frame_count = 0;
static uint32_t capture_offset_lines = 40;  // Skip vertical blanking (aggressive test)

// =============================================================================
// MVS Sync Detection
// =============================================================================

static inline void drain_sync_fifo(PIO pio, uint sm) {
    while (!pio_sm_is_rx_fifo_empty(pio, sm)) {
        pio_sm_get(pio, sm);
    }
}

static bool wait_for_vsync_and_hsync(PIO pio, uint sm_sync, uint32_t timeout_ms) {
    uint32_t equ_count = 0;
    absolute_time_t timeout = make_timeout_time_ms(timeout_ms);
    bool in_vsync = false;

    while (true) {
        if (absolute_time_diff_us(get_absolute_time(), timeout) <= 0) {
            return false;
        }

        if (pio_sm_is_rx_fifo_empty(pio, sm_sync)) {
            tight_loop_contents();
            continue;
        }

        uint32_t h_ctr = pio_sm_get(pio, sm_sync);
        bool is_short_pulse = (h_ctr <= H_THRESHOLD);

        if (!in_vsync) {
            if (is_short_pulse) {
                equ_count++;
            } else {
                if (equ_count >= 8) {
                    in_vsync = true;
                    equ_count = 0;
                    drain_sync_fifo(pio, sm_sync);
                } else {
                    equ_count = 0;
                }
            }
        } else {
            if (is_short_pulse) {
                equ_count++;
            } else {
                return true;
            }
        }
    }
}

// Non-blocking vsync check - call repeatedly, returns true when vsync detected
static uint32_t vsync_short_count = 0;

static bool check_vsync_nonblocking(PIO pio, uint sm_sync) {
    // Process all available sync pulses without blocking
    while (!pio_sm_is_rx_fifo_empty(pio, sm_sync)) {
        uint32_t h_ctr = pio_sm_get(pio, sm_sync);
        bool is_short_pulse = (h_ctr <= H_THRESHOLD);

        if (is_short_pulse) {
            vsync_short_count++;
        } else {
            if (vsync_short_count >= 8) {
                vsync_short_count = 0;
                return true;  // Vsync detected!
            }
            vsync_short_count = 0;
        }
    }
    return false;
}

// =============================================================================
// Frame Processing
// =============================================================================

// Helper to extract a pixel at a given bit index
static inline uint16_t extract_pixel(uint32_t *raw_buf, uint32_t raw_bit_idx, uint32_t words_captured) {
    uint32_t word_idx = raw_bit_idx / 32;
    uint32_t bit_idx = raw_bit_idx % 32;

    if (word_idx >= words_captured) return 0x07FF;  // CYAN for missing data

    uint32_t raw_val = raw_buf[word_idx] >> bit_idx;
    if (bit_idx > 16 && (word_idx + 1) < words_captured) {
        raw_val |= raw_buf[word_idx + 1] << (32 - bit_idx);
    }

    uint8_t r5 = raw_val & 0x1F;
    uint8_t b5 = (raw_val >> 5) & 0x1F;
    uint8_t g5 = (raw_val >> 10) & 0x1F;
    uint8_t g6 = (g5 << 1) | (g5 >> 4);
    return (r5 << 11) | (g6 << 5) | b5;
}

// Check if a pixel looks like blanking (mostly red or dark red)
static inline bool is_blanking_pixel(uint16_t pixel) {
    uint8_t r = (pixel >> 11) & 0x1F;
    uint8_t g = (pixel >> 5) & 0x3F;
    uint8_t b = pixel & 0x1F;
    // Blanking is red-ish: red > green and red > blue significantly
    return (r > 10 && r > g + 5 && r > b + 5);
}

// Process ONE line from raw MVS buffer to frame buffer (for spread processing)
static inline void process_mvs_line(uint32_t *raw_buf, uint16_t *frame_buf,
                                     uint32_t line, uint32_t words_captured) {
    // Calculate bit offset for this line (skip 20 vertical blanking lines)
    uint32_t raw_bit_idx = (20 + line) * NEO_H_TOTAL * 16;
    raw_bit_idx += NEO_H_ACTIVE_START * 16;  // Skip horizontal blanking

    uint16_t *dst = &frame_buf[line * FRAME_WIDTH];

    for (uint32_t x = 0; x < FRAME_WIDTH; x++) {
        dst[x] = extract_pixel(raw_buf, raw_bit_idx, words_captured);
        raw_bit_idx += 16;
    }
}

// =============================================================================
// DMA Configuration
// =============================================================================

static void setup_dma(PIO pio, uint sm_pixel) {
    dma_channel_config cfg = dma_channel_get_default_config(dma_chan);
    channel_config_set_read_increment(&cfg, false);
    channel_config_set_write_increment(&cfg, true);
    channel_config_set_dreq(&cfg, pio_get_dreq(pio, sm_pixel, false));
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_32);

    dma_channel_configure(
        dma_chan,
        &cfg,
        raw_buffer,  // Single buffer
        &pio->rxf[sm_pixel],
        RAW_BUFFER_WORDS,
        false);
}

// =============================================================================
// DVI scanline buffers - use 4 buffers like sprite_bounce
// =============================================================================

#define N_SCANLINE_BUFFERS 4
static uint16_t scanline_buf[N_SCANLINE_BUFFERS][FRAME_WIDTH];
static uint8_t v_offset = (FRAME_HEIGHT - MVS_HEIGHT) / 2;

// DVI line offset to compensate for timing
#define DVI_LINE_OFFSET 8

// =============================================================================
// Core 1: DVI Output (TMDS encoding and serialization)
// =============================================================================

void core1_main() {
    dvi_register_irqs_this_core(&dvi0, DMA_IRQ_0);
    while (queue_is_empty(&dvi0.q_colour_valid))
        __wfe();
    dvi_start(&dvi0);
    dvi_scanbuf_main_16bpp(&dvi0);
    __builtin_unreachable();
}

// Pattern offset for animation
static int g_pattern_offset = 0;

// Generate scanline from frame buffer
static void generate_scanline(uint16_t *buf, uint y) {
    if (y < v_offset || y >= v_offset + MVS_HEIGHT) {
        // MAGENTA for vertical border (centering 224 in 240)
        for (uint x = 0; x < FRAME_WIDTH; x++) buf[x] = 0xF81F;
    } else {
        // Read from frame buffer
        uint16_t *src = &mvs_frame[(y - v_offset) * FRAME_WIDTH];
        for (uint x = 0; x < FRAME_WIDTH; x++) {
            buf[x] = src[x];
        }
    }
}

// Update ONE line of frame buffer (called during scanline generation)
static inline void update_frame_line(int line, int offset) {
    uint16_t *dst = &mvs_frame[line * FRAME_WIDTH];
    for (int x = 0; x < FRAME_WIDTH; x++) {
        int shifted_x = (x + offset) % FRAME_WIDTH;
        uint16_t color;
        if (shifted_x < 80) color = 0x07E0;       // Green
        else if (shifted_x < 160) color = 0x001F; // Blue
        else if (shifted_x < 240) color = 0xFFE0; // Yellow
        else color = 0x07FF;                       // Cyan
        dst[x] = color;
    }
}

// =============================================================================
// Core 0: Scanline generation + MVS Capture
// =============================================================================

int main() {
    vreg_set_voltage(VREG_VSEL);
    sleep_ms(10);
    set_sys_clock_khz(DVI_TIMING.bit_clk_khz, true);

    stdio_init_all();

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);

    printf("NeoPico-HD: MVS Capture + DVI Output\n");

    // Initialize DVI
    dvi0.timing = &DVI_TIMING;
    dvi0.ser_cfg = neopico_dvi_cfg;
    dvi_init(&dvi0, next_striped_spin_lock_num(), next_striped_spin_lock_num());

    // Initialize MVS capture on PIO1
    PIO pio_mvs = pio1;
    uint offset_sync = pio_add_program(pio_mvs, &mvs_sync_4a_program);
    uint sm_sync = pio_claim_unused_sm(pio_mvs, true);
    mvs_sync_4a_program_init(pio_mvs, sm_sync, offset_sync, PIN_CSYNC, PIN_PCLK);

    uint offset_pixel = pio_add_program(pio_mvs, &mvs_pixel_capture_program);
    uint sm_pixel = pio_claim_unused_sm(pio_mvs, true);
    mvs_pixel_capture_program_init(pio_mvs, sm_pixel, offset_pixel, PIN_R0, PIN_GND, PIN_CSYNC, PIN_PCLK);

    dma_chan = dma_claim_unused_channel(true);
    setup_dma(pio_mvs, sm_pixel);

    // Start with black frames
    memset(mvs_frame, 0, sizeof(mvs_frame));
    memset(scanline_buf, 0, sizeof(scanline_buf));
    memset(raw_buffer, 0, sizeof(raw_buffer));

    // Launch DVI on Core 1
    multicore_launch_core1(core1_main);

    printf("NeoPico-HD: Starting capture + DVI\n");

    // Pre-fill free queue with scanline buffers (like sprite_bounce)
    for (int i = 0; i < N_SCANLINE_BUFFERS; ++i) {
        void *bufptr = &scanline_buf[i];
        queue_add_blocking_u32(&dvi0.q_colour_free, &bufptr);
    }

    // Fill frame buffer with blue (initial state before capture)
    for (int i = 0; i < FRAME_WIDTH * MVS_HEIGHT; i++) {
        mvs_frame[i] = 0x001F;  // Blue
    }

    // Enable MVS sync detection
    pio_sm_set_enabled(pio_mvs, sm_sync, true);

    // Capture state machine
    enum { WAIT_VSYNC, WAIT_HSYNC, CAPTURING, PROCESSING } capture_state = WAIT_VSYNC;
    static uint32_t dvi_frames = 0;
    static uint32_t words_captured = 0;

    while (true) {
        dvi_frames++;
        gpio_put(PICO_DEFAULT_LED_PIN, (dvi_frames / 30) & 1);

        // Non-blocking capture state machine
        switch (capture_state) {
        case WAIT_VSYNC:
            if (check_vsync_nonblocking(pio_mvs, sm_sync)) {
                // Vsync detected - now wait for first hsync
                drain_sync_fifo(pio_mvs, sm_sync);
                capture_state = WAIT_HSYNC;
            }
            break;

        case WAIT_HSYNC:
            // Wait for first normal hsync (long pulse) after vsync
            if (!pio_sm_is_rx_fifo_empty(pio_mvs, sm_sync)) {
                uint32_t h_ctr = pio_sm_get(pio_mvs, sm_sync);
                if (h_ctr > H_THRESHOLD) {
                    // Normal hsync - start capture NOW
                    dma_channel_set_write_addr(dma_chan, raw_buffer, false);
                    dma_channel_set_trans_count(dma_chan, RAW_BUFFER_WORDS, false);
                    pio_sm_set_enabled(pio_mvs, sm_pixel, true);
                    dma_channel_start(dma_chan);
                    pio_sm_exec(pio_mvs, sm_sync, pio_encode_irq_set(false, 4));
                    capture_state = CAPTURING;
                }
            }
            break;

        case CAPTURING:
            if (!dma_channel_is_busy(dma_chan)) {
                words_captured = RAW_BUFFER_WORDS - dma_channel_hw_addr(dma_chan)->transfer_count;
                pio_sm_set_enabled(pio_mvs, sm_pixel, false);
                capture_state = PROCESSING;
            }
            break;

        case PROCESSING:
            // Processing happens during DVI scanline generation below
            break;
        }

        // Generate DVI frame + process MVS data line-by-line
        for (uint y = 0; y < FRAME_HEIGHT; ++y) {
            uint adjusted_y = (y + FRAME_HEIGHT - DVI_LINE_OFFSET) % FRAME_HEIGHT;

            // Get free buffer from queue
            uint16_t *pixbuf;
            queue_remove_blocking_u32(&dvi0.q_colour_free, &pixbuf);

            // Fill from frame buffer
            generate_scanline(pixbuf, adjusted_y);

            // Queue for display
            queue_add_blocking_u32(&dvi0.q_colour_valid, &pixbuf);

            // Process ONE line of MVS data per scanline (spread the work)
            if (capture_state == PROCESSING && y < MVS_HEIGHT) {
                process_mvs_line(raw_buffer, mvs_frame, y, words_captured);
                if (y == MVS_HEIGHT - 1) {
                    capture_state = WAIT_VSYNC;  // Done processing, ready for next capture
                }
            }
        }

    }

    return 0;
}
