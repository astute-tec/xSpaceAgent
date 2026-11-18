#pragma once
#include "esp_err.h"
#include <stddef.h>

esp_err_t queue_init(void);
int queue_count(void);
void queue_list(void);
void queue_clear(void);
/* 把 AFE 输出的 16k 单声道 PCM 原子保存为 WAV。 */
esp_err_t queue_save(const uint8_t *pcm, int len);
/* 逐个上传队列里的文件；全部处理完返回队列剩余数 */
int queue_flush(void);
int queue_flush_periodic(void);   /* 后台任务调用；请求触发或每 30 秒补传 */
void queue_flush_async(void);     /* 请求后台任务尽快补传，不在调用线程上传 */
/* 声纹样本：先落盘再上传，失败保留重试 */
esp_err_t queue_save_enroll(int slot, const uint8_t *wav, int len);
void queue_remove_enroll(int slot);
int queue_flush_enroll(void);
