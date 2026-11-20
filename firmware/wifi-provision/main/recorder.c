/*
 * 录音 / 语音问答 / 按住说话 —— ESP32-S3 语音工作助手
 *
 * 交互总览：
 *   上面键短按       开始录音（录音中再按无效；停止靠下面键长按）
 *   下面键短按       熄屏 / 亮屏（录音不受影响）
 *   下面键长按 3 秒  保存尾段并停止录音
 *   屏幕轻点         唤醒屏幕（不改变录音）；双击进入问答
 *   屏幕按住 >0.26s  开始聆听；松开继续听，上键单击提交给唐僧
 *   屏幕顶部下滑     打开设置菜单（WiFi / 声纹）
 *   喊"你好小智"     唤醒问答（AFE 内置唤醒）
 */
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_wifi.h"
#include "cJSON.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "board.h"
#include "display.h"
#include "mic.h"
#include "afe.h"
#include "touch.h"
#include "queue.h"
#include "menu.h"
#include "font_cn.h"
#include "recorder.h"
#include "watchface.h"
#include "recording_codec.h"
#include "audio_playback.h"

static const char *TAG = "rec";

/* ---------------- 常量 ---------------- */

#define OUT_SAMPLE_RATE   16000
#define OUT_BYTES_PER_SEC (OUT_SAMPLE_RATE * 2)     /* 16k 单声道 16bit */
#define SEG_SECONDS       20
#define SEG_BYTES         (OUT_BYTES_PER_SEC * SEG_SECONDS)
#define ASK_SECONDS       30
#define ASK_BYTES         (OUT_BYTES_PER_SEC * ASK_SECONDS)
#define SILENCE_RMS       60                        /* 低于此值视为无有效语音 */

/* ---------------- 状态 ---------------- */

static uint8_t *s_seg[2];               /* 录音双缓冲 */
static int s_len[2];
static volatile bool s_buf_busy[2];
static int s_cur;
static volatile int s_total;
static volatile bool s_recording;
static volatile bool s_busy;             /* 收尾中 */
static volatile bool s_asking;           /* 问答中 */
static volatile bool s_playing;
static volatile bool s_ask_capturing;
static volatile bool s_ask_sending;
static volatile watchface_state_t s_ask_phase = WATCHFACE_THINKING;
static volatile bool s_wake_requested;
static volatile int64_t s_error_until_ms;
static volatile bool s_auto = true;      /* 全天自动采集（默认开，recorder_init 从 NVS 恢复） */
static volatile bool s_screen_off;
static volatile bool s_manual_screen_off;
static volatile bool s_pending_start;
static volatile bool s_ask_pending;
static volatile bool s_ask_cancel;   /* 下面键取消问答：丢弃在途请求与回答 */
static volatile bool s_resume_after_ask;
static volatile bool s_user_stopped;
static volatile bool s_ptt_pending;
static volatile bool s_ptt_user_down;

static uint8_t *s_askbuf;                /* 问题 / 声纹样本缓冲 */

static char s_hub_url[192] = "http://192.168.2.27:8765/upload";
static char s_token[80];
static char s_ask_url[192];
static char s_ask_url_buf[192];

/* 按住说话状态 */
static volatile bool s_ptt_active;
static volatile int s_ptt_len;
static volatile int s_stop_hold_progress;
static volatile bool s_top_ptt_session;
static volatile bool s_ptt_capture_running;
static portMUX_TYPE s_top_click_mux = portMUX_INITIALIZER_UNLOCKED;
static int s_top_clicks;
static int64_t s_top_last_ms;

typedef struct { int idx; int len; int64_t captured_at; } save_msg_t;
static QueueHandle_t s_saveq;
static i2c_master_dev_handle_t s_axp_dev;
static volatile bool s_initialized;
static volatile bool s_available;

void recorder_toggle_screen(void);
static void flush_task(void *arg);
static void recorder_ui_task(void *arg);
static void ptt_start_after_record(void);
static void ask_resume_recording(void);
static void show_playing(void);
static void show_thinking(void);
static void show_asking(void);
static void touch_task(void *arg);
static void sync_voiceprint_slots(void);
static void recorder_start_impl(bool during_question);
extern const uint8_t no_speech_start[] asm("_binary_no_speech_pcm_start");
extern const uint8_t no_speech_end[] asm("_binary_no_speech_pcm_end");
extern const uint8_t ask_failed_start[] asm("_binary_ask_failed_pcm_start");
extern const uint8_t ask_failed_end[] asm("_binary_ask_failed_pcm_end");

/* 两条本地语音提示，离线也能明确反馈；只在已停止采集后播放。 */
static void play_question_prompt(bool no_speech)
{
    const uint8_t *p = no_speech ? no_speech_start : ask_failed_start;
    const uint8_t *end = no_speech ? no_speech_end : ask_failed_end;
    s_playing = true;
    mic_set_playback_vol(100);
    while (p < end) {
        int n = end - p > 6400 ? 6400 : (int)(end - p);
        if (mic_play(p, n) != n) break;
        p += n;
    }
    mic_set_playback_vol(0);
    s_playing = false;
    ESP_LOGI(TAG, "local voice prompt=%s", no_speech ? "no_speech" : "ask_failed");
}

static void no_speech_task(void *arg)
{
    play_question_prompt(true);
    s_asking = false;
    ask_resume_recording();
    vTaskDelete(NULL);
}

static bool question_has_voice(const uint8_t *pcm, int bytes)
{
    if (bytes < OUT_BYTES_PER_SEC / 2) return false;
    int active = 0;
    for (int offset = 0; offset + 640 <= bytes; offset += 640) {
        const int16_t *frame = (const int16_t *)(pcm + offset);
        int64_t sum = 0;
        for (int i = 0; i < 320; i++) sum += (int64_t)frame[i] * frame[i];
        if (sum >= 320 * 60 * 60) active++;
    }
    return active >= 8; /* 至少 160ms 明确有声；服务端仍负责最终无语音判断。 */
}

extern void display_idle_ui(void);
extern bool prov_ui_active(void);
extern void ui_fill_rect(int x, int y, int w, int h, uint16_t color);
extern void ui_number(int value, int digits, int y, int dw, int dh, int gap, int t, uint16_t color);

/* ---------------- 屏幕控制 ---------------- */

static void screen_wake(void)
{
    if (s_screen_off) { s_screen_off = false; display_brightness(0xC0); }
}

static void screen_dim(void)
{
    if (!s_screen_off) { s_screen_off = true; display_brightness(0); }
}

/* ---------------- 通用绘图 ---------------- */

/* ---------------- 界面 ---------------- */

static void show_idle(void)
{
    screen_wake();
    if (menu_active() || prov_ui_active()) return;
    if (!display_frame_take(100)) return;
    watchface_draw(WATCHFACE_IDLE, NULL, 0, 0);
    display_frame_give();
}

static void show_recording(void)
{
    screen_wake();
    if (menu_active() || prov_ui_active()) return;
    if (!display_frame_take(100)) return;
    watchface_draw(WATCHFACE_RECORDING, NULL, 0, 0);
    display_frame_give();
}

