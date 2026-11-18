#pragma once
#include "driver/gpio.h"

/* Waveshare ESP32-S3-Touch-AMOLED-1.75C */
#define LCD_H_RES 466
#define LCD_V_RES 466

#define PIN_LCD_CS    12
#define PIN_LCD_PCLK  38
#define PIN_LCD_D0    4
#define PIN_LCD_D1    5
#define PIN_LCD_D2    6
#define PIN_LCD_D3    7
#define PIN_LCD_RST   1

#define PIN_I2C_SDA   15
#define PIN_I2C_SCL   14
#define PIN_TOUCH_RST 2
#define PIN_TOUCH_INT 11
#define PIN_PA        46

#define AUDIO_MCLK 16
#define AUDIO_BCLK 9
#define AUDIO_WS   45
#define AUDIO_DIN  10
#define AUDIO_DOUT 8
#define AUDIO_PA   46
#define REC_BUTTON GPIO_NUM_0

#define AP_SSID_PREFIX "Xiaozhi"
#define AP_IP_STR      "192.168.4.1"
