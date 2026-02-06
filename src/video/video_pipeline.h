#ifndef VIDEO_PIPELINE_H
#define VIDEO_PIPELINE_H

#include <stdint.h> // 确保这里有这一行
#include <stdbool.h>

void video_pipeline_init(uint32_t frame_width, uint32_t frame_height);
void video_pipeline_scanline_callback(uint32_t v_scanline, uint32_t active_line, uint32_t *dst);

#endif