static void recorder_ui_task(void *arg)
{
    uint16_t levels[96];
    int64_t next_status=0;
    for (;;) {
        int64_t now=esp_timer_get_time()/1000;
        if (now>=next_status) {
            next_status=now+3000;
            extern bool prov_is_connected(void);
            wifi_ap_record_t ap={0};
            bool connected=prov_is_connected() && esp_wifi_sta_get_ap_info(&ap)==ESP_OK;
            display_power_t power={0};
            display_read_power(&power);
            watchface_set_status(connected,connected ? ap.rssi : -127,
                                 power.valid && power.battery_present ? power.percent : -1,
                                 power.valid && power.charging,power.valid && power.external_power);
        }
        bool animate = s_recording || s_ptt_active || s_ask_capturing ||
                       s_stop_hold_progress > 0 || watchface_has_answer();
        if (!s_screen_off && !menu_active() && !prov_ui_active() && display_frame_take(0)) {
            watchface_set_recording(s_recording);
            watchface_state_t state = s_stop_hold_progress > 0 ? WATCHFACE_STOPPING
                : ((s_ptt_active || s_ask_capturing) ? WATCHFACE_LISTENING
                : (s_playing ? WATCHFACE_PLAYING
                : (s_error_until_ms > esp_timer_get_time() / 1000 ? WATCHFACE_ERROR
                : (s_ask_sending ? WATCHFACE_SENDING
                : (s_asking ? s_ask_phase
                : (s_recording ? WATCHFACE_RECORDING : WATCHFACE_IDLE))))));
            int count = 0;
            if (state == WATCHFACE_RECORDING || state == WATCHFACE_LISTENING)
                count = afe_get_level_history(levels, (int)(sizeof(levels) / sizeof(levels[0])));
            watchface_draw(state, levels, count, s_stop_hold_progress);
            display_frame_give();
        }
        vTaskDelay(pdMS_TO_TICKS(animate ? 160 : 1000));
    }
}

/* 按住说话界面 */
static void show_ptt(int held_ms)
{
    if (!display_frame_take(0)) return;
    if (menu_active() || prov_ui_active()) { display_frame_give(); return; }
    uint16_t levels[96];
    int count = afe_get_level_history(levels, (int)(sizeof(levels) / sizeof(levels[0])));
    watchface_draw(WATCHFACE_LISTENING, levels, count, 0);
    display_frame_give();
}

/* 采集问题界面 */
static void show_asking(void)
{
    if (!display_frame_take(100)) return;
    if (menu_active() || prov_ui_active()) { display_frame_give(); return; }
    watchface_draw(WATCHFACE_LISTENING, NULL, 0, 0);
    display_frame_give();
}

static void show_thinking(void)
{
    if (!display_frame_take(100)) return;
    if (menu_active() || prov_ui_active()) { display_frame_give(); return; }
    watchface_draw(WATCHFACE_THINKING, NULL, 0, 0);
    display_frame_give();
}

/* 播放回答界面 */
static void show_playing(void)
{
    if (!display_frame_take(100)) return;
    if (menu_active() || prov_ui_active()) { display_frame_give(); return; }
    watchface_draw(WATCHFACE_PLAYING, NULL, 0, 0);
    display_frame_give();
}

/* ---------------- 写盘任务 ---------------- */

