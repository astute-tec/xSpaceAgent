/* 麦克风采集：ES7210 + I2S TDM（配置照搬小智该板 BSP） */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_codec_dev.h"

#define MIC_SAMPLE_RATE 16000
#define MIC_CHANNELS    2
#define MIC_BYTES_PER_SEC (MIC_SAMPLE_RATE * MIC_CHANNELS * 2)

esp_err_t mic_init(void);
/* 读满 len 字节，返回实际读取的字节数；失败返回 <0 */
int mic_read(void *buf, int len);
esp_codec_dev_handle_t mic_input_dev(void);
/* 播放 24k/2ch s16le PCM（阻塞） */
int mic_play(const void *buf, int len);
void mic_set_playback_vol(int vol);
