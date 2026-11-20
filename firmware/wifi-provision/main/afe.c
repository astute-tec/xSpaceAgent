/*
 * 音频前端（AFE）：单麦降噪 + AGC + 唤醒词
 *
 * 架构（照搬 xiaozhi 的 afe_audio_engine 模式）：
 *   feed 任务：esp_codec_dev_read 读 ES7210 两个有效 TDM slot，取主麦 → AFE
 *   fetch 任务：esp_afe_sr_v1.fetch → 增强后单声道 → 唤醒检测 → 环形队列给录音/问答
 *
 * 当前保守使用 ch0 主麦。ch1 的物理参考语义尚未完成实测，因此不在本次修复中
 * 冒险启用 AEC；录音模式下喇叭保持静音。
 */
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_afe_config.h"
#include "esp_nsn_models.h"
#include "model_path.h"

#include "mic.h"
#include "recorder.h"
#include "afe.h"

static const char *TAG = "afe";

static esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t *s_afe_data;
static RingbufHandle_t s_ring;          /* 增强后的 16k 单声道输出 */
static SemaphoreHandle_t s_reset_done;
static volatile bool s_running;
static volatile bool s_ready;
static volatile bool s_reset_requested;
static volatile uint32_t s_overflows;
static void *s_model_storage; /* AFE/model pointers retain this for app lifetime. */

static bool models_move_to_psram(srmodel_list_t *models)
{
    if (!models || !models->mmap_handle) return true;
    if (models->num <= 0 || !models->model_data) return false;
    size_t bytes=0;
    for (int i=0;i<models->num;i++) {
        srmodel_data_t *m=models->model_data[i];
        if (!m || m->num<0 || !m->files || !m->data || !m->sizes) return false;
        for (int j=0;j<m->num;j++) {
            if (m->sizes[j]<0 || !m->files[j] || !m->data[j]) return false;
            size_t aligned=((size_t)m->sizes[j]+15)&~(size_t)15;
            if (bytes>SIZE_MAX-SRMODEL_STRING_LENGTH ||
                aligned>SIZE_MAX-SRMODEL_STRING_LENGTH-bytes) return false;
            bytes+=SRMODEL_STRING_LENGTH+aligned;
        }
    }
    if (!bytes) return false;
    uint8_t *storage=heap_caps_aligned_alloc(16,bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!storage) return false; /* No model pointers changed on allocation failure. */
    uint8_t *next=storage;
    for (int i=0;i<models->num;i++) {
        srmodel_data_t *m=models->model_data[i];
        for (int j=0;j<m->num;j++) {
            memcpy(next,m->files[j],SRMODEL_STRING_LENGTH);
            m->files[j]=(char *)next;
            next+=SRMODEL_STRING_LENGTH;
            memcpy(next,m->data[j],m->sizes[j]);
            m->data[j]=(char *)next;
            next+=((size_t)m->sizes[j]+15)&~(size_t)15;
        }
    }
    /* Model names/info were separately allocated by ESP-SR. Only files/data
     * pointed into the mapping. Keeping any mmap defeats PSRAM XiP during
     * Flash writes, suspending the capture scheduler despite IRAM-safe I2S. */
    esp_partition_munmap(*(esp_partition_mmap_handle_t *)models->mmap_handle);
    free(models->mmap_handle);
    models->mmap_handle=NULL;
    s_model_storage=storage;
    ESP_LOGI(TAG,"model files moved to PSRAM: %u bytes; Flash mapping released",(unsigned)bytes);
    return true;
}

#define RING_SIZE (MIC_SAMPLE_RATE * 2 * 2)    /* PSRAM：约 2 秒突发缓冲 */
#define LEVEL_HISTORY 96                        /* 96 * 10ms ~= 1 秒 */
static uint16_t s_level_history[LEVEL_HISTORY];
static uint32_t s_level_seq;
static uint32_t s_fetch_seq;
static portMUX_TYPE s_level_mux = portMUX_INITIALIZER_UNLOCKED;

static void drain_output_ring(void)
{
    if (!s_ring) return;
    for (;;) {
        size_t item_size = 0;
        void *item = xRingbufferReceiveUpTo(s_ring, &item_size, 0, RING_SIZE);
        if (!item) break;
        vRingbufferReturnItem(s_ring, item);
    }
}

bool afe_is_ready(void) { return s_ready; }