static void save_task(void *arg)
{
    save_msg_t m;
    for (;;) {
        if (xQueueReceive(s_saveq, &m, portMAX_DELAY) != pdTRUE) continue;
        esp_err_t err;
        while ((err=queue_save(s_seg[m.idx], m.len, m.captured_at)) != ESP_OK) {
            ESP_LOGE(TAG, "segment save failed: idx=%d len=%d err=%d; retaining RAM buffer", m.idx, m.len, err);
            s_recording=false;
            watchface_set_answer("录音保存失败，请联网补传并检查剩余空间");
            s_error_until_ms=esp_timer_get_time()/1000+12000;
            queue_flush_async();
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
        s_buf_busy[m.idx] = false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static bool submit_segment(int idx, int len)
{
    extern bool prov_clock_is_synced(void);
    time_t now=time(NULL);
    save_msg_t m = { .idx = idx, .len = len,
        .captured_at = prov_clock_is_synced() && now >= 1700000000
            ? (int64_t)now-len/OUT_BYTES_PER_SEC : 0 };
    s_buf_busy[idx] = true;
    if (xQueueSend(s_saveq, &m, pdMS_TO_TICKS(1000)) != pdTRUE) {
        s_buf_busy[idx] = false;
        ESP_LOGE(TAG, "save queue full: idx=%d len=%d; stop to prevent overwrite", idx, len);
        return false;
    }
    return true;
}

/* ---------------- 录音任务 ---------------- */

static void record_task(void *arg)
{
    int stalls = 0;
    int64_t last_speech = 0;

    while (s_recording) {
        int want = OUT_BYTES_PER_SEC / 10;
        int got = afe_read((int16_t *)(s_seg[s_cur] + s_len[s_cur]), want) ? want : -1;
        if (got <= 0) {
            if (++stalls >= 10) {
                ESP_LOGW(TAG, "mic stalled, stop recording");
                s_recording = false;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        stalls = 0;
        /* Consume AFE during playback; do not save our own speaker echo.
         * True acoustic full duplex still requires verified AEC. */
        if (s_playing) continue;
        {
            int frames = got / 2;
            int16_t *p = (int16_t *)(s_seg[s_cur] + s_len[s_cur]);
            int64_t acc = 0;
            for (int f = 0; f < frames; f++) {
                int32_t v = p[f];
                acc += v * v;
            }
            int rms = frames ? (int)sqrtf((float)acc / frames) : 0;

            int64_t now = esp_timer_get_time() / 1000;
            if (rms >= SILENCE_RMS) last_speech = now;
            /* 录音时：有声→亮屏显示声波；静音 10 秒→熄屏（录音继续，不受影响） */
            if (!menu_active() && !prov_ui_active() && !s_asking &&
                !watchface_has_answer() && !s_manual_screen_off) {
                bool want_screen = now - last_speech < 10000;
                if (want_screen && s_screen_off) screen_wake();
                else if (!want_screen && !s_screen_off) screen_dim();
            }
        }
        s_len[s_cur] += got;
        s_total += got;

        if (s_len[s_cur] >= SEG_BYTES) {
            int next = 1 - s_cur;
            int guard = 0;
            while (s_buf_busy[next] && guard < 200) {
                vTaskDelay(pdMS_TO_TICKS(100));
                guard++;
            }
            if (s_buf_busy[next]) {
                ESP_LOGE(TAG, "next record buffer still busy; stop to preserve queued audio");
                s_recording = false;
                break;
            }
            if (!submit_segment(s_cur, s_len[s_cur])) {
                s_recording = false;
                break;
            }
            s_cur = next;
            s_len[s_cur] = 0;
        }
    }

    /* 尾段 */
    if (s_len[s_cur] > 0) {
        if (!s_buf_busy[s_cur]) submit_segment(s_cur, s_len[s_cur]);
        int guard = 0;
        while (s_buf_busy[s_cur] && guard < 300) {
            vTaskDelay(pdMS_TO_TICKS(100));
            guard++;
        }
    }
    for (int idx = 0; idx < 2; idx++) {
        int guard = 0;
        while (s_buf_busy[idx] && guard++ < 300) vTaskDelay(pdMS_TO_TICKS(100));
        if (s_buf_busy[idx]) ESP_LOGE(TAG, "buffer %d save timeout; keeping recorder busy state", idx);
    }
    ESP_LOGI(TAG, "record stop: total %d bytes (%.1f s)", s_total, (float)s_total / OUT_BYTES_PER_SEC);
    s_len[0] = s_len[1] = 0;
    queue_flush_async();
    s_busy = false;
    if (s_ask_pending) {
        s_ask_pending = false;
        recorder_ask();
    } else if (s_ptt_pending) {
        ptt_start_after_record();
    } else {
        show_idle();
    }
    if (s_pending_start && !s_asking) {
        s_pending_start = false;
        vTaskDelay(pdMS_TO_TICKS(200));
        recorder_start();
    }
    vTaskDelete(NULL);
}

/* ---------------- 录音控制 ---------------- */

void recorder_start(void)
{
    recorder_start_impl(false);
}

static void recorder_start_impl(bool during_question)
{
    if (!s_available || !afe_is_ready()) {
        ESP_LOGE(TAG, "recorder unavailable");
        return;
    }
    if (s_recording) {
        ESP_LOGI(TAG, "already recording, ignore start");
        return;
    }
    if (s_buf_busy[0] || s_buf_busy[1]) {
        ESP_LOGW(TAG,"pending unsaved audio; start refused to preserve RAM buffers");
        return;
    }
    if (s_busy || (s_asking && !during_question) ||
        (during_question && (s_user_stopped || s_ptt_active || s_ptt_capture_running || s_playing))) {
        ESP_LOGW(TAG, "busy");
        return;
    }
    if (!afe_reset_output(1200)) {
        ESP_LOGE(TAG, "AFE reset timeout, recording not started");
        return;
    }
    s_len[0] = s_len[1] = 0;
    s_total = 0;
    s_cur = 0;
    s_buf_busy[0] = s_buf_busy[1] = false;
    s_recording = true;
    s_busy = true;
    if (!during_question) {
        screen_wake();
        show_recording();
    }
    ESP_LOGI(TAG, "record start (20s 分段落盘)");
    if (xTaskCreate(record_task, "rec", 6144, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "record task create failed");
        s_recording = false;
        s_busy = false;
        show_idle();
    }
}

/* 停止录音（CLI 调试用） */
void recorder_stop(void)
{
    s_user_stopped = true;
    s_resume_after_ask = false;
    s_pending_start = false;
    if (s_recording) s_recording = false;
}

/* 上面键双击 → 语音问答（等同"你好小智"唤醒） */
static void top_click_task(void *arg)
{
    int64_t quiet_ms;
    do {
        taskENTER_CRITICAL(&s_top_click_mux);
        quiet_ms = esp_timer_get_time() / 1000 - s_top_last_ms;
        taskEXIT_CRITICAL(&s_top_click_mux);
        if (quiet_ms < 400) vTaskDelay(pdMS_TO_TICKS(20));
    } while (quiet_ms < 400);

    taskENTER_CRITICAL(&s_top_click_mux);
    int clicks = s_top_clicks;
    s_top_clicks = 0;
    taskEXIT_CRITICAL(&s_top_click_mux);
    if (clicks >= 2) {
        ESP_LOGI(TAG, "top key DOUBLE -> voice ask listen");
        recorder_ask();
    } else if (clicks == 1) {
        ESP_LOGI(TAG, "top key SINGLE -> start recording");
        recorder_request_start();
    }
    vTaskDelete(NULL);
}

/* 上面键按下（AXP IRQ）统一入口 */
void recorder_top_key(void)
{
    /* 1) 任何设置页面：先回主界面 */
    extern bool menu_active(void);
    extern void menu_close(void);
    if (menu_active()) {
        taskENTER_CRITICAL(&s_top_click_mux);
        s_top_clicks = 0;
        taskEXIT_CRITICAL(&s_top_click_mux);
        ESP_LOGI(TAG, "top key -> close settings, back to main");
        menu_close();
        return;
    }
    if (s_top_ptt_session) {
        s_top_ptt_session = false;
        ESP_LOGI(TAG, "top key -> submit voice question");
        recorder_ptt_end(true);
        return;
    }
    /* 录音中单击无效果，但双击仍可像唤醒词一样开始问答。 */
    if (s_asking || (s_busy && !s_recording)) {
        ESP_LOGI(TAG, "top key ignored (busy)");
        return;
    }
    int64_t now = esp_timer_get_time() / 1000;
    bool create_task = false;
    taskENTER_CRITICAL(&s_top_click_mux);
    if (now - s_top_last_ms > 600) s_top_clicks = 0;
    create_task = s_top_clicks == 0;
    s_top_last_ms = now;
    s_top_clicks++;
    taskEXIT_CRITICAL(&s_top_click_mux);
    if (create_task && xTaskCreate(top_click_task, "topkey", 4096, NULL, 6, NULL) != pdPASS) {
        taskENTER_CRITICAL(&s_top_click_mux);
        s_top_clicks = 0;
        taskEXIT_CRITICAL(&s_top_click_mux);
        ESP_LOGE(TAG, "top key classifier task create failed");
    }
}

/* 上面键短按入口：空闲→立即开始；录音中→忽略；收尾中→挂起 */
void recorder_request_start(void)
{
    s_user_stopped = false;
    if (s_recording) {
        ESP_LOGI(TAG, "already recording, ignore");
        return;
    }
    if (s_busy || s_asking) {
        ESP_LOGI(TAG, "busy, pending start");
        s_pending_start = true;
        return;
    }
    watchface_set_answer("");
    screen_wake();
    recorder_start();
}

void recorder_auto_set(bool on)
{
    s_auto = on;
    nvs_handle_t h;
    if (nvs_open("prov", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "auto", on ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "auto mode = %d", on);
}

bool recorder_auto_enabled(void) { return s_auto; }
bool recorder_is_busy(void) { return s_recording || s_busy || s_asking; }
bool recorder_question_active(void) { return s_asking || s_ptt_pending; }

const char *recorder_state(void)
{
    return s_recording ? "recording"
         : (s_busy ? "uploading"
         : (s_asking ? "asking" : "idle"));
}

void recorder_log_diagnostics(void)
{
    ESP_LOGI(TAG, "health recording=%d asking=%d listening=%d capture_running=%d question=%d bytes heap=%u screen_off=%d manual_off=%d answer_text=%d",
             s_recording, s_asking, s_ptt_active, s_ptt_capture_running, s_ptt_len,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), s_screen_off,
             s_manual_screen_off, watchface_has_answer());
    afe_log_diagnostics();
    display_log_diagnostics();
    queue_log_storage();
}

const char *recorder_upload_url(void) { return s_hub_url; }
const char *recorder_upload_token(void) { return s_token; }

/* ---------------- 离线队列补传 ---------------- */

static void sync_voiceprint_slots(void)
{
    static int64_t next_sync;
    int64_t now = esp_timer_get_time() / 1000;
    extern bool prov_is_connected(void);
    if (now < next_sync || !prov_is_connected() || recorder_is_busy() ||
        menu_voice_capture_active()) return;
    next_sync = now + 60000;
    char url[192];
    snprintf(url, sizeof(url), "%s", s_hub_url);
    char *cut = strstr(url, "/v1/upload");
    if (!cut) cut = strstr(url, "/upload");
    if (!cut || !http_lock_take(0)) return;
    snprintf(cut, sizeof(url) - (cut - url), "/v1/voiceprints");
    esp_http_client_config_t cfg = {.url = url, .timeout_ms = 8000,
        .crt_bundle_attach = esp_crt_bundle_attach};
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli) {
        char auth[110];
        snprintf(auth, sizeof(auth), "Bearer %s", s_token);
        esp_http_client_set_header(cli, "Authorization", auth);
        if (esp_http_client_open(cli, 0) == ESP_OK &&
            esp_http_client_fetch_headers(cli) >= 0 &&
            esp_http_client_get_status_code(cli) == 200) {
            char body[768] = {0};
            int n = esp_http_client_read_response(cli, body, sizeof(body) - 1);
            if (n > 0 && esp_http_client_is_complete_data_received(cli)) {
                body[n] = 0;
                cJSON *json = cJSON_ParseWithOpts(body, NULL, true);
                const cJSON *slots = cJSON_GetObjectItemCaseSensitive(json, "slots");
                if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "ok")) && cJSON_IsArray(slots)) {
                    const cJSON *entry;
                    cJSON_ArrayForEach(entry, slots) {
                        const cJSON *id = cJSON_GetObjectItemCaseSensitive(entry, "slot");
                        const cJSON *name = cJSON_GetObjectItemCaseSensitive(entry, "name");
                        if (cJSON_IsNumber(id) && id->valuedouble == id->valueint &&
                            id->valueint >= 0 && id->valueint < 5 &&
                            cJSON_IsString(name) && name->valuestring[0] &&
                            strlen(name->valuestring) < 32 && !queue_enroll_pending(id->valueint) &&
                            strcmp(menu_get_slot(id->valueint), name->valuestring)) {
                            menu_confirm_enroll(id->valueint, name->valuestring);
                        }
                    }
                }
                cJSON_Delete(json);
            }
        }
        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
    }
    http_lock_give();
}

static void flush_task(void *arg)
{
    for (;;) {
        sync_voiceprint_slots();
        /* 声纹样本若上传失败，这里自动重试 */
        int vp = queue_flush_enroll();
        if (vp > 0) ESP_LOGI(TAG, "voiceprint: %d 个样本待重试", vp);
        int left = queue_flush_periodic();
        if (left > 0) ESP_LOGI(TAG, "queue: %d 段待补传", left);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* ---------------- 语音问答 ---------------- */

/* 发送已录好的问题并播放回答 */
extern bool prov_is_connected(void);

static void ask_send_task(void *arg)
{
    int qlen = (int)(intptr_t)arg;
    uint8_t *wav = NULL;
    size_t question_bytes=0;
    esp_http_client_handle_t cli = NULL;
    bool locked = false, success = false, no_speech = false, fetch_audio = false, request_audio=false;
    int got = 0;
    char job[33] = {0}, url[240];
    snprintf(url, sizeof(url), "%s", s_ask_url_buf[0] ? s_ask_url_buf : s_ask_url);
    s_ask_cancel = false;
    s_ask_sending = true;
    s_ask_phase = WATCHFACE_THINKING;
    if (!prov_is_connected()) goto done;
    /* AFE is free once question capture stops; keep recording the room. */
    if (!s_user_stopped) recorder_start_impl(true);
    if (recording_encode((const int16_t *)s_askbuf,qlen/2,(uint32_t)esp_timer_get_time(),&wav,&question_bytes)!=ESP_OK)
        goto done;
    int64_t deadline = esp_timer_get_time() / 1000 + 240000;
    while (esp_timer_get_time() / 1000 < deadline) {
        if (s_ask_cancel) { ESP_LOGI(TAG, "ask cancelled by bottom key"); goto done; }
        if (!http_lock_take(30000)) {
            ESP_LOGW(TAG, "ask: waiting for HTTPS timed out");
            goto done;
        }
        locked = true;
        esp_http_client_config_t cfg = {
            .url = url,
            .method = job[0] ? HTTP_METHOD_GET : HTTP_METHOD_POST,
            .timeout_ms = job[0] ? 15000 : 60000,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        cli = esp_http_client_init(&cfg);
        if (!cli) goto done;
        esp_http_client_set_header(cli, "Content-Type", "audio/ogg");
        esp_http_client_set_header(cli, "X-Async-Response", "1");
        esp_http_client_set_header(cli, "X-Stream-Audio", "1");
        if (job[0] && !request_audio) esp_http_client_set_header(cli, "X-Ask-Status", "1");
        if (s_token[0]) {
            char auth[110];
            snprintf(auth, sizeof(auth), "Bearer %s", s_token);
            esp_http_client_set_header(cli, "Authorization", auth);
        }
        int total = job[0] ? 0 : (int)question_bytes;
        ESP_LOGI(TAG, "ask opening %s bytes=%d", job[0] ? "poll" : "upload", total);
        if (esp_http_client_open(cli, total) != ESP_OK) goto done;
        http_low_latency(cli);
        int sent = 0;
        int last_report = 0;
        while (sent < total) {
            if (s_ask_cancel || esp_timer_get_time() / 1000 > deadline) goto done;
            int part = total - sent < 2048 ? total - sent : 2048;
            int wrote = esp_http_client_write(cli, (const char *)wav + sent, part);
            if (wrote <= 0) break;
            sent += wrote;
            if (sent - last_report >= 65536) {
                last_report = sent;
                ESP_LOGI(TAG, "ask uploading %d/%d", sent, total);
            }
        }
        if (sent != total) {
            ESP_LOGW(TAG, "ask body incomplete %d/%d", sent, total);
            goto done;
        }
        if (total) ESP_LOGI(TAG, "ask body sent %d/%d", sent, total);
        if (esp_http_client_fetch_headers(cli) < 0) goto done;
        int status = esp_http_client_get_status_code(cli);
        ESP_LOGI(TAG, "ask http=%d job=%s", status, job[0] ? job : "new");
        if (s_ask_cancel) { ESP_LOGI(TAG, "ask cancelled before playback"); goto done; }
        if (status == 200) {
            s_playing=true;
            s_ask_sending=false;
            show_playing();
            mic_set_playback_vol(100);
            success=audio_play_http(cli,fetch_audio,&got);
            mic_set_playback_vol(0);
            s_playing=false;
            ESP_LOGI(TAG,"answer streamed playback bytes=%d complete=%d",got,success);
            break;
        }
        char body[2048] = {0};
        int length = esp_http_client_read_response(cli, body, sizeof(body) - 1);
        if (status != 202 || length <= 0 || !esp_http_client_is_complete_data_received(cli)) {
            ESP_LOGW(TAG, "ask failed http=%d detail=%.*s", status, length > 0 ? length : 0, body);
            no_speech = status == 422 && strstr(body, "transcribing") != NULL;
            goto done;
        }
        body[length] = 0;
        cJSON *json = cJSON_ParseWithOpts(body, NULL, true);
        const cJSON *ok = cJSON_GetObjectItemCaseSensitive(json, "ok");
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(json, "job");
        const cJSON *stage = cJSON_GetObjectItemCaseSensitive(json, "stage");
        const cJSON *state = cJSON_GetObjectItemCaseSensitive(json, "state");
        const cJSON *answer = cJSON_GetObjectItemCaseSensitive(json, "answer");
        bool valid = cJSON_IsTrue(ok) && cJSON_IsString(id) && strlen(id->valuestring) == 32 &&
                     strspn(id->valuestring, "0123456789abcdef") == 32;
        if (valid && job[0] && strcmp(job, id->valuestring)) valid = false;
        if (valid) {
            snprintf(job, sizeof(job), "%s", id->valuestring);
            if (cJSON_IsString(answer) && answer->valuestring[0]) {
                bool first_text = !watchface_has_answer();
                watchface_set_answer(answer->valuestring);
                if (first_text && !s_manual_screen_off) screen_wake();
                if (first_text) ESP_LOGI(TAG, "answer text ready before audio: %u bytes",
                                        (unsigned)strlen(answer->valuestring));
            }
            fetch_audio = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json,"stream_ready"));
            bool legacy_done=cJSON_IsString(state) && !strcmp(state->valuestring,"done");
            request_audio=fetch_audio || legacy_done;
            if (cJSON_IsString(state) && !strcmp(state->valuestring, "failed")) {
                no_speech = cJSON_IsString(stage) && !strcmp(stage->valuestring, "transcribing");
                valid = false;
            }
            ESP_LOGI(TAG, "ask progress job=%s stage=%s", job,
                     cJSON_IsString(stage) ? stage->valuestring : "waiting");
            if (cJSON_IsString(stage)) {
                if (!strcmp(stage->valuestring, "transcribing")) s_ask_phase = WATCHFACE_RECOGNIZING;
                else if (!strcmp(stage->valuestring, "tangseng")) s_ask_phase = WATCHFACE_TANGSENG;
                else if (!strcmp(stage->valuestring, "tts")) s_ask_phase = WATCHFACE_TTS;
                else s_ask_phase = WATCHFACE_THINKING;
            }
        }
        cJSON_Delete(json);
        if (!valid) goto done;
        free(wav);
        wav = NULL;
        snprintf(url, sizeof(url), "%s/%s%s", s_ask_url_buf[0] ? s_ask_url_buf : s_ask_url,
                 job,fetch_audio ? "/audio" : "");
        s_ask_sending = false;
        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
        cli = NULL;
        http_lock_give();
        locked = false;
        if (!request_audio) vTaskDelay(pdMS_TO_TICKS(500));
    }
done:
    if (cli) {
        esp_http_client_close(cli);
        esp_http_client_cleanup(cli);
    }
    if (locked) http_lock_give();
    free(wav);
    s_ask_sending = false;
    if (!success && !s_ask_cancel) {
        ESP_LOGW(TAG, "ask failed job=%s; UI shows retry", job[0] ? job : "not_submitted");
        s_error_until_ms = esp_timer_get_time() / 1000 + 4000;
        play_question_prompt(no_speech);
    } else if (s_ask_cancel) {
        ESP_LOGI(TAG, "ask cancelled; skip error UI");
    }
    s_asking = false;
    ask_resume_recording();
    if (!s_recording) show_idle();
    vTaskDelete(NULL);
}

static void ask_resume_recording(void)
{
    bool resume = !s_user_stopped && (s_resume_after_ask || s_pending_start);
    s_resume_after_ask = false;
    s_pending_start = false;
    if (resume && !s_recording && !s_busy) {
        vTaskDelay(pdMS_TO_TICKS(300));
        if (!s_user_stopped && !s_recording && !s_busy && !s_asking) {
            ESP_LOGI(TAG, "question finished -> resume continuous recording");
            recorder_start();
        }
    }
}

/* AFE fetch 线程只投递事件，不能等待自身完成 reset。 */
void recorder_ask_wake(void) { s_wake_requested = true; }

static void handle_wake_request(void)
{
    s_wake_requested = false;
    if (!menu_voice_capture_active() && !s_asking && !s_playing) recorder_ask();
}

/* 所有入口均听到上键明确提交，绝不在松屏或固定秒数后静默提交。 */
void recorder_ask(void)
{
    if (!s_available || s_asking || s_ptt_pending || menu_active()) return;
    watchface_set_answer("");
    s_top_ptt_session = true;
    recorder_ptt_begin();
    if (!s_asking && !s_ptt_pending) s_top_ptt_session = false;
}

/* 下面键取消当前问答流程：聆听中→直接丢弃；已提交→丢弃在途请求与回答。 */
void recorder_ask_cancel(void)
{
    if (s_top_ptt_session) {
        s_top_ptt_session = false;
        ESP_LOGI(TAG, "bottom key -> cancel listening, not asking tangseng");
        recorder_ptt_end(false);
        return;
    }
    if (s_ptt_pending || s_asking || s_ask_sending) {
        ESP_LOGI(TAG, "bottom key -> cancel ask in flight, drop answer");
        s_ask_cancel = true;
        return;
    }
}

/* ---------------- 按住说话 ---------------- */

static void ptt_capture_task(void *arg)
{
    int want = OUT_BYTES_PER_SEC / 10;
    int64_t start = esp_timer_get_time() / 1000;
    int last_draw = 0;
    while (s_ptt_active && s_ptt_len + want <= ASK_BYTES) {
        int got = afe_read((int16_t *)(s_askbuf + s_ptt_len), want) ? want : -1;
        if (got > 0) s_ptt_len += got;
        int held = (int)(esp_timer_get_time() / 1000 - start);
        if (held - last_draw >= 200) {
            last_draw = held;
            show_ptt(held);
        }
        if (got <= 0) vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGI(TAG, "ptt captured %d bytes (%.1fs)", s_ptt_len,
             (float)s_ptt_len / OUT_BYTES_PER_SEC);
    s_ptt_capture_running = false;
    vTaskDelete(NULL);
}

static void ptt_start_after_record(void)
{
    s_ptt_pending = false;
    if (!s_ptt_user_down) {
        s_asking = false;
        show_idle();
        ask_resume_recording();
        return;
    }
    if (!s_available || !afe_reset_output(1200)) {
        ESP_LOGE(TAG, "AFE reset timeout before deferred ptt");
        s_asking = false;
        show_idle();
        ask_resume_recording();
        return;
    }
    s_asking = true;
    s_ptt_len = 0;
    s_ptt_active = true;
    s_ptt_capture_running = true;
    show_asking();
    if (xTaskCreate(ptt_capture_task, "ptt", 4096, NULL, 5, NULL) != pdPASS) {
        s_ptt_active = false;
        s_ptt_capture_running = false;
        s_asking = false;
        ESP_LOGE(TAG, "deferred ptt task create failed");
        show_idle();
        ask_resume_recording();
    }
}

void recorder_ptt_begin(void)
{
    if (!s_available || s_asking || menu_active()) return;
    s_user_stopped = false;
    s_resume_after_ask = true;
    s_ptt_user_down = true;
    screen_wake();
    if (s_recording) {
        s_resume_after_ask = true;
        s_ptt_pending = true;
        s_recording = false;
        ESP_LOGI(TAG, "ptt requested; sealing current recording segment");
        return;
    }
    if (s_busy) {
        s_ptt_pending = true;
        ESP_LOGI(TAG, "ptt queued during recorder finalization");
        return;
    }
    if (!afe_reset_output(1200)) {
        ESP_LOGE(TAG, "AFE reset timeout before ptt");
        s_ptt_user_down = false;
        ask_resume_recording();
        return;
    }
    s_asking = true;
    s_ptt_len = 0;
    s_ptt_active = true;
    s_ptt_capture_running = true;
    if (xTaskCreate(ptt_capture_task, "ptt", 4096, NULL, 5, NULL) != pdPASS) {
        s_ptt_active = false;
        s_ptt_capture_running = false;
        s_asking = false;
        ESP_LOGE(TAG, "ptt task create failed");
        ask_resume_recording();
        return;
    }
    ESP_LOGI(TAG, "ptt begin");
}

void recorder_ptt_end(bool send)
{
    s_ptt_user_down = false;
    if (s_ptt_pending) {
        ESP_LOGI(TAG, "ptt released while recorder is sealing");
        return;
    }
    s_ptt_active = false;
    int wait = 0;
    while (s_ptt_capture_running && wait++ < 120) vTaskDelay(pdMS_TO_TICKS(50));
    if (s_ptt_capture_running) {
        ESP_LOGE(TAG, "question capture did not stop; keeping buffer owned");
        return;
    }
    if (!send) {
        ESP_LOGI(TAG, "ptt cancelled (%d bytes)", s_ptt_len);
        s_asking = false;
        show_idle();
        ask_resume_recording();
        return;
    }
    if (!question_has_voice(s_askbuf, s_ptt_len)) {
        ESP_LOGW(TAG, "question has no clear speech");
        if (xTaskCreate(no_speech_task, "sayretry", 4096, NULL, 5, NULL) != pdPASS) {
            s_asking = false;
            ask_resume_recording();
        }
        return;
    }
    /* Trim quiet edges only, keeping 300ms context and all internal pauses. */
    int first = -1, last = 0;
    for (int offset = 0; offset + 640 <= s_ptt_len; offset += 640) {
        const int16_t *frame = (const int16_t *)(s_askbuf + offset);
        int64_t sum = 0;
        for (int i = 0; i < 320; i++) sum += (int64_t)frame[i] * frame[i];
        if (sum >= 320 * 60 * 60) {
            if (first < 0) first = offset;
            last = offset + 640;
        }
    }
    if (first >= 0) {
        int begin = first > 9600 ? first - 9600 : 0;
        int end = last + 9600 < s_ptt_len ? last + 9600 : s_ptt_len;
        int previous = s_ptt_len;
        memmove(s_askbuf, s_askbuf + begin, end - begin);
        s_ptt_len = end - begin;
        ESP_LOGI(TAG, "question silence trim %d -> %d bytes", previous, s_ptt_len);
    }
    ESP_LOGI(TAG, "ptt send %d bytes", s_ptt_len);
    snprintf(s_ask_url_buf, sizeof(s_ask_url_buf), "%s", s_ask_url);
    show_thinking();
    if (xTaskCreate(ask_send_task, "asksend", 8192, (void *)(intptr_t)s_ptt_len, 5, NULL) != pdPASS) {
        s_asking = false;
        show_idle();
        ask_resume_recording();
    }
}

/* ---------------- 声纹录入支持 ---------------- */

int recorder_grab_pcm(int16_t *buf, int max_bytes, int seconds)
{
    if (!s_available || !afe_reset_output(1200)) return 0;
    int need = seconds * OUT_SAMPLE_RATE * 2;   /* 目标字节数 */
    if (need > max_bytes) need = max_bytes;
    int got = 0;
    int64_t deadline = esp_timer_get_time() / 1000 + seconds * 1000ULL + 3000;
    while (got < need) {
        int part = need - got;
        if (part > OUT_BYTES_PER_SEC / 10) part = OUT_BYTES_PER_SEC / 10; /* 最多 100ms/次 */
        /* afe_read 第二参数为字节数 */
        if (!afe_read((int16_t *)((uint8_t *)buf + got), part)) {
            if (esp_timer_get_time() / 1000 > deadline) break;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        got += part;
    }
    return got;
}

bool recorder_upload_enroll(const uint8_t *wav, int len, int slot, char *out, int outlen)
{
    char url[192];
    snprintf(url, sizeof(url), "%s", s_hub_url);
    char *cut = strstr(url, "/v1/upload");
    if (!cut) cut = strstr(url, "/upload");
    if (cut) snprintf(cut, sizeof(url) - (cut - url), "/v1/enroll");
    else snprintf(url, sizeof(url), "%s", s_hub_url);

    ESP_LOGI(TAG, "enroll: post to %s (%d bytes)", url, len);
    if (!http_lock_take(20000)) return false;
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 20000,   /* 20s：录音8s+上传+服务端MFCC处理，之前8s太短 */
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) { http_lock_give(); return false; }
    esp_http_client_set_header(cli, "Content-Type", "audio/wav");
    char slot_hdr[8];
    snprintf(slot_hdr, sizeof(slot_hdr), "%d", slot);
    esp_http_client_set_header(cli, "X-Slot", slot_hdr);
    if (s_token[0]) {
        char auth[110];
        snprintf(auth, sizeof(auth), "Bearer %s", s_token);
        esp_http_client_set_header(cli, "Authorization", auth);
    }
    bool ok = false;
    ESP_LOGI(TAG, "enroll: opening connection...");
    int open_rc = esp_http_client_open(cli, len);
    ESP_LOGI(TAG, "enroll: open rc=%d", open_rc);
    if (open_rc == ESP_OK) {
        int sent = 0;
        while (sent < len) {
            int w = esp_http_client_write(cli, (const char *)wav + sent, len - sent);
            if (w <= 0) {
                ESP_LOGE(TAG, "enroll write failed at %d/%d rc=%d", sent, len, w);
                break;
            }
            sent += w;
        }
        if (sent == len) {
            esp_http_client_fetch_headers(cli);
            int status = esp_http_client_get_status_code(cli);
            char resp[512] = {0};
            int used = 0;
            while (used < (int)sizeof(resp) - 1) {
                int r = esp_http_client_read(cli, resp + used, sizeof(resp) - 1 - used);
                if (r <= 0) break;
                used += r;
            }
            ESP_LOGI(TAG, "enroll upload %d/%d bytes http=%d resp=%s", sent, len, status, resp);
            if (status == 200 && strstr(resp, "\"ok\": true")) {
                char *nm = strstr(resp, "\"name\"");
                if (nm) {
                    nm = strchr(nm + 6, '"');
                    if (nm) {
                        nm++;
                        int i = 0;
                        while (nm[i] && nm[i] != '"' && i < outlen - 1) { out[i] = nm[i]; i++; }
                        out[i] = 0;
                        ok = i > 0;
                    }
                }
            }
        }
    }
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    http_lock_give();
    return ok;
}

/* 给某名额改名（同步到服务器声纹库） */
bool recorder_upload_rename(int slot, const char *name)
{
    char url[192];
    snprintf(url, sizeof(url), "%s", s_hub_url);
    char *cut = strstr(url, "/v1/upload");
    if (cut) snprintf(cut, sizeof(url) - (cut - url), "/v1/rename");
    else snprintf(url, sizeof(url), "%s", s_hub_url);

    char body[80];
    int blen = snprintf(body, sizeof(body), "{\"slot\":%d,\"name\":\"%s\"}", slot, name);

    if (!http_lock_take(20000)) return false;
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 8000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    esp_http_client_set_header(cli, "Content-Type", "application/json");
    if (s_token[0]) {
        char auth[110];
        snprintf(auth, sizeof(auth), "Bearer %s", s_token);
        esp_http_client_set_header(cli, "Authorization", auth);
    }
    bool ok = false;
    if (esp_http_client_open(cli, blen) == ESP_OK) {
        esp_http_client_write(cli, body, blen);
        esp_http_client_fetch_headers(cli);
        ok = (esp_http_client_get_status_code(cli) == 200);
    }
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    ESP_LOGI(TAG, "rename slot %d -> %s: %s", slot, name, ok ? "ok" : "fail");
    http_lock_give();
    return ok;
}

/* 通知服务器删除某名额的声纹 */
bool recorder_upload_unenroll(int slot)
{
    char url[192];
    snprintf(url, sizeof(url), "%s", s_hub_url);
    char *cut = strstr(url, "/v1/upload");
    if (!cut) cut = strstr(url, "/upload");
    if (cut) snprintf(cut, sizeof(url) - (cut - url), "/v1/unenroll");
    else snprintf(url, sizeof(url), "%s", s_hub_url);

    if (!http_lock_take(20000)) return false;
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 8000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    char slot_hdr[8];
    snprintf(slot_hdr, sizeof(slot_hdr), "%d", slot);
    esp_http_client_set_header(cli, "X-Slot", slot_hdr);
    if (s_token[0]) {
        char auth[110];
        snprintf(auth, sizeof(auth), "Bearer %s", s_token);
        esp_http_client_set_header(cli, "Authorization", auth);
    }
    bool ok = false;
    if (esp_http_client_open(cli, 0) == ESP_OK) {
        esp_http_client_fetch_headers(cli);
        ok = (esp_http_client_get_status_code(cli) == 200);
    }
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    ESP_LOGI(TAG, "unenroll slot %d -> %s", slot, ok ? "ok" : "fail");
    http_lock_give();
    return ok;
}

/* ---------------- 按键 ---------------- */

void recorder_toggle_screen(void)
{
    s_screen_off = !s_screen_off;
    s_manual_screen_off = s_screen_off;
    display_brightness(s_screen_off ? 0 : 0xC0);
    ESP_LOGI(TAG, "screen %s", s_screen_off ? "off" : "on");
}

/* 上面键 = AXP PWRKEY：短按 → 开始录音（长按 4s 由 PMIC 硬件关机） */
static void axp_clear_key_irqs(void)
{
    if (!s_axp_dev) return;
    const uint8_t regs[] = {0x48, 0x49, 0x4A};
    for (int i = 0; i < 3; i++) {
        uint8_t reg = regs[i], value = 0;
        if (i2c_master_transmit_receive(s_axp_dev, &reg, 1, &value, 1, 50) == ESP_OK && value) {
            uint8_t clear[2] = {reg, value};
            i2c_master_transmit(s_axp_dev, clear, 2, 50);
        }
    }
}

static void axp_poll_key(void)
{
    if (!s_axp_dev) return;
    const uint8_t regs[] = {0x48, 0x49, 0x4A};
    for (int i = 0; i < 3; i++) {
        uint8_t r = regs[i], v = 0;
        if (i2c_master_transmit_receive(s_axp_dev, &r, 1, &v, 1, 50) != ESP_OK || v == 0) continue;
        if (r == 0x49) {
            if (v & 0x08) {
                ESP_LOGI(TAG, "top key pressed");
                recorder_top_key();
            }
            uint8_t w[2] = {r, v};
            i2c_master_transmit(s_axp_dev, w, 2, 50);
        }
    }
}

static void button_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << REC_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    /* 上电和 USB reset 会留下 PWRKEY IRQ；先等硬件稳定并清掉历史事件。 */
    vTaskDelay(pdMS_TO_TICKS(600));
    axp_clear_key_irqs();

    bool was_down = false;
    int64_t down_ms = 0;
    bool hold_handled = false;

    for (;;) {
        bool now = gpio_get_level(REC_BUTTON);
        int64_t ms = esp_timer_get_time() / 1000;

        if (!was_down && !now) {
            was_down = true;
            down_ms = ms;
            hold_handled = false;
        } else if (was_down && now) {
            was_down = false;
            if (!hold_handled && (ms - down_ms) < 500) {
                recorder_ask_cancel();        /* 唤醒/问答中：先取消流程，不再问唐僧 */
                recorder_toggle_screen();     /* 短按：熄屏/亮屏，录音不受影响 */
            }
            if (!hold_handled || !s_busy) s_stop_hold_progress = 0;
        }

        if (was_down && s_recording) {
            int64_t held = ms - down_ms;
            s_stop_hold_progress = held >= 3000 ? 3000 : (int)held;
            if (s_recording) {
                if (held >= 3000) {
                    hold_handled = true;
                    s_stop_hold_progress = 3000;
                    ESP_LOGI(TAG, "bottom key LONG press -> save and stop recording");
                    recorder_stop();
                    screen_wake();
                }
            }
        } else if (s_stop_hold_progress < 3000 || !s_busy) {
            s_stop_hold_progress = 0;
        }
        if (s_wake_requested) handle_wake_request();
        axp_poll_key();
        if (menu_active()) menu_poll();
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

/* ---------------- 触摸 ---------------- */

static void touch_task(void *arg)
{
    int taps = 0;
    int64_t first_ms = 0;
    bool down_prev = false;
    int start_x = 0, start_y = 0, cur_x = 0, cur_y = 0;
    int64_t down_ms = 0;
    bool swiped = false;
    bool ptt = false;

    for (;;) {
        uint16_t tx = 0, ty = 0;
        int down = 0;
        touch_poll_xy(&tx, &ty, &down);
        int64_t ms = esp_timer_get_time() / 1000;

        /* 熄屏时忽略一切触摸（单击唤醒 / 长按问答 / 下滑菜单全部禁用），
         * 只有下面键能点亮屏幕，避免误触。 */
        if (s_screen_off) {
            taps = 0;
            swiped = false;
            ptt = false;
            down_prev = down;
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }

        if (down && !down_prev) {
            start_x = tx; start_y = ty; cur_x = tx; cur_y = ty;
            down_ms = ms;
            swiped = false;
            ptt = false;
        }
        if (down) { cur_x = tx; cur_y = ty; }

        /* 按住超过 260ms → 进入"按住说话"（顶部区域留给下滑菜单） */
        if (down && !swiped && !ptt && !menu_active() && (ms - down_ms) >= 260 && start_y > 120) {
            ptt = true;
            recorder_ask();
        }

        /* 顶部下滑 → 设置菜单 */
        if (down && !swiped && !ptt && start_y < 140 && (cur_y - start_y) > 60) {
            swiped = true;
            ESP_LOGI(TAG, "swipe down -> open menu");
            menu_open();
        }

        /* 菜单打开时：按住 800ms = 长按操作（删除声纹） */
        if (down && menu_active() && !swiped && (ms - down_ms) >= 800) {
            swiped = true;                       /* 防止松手时再触发点击 */
            extern bool menu_long_press(int x, int y);
            menu_long_press(cur_x, cur_y);
        }

        if (!down && down_prev) {
            if (ptt) {
                /* 松屏仍保持聆听；仅上键单击提交。 */
                ptt = false;
                taps = 0;
            } else if (!swiped) {
                if (menu_active()) {
                    menu_tap(cur_x, cur_y);
                } else {
                    int64_t held = ms - down_ms;
                    int dx = cur_x - start_x, dy = cur_y - start_y;
                    if (held < 260 && dx * dx + dy * dy < 40 * 40) {
                        if (taps == 0 || ms - first_ms > 700) {
                            taps = 1;
                            first_ms = ms;
                        } else {
                            taps = 2;
                        }
                    }
                }
            }
        }
        down_prev = down;

        /* 单击只唤醒屏幕；双击进入在线语音问答。 */
        if (taps == 1 && ms - first_ms > 700) {
            ESP_LOGI(TAG, "tap -> wake display");
            watchface_set_answer("");
            screen_wake();
            if (!s_recording && !s_busy && !s_asking) show_idle();
            taps = 0;
        } else if (taps == 2 && ms - first_ms > 700) {
            ESP_LOGI(TAG, "double tap -> ask");
            if (!s_recording && !s_busy && !s_asking) recorder_ask();
            taps = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

/* ---------------- 配置 ---------------- */

void recorder_set_url(const char *url, const char *token)
{
    if (url && url[0]) snprintf(s_hub_url, sizeof(s_hub_url), "%s", url);
    if (token) snprintf(s_token, sizeof(s_token), "%s", token);
    snprintf(s_ask_url, sizeof(s_ask_url), "%s", s_hub_url);
    char *cut = strstr(s_ask_url, "/v1/upload");
    if (cut) snprintf(cut, sizeof(s_ask_url) - (cut - s_ask_url), "/v1/ask");
    ESP_LOGI(TAG, "upload target = %s (token %s), ask = %s",
             s_hub_url, s_token[0] ? "set" : "none", s_ask_url);
    nvs_handle_t h;
    if (nvs_open("prov", NVS_READWRITE, &h) == ESP_OK) {
        if (url && url[0]) nvs_set_str(h, "hub_url", s_hub_url);
        if (token) nvs_set_str(h, "token", s_token);
        nvs_commit(h);
        nvs_close(h);
    }
}

void recorder_init(const char *hub_ip, int hub_port)
{
    if (s_initialized) {
        if (hub_ip && hub_ip[0]) {
            char url[96];
            snprintf(url, sizeof(url), "http://%s:%d/upload", hub_ip, hub_port);
            recorder_set_url(url, "");
        }
        ESP_LOGI(TAG, "recorder already initialized");
        return;
    }
    s_initialized = true;
    {
        nvs_handle_t h;
        if (nvs_open("prov", NVS_READONLY, &h) == ESP_OK) {
            char u[192] = {0}, t[80] = {0};
            size_t lu = sizeof(u), lt = sizeof(t);
            bool have = false;
            if (nvs_get_str(h, "hub_url", u, &lu) == ESP_OK && u[0]) {
                snprintf(s_hub_url, sizeof(s_hub_url), "%s", u);
                have = true;
            }
            if (nvs_get_str(h, "token", t, &lt) == ESP_OK && t[0]) {
                snprintf(s_token, sizeof(s_token), "%s", t);
            }
            nvs_close(h);
            if (have) {
                snprintf(s_ask_url, sizeof(s_ask_url), "%s", s_hub_url);
                char *cut = strstr(s_ask_url, "/v1/upload");
                if (cut) snprintf(cut, sizeof(s_ask_url) - (cut - s_ask_url), "/v1/ask");
                ESP_LOGI(TAG, "restored upload target: %s", s_hub_url);
            }
        }
    }
    if (hub_ip && hub_ip[0]) {
        char url[96];
        snprintf(url, sizeof(url), "http://%s:%d/upload", hub_ip, hub_port);
        recorder_set_url(url, "");
    }

    s_seg[0] = heap_caps_malloc(SEG_BYTES, MALLOC_CAP_SPIRAM);
    s_seg[1] = heap_caps_malloc(SEG_BYTES, MALLOC_CAP_SPIRAM);
    s_askbuf = heap_caps_malloc(ASK_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_seg[0] || !s_seg[1] || !s_askbuf) {
        ESP_LOGE(TAG, "no psram for record buffers");
        return;
    }
    s_saveq = xQueueCreate(4, sizeof(save_msg_t));
    if (!s_saveq) {
        ESP_LOGE(TAG, "save queue create failed");
        return;
    }

    {
        i2c_device_config_t dc = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = 0x34,
            .scl_speed_hz = 100000,
        };
        i2c_master_bus_add_device(display_i2c_bus(), &dc, &s_axp_dev);
    }

    menu_init();
    if (queue_init() != ESP_OK) {
        ESP_LOGE(TAG, "storage queue unavailable; recorder disabled");
        return;
    }
    if (afe_init() != ESP_OK) {
        ESP_LOGE(TAG, "AFE init failed; recorder disabled");
        return;
    }
    if (xTaskCreate(save_task, "qsave", 8192, NULL, 4, NULL) != pdPASS ||
        xTaskCreate(flush_task, "qflush", 8192, NULL, 4, NULL) != pdPASS ||
        xTaskCreate(button_task, "btn", 4096, NULL, 7, NULL) != pdPASS) {
        ESP_LOGE(TAG, "recorder worker task create failed; recorder disabled");
        return;
    }
    if (xTaskCreate(recorder_ui_task, "rec_ui", 4096, NULL, 2, NULL) != pdPASS) {
        ESP_LOGW(TAG, "recorder UI task create failed; audio capture remains available");
    }
    if (touch_init() == ESP_OK) {
        if (xTaskCreate(touch_task, "tp", 4096, NULL, 8, NULL) != pdPASS) {
            ESP_LOGW(TAG, "touch task create failed");
        }
    }
    s_available = true;
    ESP_LOGI(TAG, "recorder ready -> %s", s_hub_url);
}
