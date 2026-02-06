#include "video_pipeline.h"
#include <string.h>
#include <stdint.h> // 关键修复：添加标准整数类型定义
#include "pico.h"
#include "pico/stdlib.h"
#include "pico_hdmi/video_output.h"
#include "video_config.h"
#include "video_buffers.h" 

// 快速像素倍增函数 (内联优化)
static inline void __attribute__((always_inline)) double_pixels_fast(uint32_t *dst, const uint16_t *src, int width)
{
    // 输入 src 是 16位 (RGB565)
    // 输出 dst 是 32位 (包含两个像素)
    // 我们的目标是将 320 个像素扩展为 640 个像素。
    
    for (int i = 0; i < width; i++) {
        uint32_t p = src[i];
        // 将1个像素复制两遍，拼成一个32位整数 (低16位=P, 高16位=P)
        dst[i] = (p << 16) | p;
    }
}

/**
 * 扫描线回调 - 由 HDMI 库 Core 1 调用
 * 签名匹配：void (*)(uint32_t, uint32_t, uint32_t *)
 */
void __time_critical_func(video_pipeline_scanline_callback)(uint32_t v_scanline, uint32_t active_line, uint32_t *dst)
{
    (void)v_scanline;

    // 1. 计算源行号 (320 -> 640, 所以除以 2)
    // 输入缓冲区是 240 行，输出是 480 行，所以除以 2
    uint32_t y_src = active_line >> 1;

    // 2. 边界检查
    if (y_src >= FRAME_HEIGHT) {
        // 如果越界，输出黑色。注意 dst 是 32位指针，长度是 640/2 = 320 个字
        memset(dst, 0, 640 * 2); 
        return;
    }

    // 3. 从双缓冲读取
    // 读取当前 Core 0 已经写好的那一帧 (g_display_idx)
    const uint16_t *src_row = &g_frame_buf[g_display_idx][y_src * FRAME_WIDTH];

    // 4. 像素倍增 (320 -> 640)
    double_pixels_fast(dst, src_row, FRAME_WIDTH);
}

void video_pipeline_init(uint32_t frame_width, uint32_t frame_height)
{
    // 初始化 HDMI 输出 (标准 VGA 640x480)
    // 库函数需要明确的分辨率参数
    video_output_init(640, 480);

    // 注册回调函数
    video_output_set_scanline_callback(video_pipeline_scanline_callback);
}