void afe_log_diagnostics(void)
{
    uint16_t levels[96];
    int n = afe_get_level_history(levels, 96);
    int low = UINT16_MAX, high = 0;
    for (int i = 0; i < n; i++) {
        if (levels[i] < low) low = levels[i];
        if (levels[i] > high) high = levels[i];
    }
    ESP_LOGI(TAG, "health feed=%lu fetch=%lu rms=%d..%d overflow=%lu",
             (unsigned long)s_level_seq, (unsigned long)s_fetch_seq,
             n ? low : 0, high, (unsigned long)s_overflows);
}

int afe_get_level_history(uint16_t *out, int max_items)
{
    if (!out || max_items <= 0) return 0;
    if (max_items > LEVEL_HISTORY) max_items = LEVEL_HISTORY;
    taskENTER_CRITICAL(&s_level_mux);
    uint32_t total = s_level_seq < LEVEL_HISTORY ? s_level_seq : LEVEL_HISTORY;
    if ((uint32_t)max_items > total) max_items = (int)total;
    uint32_t end = s_level_seq;
    for (int i = 0; i < max_items; i++) {
        uint32_t seq = end - (uint32_t)max_items + (uint32_t)i;
        out[i] = s_level_history[seq % LEVEL_HISTORY];
    }
    taskEXIT_CRITICAL(&s_level_mux);
    return max_items;
}

