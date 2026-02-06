#ifndef HARDWARE_CONFIG_H
#define HARDWARE_CONFIG_H

#include <stdint.h>

// =============================================================================
// Pin Definitions (RP2350B / Pico 2)
// =============================================================================

// 同步信号输入
#define PIN_HSYNC       0   // 对应 LCD Pin 33
#define PIN_VSYNC       1   // 对应 LCD Pin 34
#define PIN_PCLK        2   // 对应 LCD Pin 35

// RGB 数据输入 (16位连续)
// 接线映射 (RGB565格式):
// GPIO 20-24 (5 bit) <--- LCD Blue (B3-B7)  [GPIO20=B3 ... GPIO24=B7]
// GPIO 25-30 (6 bit) <--- LCD Green (G2-G7) [GPIO25=G2 ... GPIO30=G7]
// GPIO 31-35 (5 bit) <--- LCD Red   (R3-R7) [GPIO31=R3 ... GPIO35=R7]
#define PIN_RGB_BASE    20
#define PIN_RGB_COUNT   16

// =============================================================================
// Pixel Format Helper
// =============================================================================
// 由于我们通过硬件接线直接匹配了 RGB565 的位序 (LSB at GPIO 20, MSB at GPIO 35)
// 所以这里不需要软件转换，直接返回原始数据即可。
static inline uint16_t extract_pixel(uint32_t raw_val)
{
    // 硬件直接采集到的就是标准的 RGB565
    return (uint16_t)raw_val;
}

#endif // HARDWARE_CONFIG_H
