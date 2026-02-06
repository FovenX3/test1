#include "video_capture.h"
#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "video_buffers.h"
#include "video_capture.pio.h"
#include "hardware_config.h"

static PIO g_pio = pio0;
static uint g_sm = 0;
static uint g_offset = 0;
static int  g_dma_chan = -1;
// 将 DMA 配置保存为全局变量
static dma_channel_config g_dma_config; 

// 帧同步标志
static volatile bool g_vsync_detected = false;

// VSYNC 中断
static void vsync_irq_handler(uint gpio, uint32_t events) {
    g_vsync_detected = true;
}

void video_capture_init(uint active_height)
{
    // 1. GPIO 初始化
    gpio_init(PIN_HSYNC); gpio_set_dir(PIN_HSYNC, GPIO_IN);
    gpio_init(PIN_VSYNC); gpio_set_dir(PIN_VSYNC, GPIO_IN);
    gpio_init(PIN_PCLK);  gpio_set_dir(PIN_PCLK, GPIO_IN);
    
    for(int i=0; i<PIN_RGB_COUNT; i++) {
        gpio_init(PIN_RGB_BASE + i);
        gpio_set_dir(PIN_RGB_BASE + i, GPIO_IN);
        gpio_disable_pulls(PIN_RGB_BASE + i);
        gpio_set_input_hysteresis_enabled(PIN_RGB_BASE + i, true);
    }

    gpio_set_irq_enabled_with_callback(PIN_VSYNC, GPIO_IRQ_EDGE_FALL, true, &vsync_irq_handler);

    // 2. PIO 初始化
    pio_clear_instruction_memory(g_pio);
    g_offset = pio_add_program(g_pio, &video_capture_program);
    g_sm = pio_claim_unused_sm(g_pio, true);
    pio_sm_config c = video_capture_program_get_default_config(g_offset);
    sm_config_set_in_pins(&c, PIN_RGB_BASE);
    sm_config_set_in_shift(&c, false, true, 16); 
    pio_sm_init(g_pio, g_sm, g_offset, &c);
    pio_sm_set_enabled(g_pio, g_sm, true);

    // 3. DMA 初始化
    g_dma_chan = dma_claim_unused_channel(true);
    g_dma_config = dma_channel_get_default_config(g_dma_chan);
    channel_config_set_transfer_data_size(&g_dma_config, DMA_SIZE_16);
    channel_config_set_read_increment(&g_dma_config, false); // 读 PIO 不自增
    channel_config_set_write_increment(&g_dma_config, true); // 写内存自增
    channel_config_set_dreq(&g_dma_config, pio_get_dreq(g_pio, g_sm, false));
    
    // 预先配置一次（但不启动），确保状态正确
    dma_channel_configure(
        g_dma_chan, 
        &g_dma_config, 
        NULL, 
        &g_pio->rxf[g_sm], 
        FRAME_WIDTH, 
        false
    );
}

void video_capture_run(void)
{
    int write_idx = 0;
    // 定义一个丢弃缓冲区，用于消耗掉 Back Porch 的数据
    uint16_t discard_buffer[FRAME_WIDTH]; 

    while (1) {
        // 1. 等待 VSYNC
        while (!g_vsync_detected) { tight_loop_contents(); }
        g_vsync_detected = false;

        write_idx = !write_idx; 

        // 2. 重置 PIO (确保从行头开始)
        pio_sm_set_enabled(g_pio, g_sm, false);
        pio_sm_clear_fifos(g_pio, g_sm);
        pio_sm_restart(g_pio, g_sm);
        pio_sm_exec(g_pio, g_sm, pio_encode_jmp(g_offset));
        pio_sm_set_enabled(g_pio, g_sm, true);

        // 3. 跳过前 18 行 (Back Porch)
        // 使用 DMA 快速读取并丢弃，防止 PIO FIFO 溢出
        for (int i = 0; i < 18; i++) {
             dma_channel_configure(
                g_dma_chan, 
                &g_dma_config, 
                discard_buffer,    // 写入丢弃区
                &g_pio->rxf[g_sm], // 读 PIO
                FRAME_WIDTH,       // 长度
                true               // 立即启动
             );
             dma_channel_wait_for_finish_blocking(g_dma_chan);
        }

        // 4. 采集有效画面 (240行)
        // 直接写入当前帧缓冲区
        uint16_t *base_ptr = g_frame_buf[write_idx];
        
        for (int line = 0; line < FRAME_HEIGHT; line++) {
            dma_channel_configure(
                g_dma_chan,
                &g_dma_config,
                base_ptr + (line * FRAME_WIDTH), // 写入显存位置
                &g_pio->rxf[g_sm],
                FRAME_WIDTH,
                true // 立即启动
            );
            dma_channel_wait_for_finish_blocking(g_dma_chan);
        }

        // 5. 提交给显示端
        g_display_idx = write_idx;
    }
}

uint32_t video_capture_get_frame_count(void) { return 0; }
