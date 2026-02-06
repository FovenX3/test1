#ifndef VIDEO_BUFFERS_H
#define VIDEO_BUFFERS_H

#include <stdint.h>
#include "video_config.h"

// 定义双缓冲：2帧 * 320像素 * 240行 * 2字节(RGB565) = 约300KB RAM
// RP2350B 有 520KB RAM，完全够用
extern uint16_t g_frame_buf[2][FRAME_WIDTH * FRAME_HEIGHT];

// 指向当前主要用于显示的缓冲区索引 (0 或 1)
extern volatile int g_display_idx;

#endif
