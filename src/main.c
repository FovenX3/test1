#include "pico_hdmi/hstx_data_island_queue.h"
#include "pico_hdmi/video_output.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include <stdio.h>
#include <string.h>

#include "video/video_config.h"
#include "video/video_pipeline.h"
#include "video_capture.h"
#include "video/video_buffers.h" 

// --- 全局变量定义 ---
// 分配在 RAM 中的帧缓冲区 (RP2350 专用)
uint16_t g_frame_buf[2][FRAME_WIDTH * FRAME_HEIGHT];
volatile int g_display_idx = 0;

int main(void)
{
    // 【关键修正】设置系统时钟为 126 MHz
    // RP2350 HSTX HDMI 库通常基于 126 MHz 时钟生成 640x480 @ 60Hz 时序 (25.2 MHz Pixel Clock)
    // 之前设置 252 MHz 导致输出频率错误，显示器提示不支持
    set_sys_clock_khz(126000, true);

    stdio_init_all();
    sleep_ms(1000);

    // 清空缓冲区
    memset(g_frame_buf, 0, sizeof(g_frame_buf));

    // 初始化 HDMI 队列与管道
    hstx_di_queue_init();
    
    // 初始化视频管道 (输出 640x480)
    video_pipeline_init(FRAME_WIDTH, FRAME_HEIGHT);

    // 初始化采集 (GPIO, PIO, DMA)
    video_capture_init(MVS_HEIGHT);

    // 启动 Core 1 运行 HDMI 输出线程
    multicore_launch_core1(video_output_core1_run);
    
    sleep_ms(100);

    // Core 0 运行视频采集
    video_capture_run();

    return 0;
}
