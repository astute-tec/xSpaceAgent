#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

/* 466x466 RGB565 全屏缓冲（PSRAM） */
uint16_t *display_fb(void);
i2c_master_bus_handle_t display_i2c_bus(void);
esp_err_t display_init(void);
void display_flush(void);
void display_fill(uint16_t color);
void display_brightness(uint8_t level);
bool display_frame_take(int wait_ms);
void display_frame_give(void);

/* 简单 UI 原语（供 recorder 使用） */
void ui_fill_rect(int x, int y, int w, int h, uint16_t color);
void ui_number(int value, int digits, int y, int dw, int dh, int gap, int t, uint16_t color);
