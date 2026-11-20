#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_http_client.h"

esp_err_t queue_init(void);
int queue_count(void);
void queue_list(void);
void queue_clear(void);
void queue_log_storage(void);
/* Encode 16k mono PCM to 20kbit/s CBR Ogg Opus, then save atomically. */
esp_err_t queue_save(const uint8_t *pcm, int len, int64_t captured_at);
const char *queue_save_error(void); /* actionable reason; RAM buffer is still retained */
/* 逐个上传队列里的文件；全部处理完返回队列剩余数 */
int queue_flush(void);
int queue_flush_periodic(void);   /* 后台任务调用；请求触发或每 30 秒补传 */
void queue_flush_async(void);     /* 请求后台任务尽快补传，不在调用线程上传 */
/* 声纹样本：先落盘再上传，失败保留重试 */
esp_err_t queue_save_enroll(int slot, const uint8_t *wav, int len);
bool queue_enroll_pending(int slot);
int queue_flush_enroll(void);
/* 全局 HTTP 串行锁（内部 RAM 只够一个 TLS 会话） */
bool http_lock_take(uint32_t timeout_ms);
void http_lock_give(void);
void http_low_latency(esp_http_client_handle_t client);