bool afe_reset_output(uint32_t timeout_ms)
{
    if (!s_ready || !s_reset_done) return false;
    while (xSemaphoreTake(s_reset_done, 0) == pdTRUE) {}
    s_reset_requested = true;
    return xSemaphoreTake(s_reset_done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

/* byte_count：字节数（不是采样数），调用方传入的均为字节。
 * 之前参数叫 samples 且内部乘以 sizeof(int16_t)，导致每次读双倍
 * 字节，后半段被下一次调用覆盖，造成音频质量严重下降。*/
bool afe_read(int16_t *dest, int byte_count)
{
    if (!s_ready || !s_ring || !dest || byte_count <= 0) return false;
    size_t need = (size_t)byte_count;   /* 字节数，不再乘以 2 */
    size_t got = 0;
    int wait = 0;
    while (got < need && wait < 4000) {
        size_t item_size;
        uint8_t *data = (uint8_t *)xRingbufferReceiveUpTo(s_ring, &item_size,
                                                          pdMS_TO_TICKS(50), need - got);
        if (data) {
            memcpy((uint8_t *)dest + got, data, item_size);
            vRingbufferReturnItem(s_ring, data);
            got += item_size;
        } else {
            wait += 50;
        }
    }
    if (got < need) {
        memset((uint8_t *)dest + got, 0, need - got);
    }
    return got == need;
}

static void feed_task(void *arg)
{
    const int chunk = s_afe->get_feed_chunksize(s_afe_data);
    int16_t *stereo = heap_caps_malloc(chunk * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    int16_t *mono = heap_caps_malloc(chunk * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!stereo || !mono) { ESP_LOGE(TAG, "feed buf no mem"); vTaskDelete(NULL); return; }
    ESP_LOGI(TAG, "feed ready: chunk=%d (从 2ch 取左声道)", chunk);
    for (;;) {
        int rc = esp_codec_dev_read(mic_input_dev(), stereo, chunk * 2 * sizeof(int16_t));
        if (rc != ESP_CODEC_DEV_OK) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        for (int f = 0; f < chunk; f++) mono[f] = stereo[f * 2];   /* 左声道 */
        int64_t sum_sq = 0;
        for (int f = 0; f < chunk; f++) {
            int32_t sample = mono[f];
            sum_sq += (int64_t)sample * sample;
        }
        uint32_t raw_rms = chunk > 0 ? (uint32_t)sqrtf((float)sum_sq / chunk) : 0;
        if (raw_rms > UINT16_MAX) raw_rms = UINT16_MAX;
        taskENTER_CRITICAL(&s_level_mux);
        s_level_history[s_level_seq % LEVEL_HISTORY] = (uint16_t)raw_rms;
        s_level_seq++;
        taskEXIT_CRITICAL(&s_level_mux);
        s_afe->feed(s_afe_data, mono);
    }
}

static void fetch_task(void *arg)
{
    while (s_running) {
        if (s_reset_requested) {
            s_reset_requested = false;
            drain_output_ring();
            int rc = s_afe->reset_buffer(s_afe_data);
            drain_output_ring();
            ESP_LOGI(TAG, "output reset rc=%d overflow=%lu",
                     rc, (unsigned long)s_overflows);
            xSemaphoreGive(s_reset_done);
            continue;
        }
        afe_fetch_result_t *res = s_afe->fetch(s_afe_data);
        if (!res || res->ret_value != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        /* 1) 唤醒检测（AFE 内置 wakenet） */
        s_fetch_seq++;
        if (res->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "WAKE WORD detected (你好小智)");
            extern void recorder_ask_wake(void);
            recorder_ask_wake();          /* 仅投递事件，不在 fetch 线程中等待自己复位 */
        }
        /* 2) 增强后的单声道音频入环形队列，供录音/问答取用 */
        if (xRingbufferSend(s_ring, res->data, res->data_size, 0) != pdTRUE) {
            size_t old_size = 0;
            void *old = xRingbufferReceiveUpTo(s_ring, &old_size, 0, res->data_size);
            if (old) vRingbufferReturnItem(s_ring, old);
            s_overflows++;
            if (xRingbufferSend(s_ring, res->data, res->data_size, 0) != pdTRUE &&
                (s_overflows % 100) == 1) {
                ESP_LOGW(TAG, "output ring full, overflow=%lu", (unsigned long)s_overflows);
            }
        }
    }
    vTaskDelete(NULL);
}

esp_err_t afe_init(void)
{
    /* 用官方 afe_config_init 生成默认配置，再按板子调整
     * 输入格式 "M" = 单麦克风、无回采参考（这块板的双麦在 ES7210 内部混合成
     * 一个通道输出，两个 TDM slot 是同一信号——不存在独立参考通道）。
     * 不开 AEC：参考=语音本身会把语音当回声消掉。设备录音时不播放音频，无回声。 */
    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models_move_to_psram(models)) {
        ESP_LOGE(TAG,"cannot move wake model to PSRAM; AFE not started");
        return ESP_ERR_NO_MEM;
    }
    char *wake_model = models ? esp_srmodel_filter(models, "wn", "nihaoxiaozhi") : NULL;
    ESP_LOGI(TAG, "wake model=%s", wake_model ? wake_model : "(missing)");
    afe_type_t type = AFE_TYPE_SR;
    afe_mode_t mode = AFE_MODE_LOW_COST;
    afe_config_t *cfg = afe_config_init("M", models, type, mode);
    if (!cfg) {
        ESP_LOGE(TAG, "afe_config_init failed");
        return ESP_FAIL;
    }
    cfg->aec_init = false;
    cfg->wakenet_init = wake_model != NULL;
    cfg->wakenet_model_name = wake_model;
    cfg->ns_init = true;
    cfg->vad_init = false;   /* 远场低 SNR 时 VAD 会门掉整段，先关 */
    cfg->agc_init = true;                   /* 远场小声自动增益（核心远场增益来源） */
    cfg->agc_compression_gain_db = 12;
    /* Audio enhancement must outrank encoding/UI and avoid core0's WiFi/TLS
     * bursts. The default internal task can otherwise starve with FEED full. */
    cfg->afe_perferred_core = 1;
    cfg->afe_perferred_priority = 9;
    cfg->afe_ringbuf_size = 100;
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    s_afe = (esp_afe_sr_iface_t *)esp_afe_handle_from_config(cfg);
    s_afe_data = s_afe->create_from_config(cfg);
    if (!s_afe_data) {
        ESP_LOGE(TAG, "afe create failed");
        return ESP_FAIL;
    }
    s_ring = xRingbufferCreateWithCaps(RING_SIZE, RINGBUF_TYPE_BYTEBUF,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring) return ESP_ERR_NO_MEM;
    s_reset_done = xSemaphoreCreateBinary();
    if (!s_reset_done) return ESP_ERR_NO_MEM;
    s_running = true;
    /* Capture outranks enhancement and leaves WiFi/lwIP on core0. The enlarged
     * AFE input ring absorbs short catch-up bursts after Flash writes. */
    if (xTaskCreatePinnedToCore(feed_task, "afe_feed", 4096, NULL, 12, NULL, 1) != pdPASS)
        return ESP_FAIL;
    if (xTaskCreatePinnedToCore(fetch_task, "afe_fetch", 8192, NULL, 10, NULL, 1) != pdPASS)
        return ESP_FAIL;
    s_ready = true;
    s_afe->print_pipeline(s_afe_data);
    ESP_LOGI(TAG, "AFE ready: feed12/fetch10/enhance9 on core1, mono+ns+agc+wakenet (aec off), %d Hz, ring=%d bytes",
             MIC_SAMPLE_RATE, RING_SIZE);
    return ESP_OK;
}
