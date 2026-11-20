#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t afe_init(void);
/* 从 AFE 输出队列读增强后的 16k 单声道 PCM（阻塞，最多 ~4 秒）
 * byte_count：要读取的字节数（不是采样数）*/
bool afe_read(int16_t *dest, int byte_count);
/* Stop waiting within one 50ms ring wait when a question is submitted/cancelled. */
bool afe_read_while(int16_t *dest, int byte_count, const volatile bool *active);
/* 丢弃空闲期积压的历史音频，并重置 AFE 内部缓冲。
 * 每次开始录音、问答、按住说话或声纹采集前调用。 */
bool afe_reset_output(uint32_t timeout_ms);
bool afe_is_ready(void);
/* 获取最近约 1 秒的原始麦克风 RMS（每项约 10ms，返回实际数量）。 */
int afe_get_level_history(uint16_t *out, int max_items);
void afe_log_diagnostics(void);
