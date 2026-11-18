/*
 * 录音 / 语音问答 / 按住说话 —— ESP32-S3 语音工作助手
 *
 * 交互总览：
 *   上面键短按       开始录音（录音中再按无效；停止靠下面键长按）
 *   下面键短按       熄屏 / 亮屏（录音不受影响）
 *   下面键长按 3 秒  保存尾段并停止录音
 *   屏幕轻点         唤醒屏幕（不改变录音）；双击进入问答
 *   屏幕按住 >0.26s  按住说话（微信式）：说完松手即发送给唐僧
 *   屏幕顶部下滑     打开设置菜单（WiFi / 声纹）
 *   喊"你好小智"     唤醒问答（AFE 内置唤醒）
 */
#include <string.h>
#include <stdio.h>
#include <math.h>
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

static const char *TAG = "rec";

/* ---------------- 常量 ---------------- */

#define OUT_SAMPLE_RATE   16000
#define OUT_BYTES_PER_SEC (OUT_SAMPLE_RATE * 2)     /* 16k 单声道 16bit */
#define SEG_SECONDS       20
#define SEG_BYTES         (OUT_BYTES_PER_SEC * SEG_SECONDS)
#define ASK_SECONDS       10
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
static volatile bool s_auto;             /* 全天自动采集 */
static volatile bool s_screen_off;
static volatile bool s_pending_start;
static volatile bool s_ask_pending;
static volatile bool s_resume_after_ask;
static volatile bool s_ptt_pending;
static volatile bool s_ptt_user_down;

static uint8_t *s_askbuf;                /* 问题 / 声纹样本缓冲 */

static char s_hub_url[192] = "http://192.168.2.27:8765/upload";
static char s_token[80];
static char s_ask_url[192];
static char s_ask_url_buf[192];

/* 声波界面状态 */
#define WAVE_BINS 72
static float s_wave_bins[WAVE_BINS];

/* 按住说话状态 */
static volatile bool s_ptt_active;
static volatile int s_ptt_len;

typedef struct { int idx; int len; } save_msg_t;
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
static void show_offline(void);
static void touch_task(void *arg);

extern void display_idle_ui(void);
extern bool prov_ui_active(void);
extern void ui_fill_rect(int x, int y, int w, int h, uint16_t color);
extern void ui_number(int value, int digits, int y, int dw, int dh, int gap, int t, uint16_t color);

/* ---------------- WAV 头 ---------------- */

static void wav_header(uint8_t *h, uint32_t data_len, uint32_t rate, uint16_t ch, uint16_t bits)
{
    uint32_t byte_rate = rate * ch * bits / 8;
    uint16_t block_align = ch * bits / 8;
    memcpy(h + 0, "RIFF", 4);
    uint32_t riff = 36 + data_len;
    memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    uint32_t fmt = 16;
    memcpy(h + 16, &fmt, 4);
    uint16_t pcm = 1;
    memcpy(h + 20, &pcm, 2);
    memcpy(h + 22, &ch, 2);
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &byte_rate, 4);
    memcpy(h + 32, &block_align, 2);
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data_len, 4);
}

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

static void draw_disc(int cx, int cy, int r, uint16_t color)
{
    for (int y = -r; y <= r; y++) {
        int w = 0;
        while ((w + 1) * (w + 1) + y * y <= r * r) w++;
        if (w > 0) ui_fill_rect(cx - w, cy + y, 2 * w + 1, 1, color);
    }
}

/* 录音页使用 AFE feed 线程记录的原始麦克风包络，避免 AGC 压平音量。 */
static void draw_level_wave(int cy, const uint16_t *levels, int count)
{
    const int step_x = 5;
    const int bar_w = 3;
    const int x0 = (LCD_H_RES - WAVE_BINS * step_x) / 2;
    for (int i = 0; i < WAVE_BINS; i++) {
        int src = count > 0 ? (i * count) / WAVE_BINS : 0;
        if (src >= count) src = count - 1;
        float raw_rms = count > 0 ? (float)levels[src] : 0.0f;
        float db = 20.0f * log10f((raw_rms + 1.0f) / 32768.0f);
        float target = (db + 72.0f) / 60.0f;
        if (target < 0.0f) target = 0.0f;
        if (target > 1.0f) target = 1.0f;
        float alpha = target > s_wave_bins[i] ? 0.72f : 0.28f;
        s_wave_bins[i] += (target - s_wave_bins[i]) * alpha;
        int h = 2 + (int)(s_wave_bins[i] * 108.0f);
        ui_fill_rect(x0 + i * step_x, cy - h, bar_w, 2 * h, 0xFFFF);
    }
}

/* ---------------- 界面 ---------------- */

static void show_idle(void)
{
    screen_wake();
    if (menu_active() || prov_ui_active()) return;
    if (!display_frame_take(100)) return;
    display_idle_ui();
    display_frame_give();
}

static void show_recording(void)
{
    screen_wake();
    if (menu_active() || prov_ui_active()) return;
    if (!display_frame_take(100)) return;
    memset(s_wave_bins, 0, sizeof(s_wave_bins));
    display_fill(0x0000);
    draw_disc(LCD_H_RES / 2, 233, 26, 0xF800);          /* 红点 */
    int tw = 3 * UI_FONT_CELL;
    ui_text((LCD_H_RES - tw) / 2, 150, "录音中", 0xFFFF, 1);
    display_flush();
    display_frame_give();
}

static void recorder_ui_task(void *arg)
{
    uint16_t levels[96];
    for (;;) {
        if (s_recording && !s_screen_off && !menu_active() && !prov_ui_active()) {
            int count = afe_get_level_history(levels, (int)(sizeof(levels) / sizeof(levels[0])));
            if (display_frame_take(0)) {
                if (!menu_active() && !prov_ui_active() && s_recording && !s_screen_off) {
                    display_fill(0x0000);
                    draw_level_wave(233, levels, count);
                    ui_fill_rect(70, 232, LCD_H_RES - 140, 2, 0xFFFF);
                    int tw = 3 * UI_FONT_CELL;
                    ui_text((LCD_H_RES - tw) / 2, 150, "录音中", 0xFFFF, 1);
                    draw_disc(LCD_H_RES / 2, 118, 7, 0xF800);
                    int val = (s_total / OUT_BYTES_PER_SEC / 60) * 100 +
                              (s_total / OUT_BYTES_PER_SEC % 60);
                    if (val > 9999) val = 9999;
                    ui_number(val, 4, 360, 34, 54, 9, 7, 0xFFFF);
                    display_flush();
                }
                display_frame_give();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* 按住说话界面 */
static void show_ptt(int held_ms)
{
    if (!display_frame_take(0)) return;
    if (menu_active() || prov_ui_active()) { display_frame_give(); return; }
    uint16_t levels[96];
    int count = afe_get_level_history(levels, (int)(sizeof(levels) / sizeof(levels[0])));
    display_fill(0x0000);
    int tw = 3 * UI_FONT_CELL;
    ui_text((LCD_H_RES - tw) / 2, 150, "请说话", 0xFFFF, 1);
    draw_disc(LCD_H_RES / 2, 118, 7, 0xF800);
    draw_level_wave(233, levels, count);
    tw = 4 * UI_FONT_CELL;
    ui_text((LCD_H_RES - tw) / 2, 396, "松开发送", 0xFFFF, 1);
    display_flush();
    display_frame_give();
}

/* 断网提示 */
static void show_offline(void)
{
    if (!display_frame_take(100)) return;
    if (menu_active() || prov_ui_active()) { display_frame_give(); return; }
    display_fill(0x0000);
    int tw = 4 * UI_FONT_CELL;
    ui_text((LCD_H_RES - tw) / 2, 190, "无网络", 0xFFFF, 1);
    tw = 6 * UI_FONT_CELL;
    ui_text((LCD_H_RES - tw) / 2, 250, "请先配置WiFi", 0xFFFF, 1);
    display_flush();
    display_frame_give();
}

/* 采集问题界面 */
static void show_asking(void)
{
    if (!display_frame_take(100)) return;
    if (menu_active() || prov_ui_active()) { display_frame_give(); return; }
    display_fill(0x0000);
    int tw = 3 * UI_FONT_CELL;
    ui_text((LCD_H_RES - tw) / 2, 160, "请说话", 0xFFFF, 1);
    draw_disc(LCD_H_RES / 2, 270, 12, 0xF800);
    display_flush();
    display_frame_give();
}

static void show_thinking(void)
{
    if (!display_frame_take(100)) return;
    if (menu_active() || prov_ui_active()) { display_frame_give(); return; }
    display_fill(0x0000);
    int tw = 3 * UI_FONT_CELL;
    ui_text((LCD_H_RES - tw) / 2, 180, "思考中", 0xFFFF, 1);
    draw_disc(LCD_H_RES / 2, 280, 10, 0x07FF);
    display_flush();
    display_frame_give();
}

/* 播放回答界面 */
static void show_playing(void)
{
    if (!display_frame_take(100)) return;
    if (menu_active() || prov_ui_active()) { display_frame_give(); return; }
    display_fill(0x0000);
    int tw = 3 * UI_FONT_CELL;
    ui_text((LCD_H_RES - tw) / 2, 160, "回答中", 0xFFFF, 1);
    for (int i = 0; i < 3; i++) {
        draw_disc(LCD_H_RES / 2 - 70 + i * 70, 300, 12, 0xFFFF);
    }
    display_flush();
    display_frame_give();
}

/* ---------------- 写盘任务 ---------------- */

static void save_task(void *arg)
{
    save_msg_t m;
    for (;;) {
        if (xQueueReceive(s_saveq, &m, portMAX_DELAY) != pdTRUE) continue;
        esp_err_t err = queue_save(s_seg[m.idx], m.len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "segment save failed: idx=%d len=%d err=%d", m.idx, m.len, err);
        }
        s_buf_busy[m.idx] = false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static bool submit_segment(int idx, int len)
{
    save_msg_t m = { .idx = idx, .len = len };
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
    int seg_max_rms = 0;
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
        {
            int frames = got / 2;
            int16_t *p = (int16_t *)(s_seg[s_cur] + s_len[s_cur]);
            int64_t acc = 0;
            for (int f = 0; f < frames; f++) {
                int32_t v = p[f];
                acc += v * v;
            }
            int rms = frames ? (int)sqrtf((float)acc / frames) : 0;
            if (rms > seg_max_rms) seg_max_rms = rms;

            int64_t now = esp_timer_get_time() / 1000;
            if (rms >= SILENCE_RMS) last_speech = now;
            if (s_auto && !menu_active() && !prov_ui_active()) {
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
            if (seg_max_rms >= SILENCE_RMS) {
                if (!submit_segment(s_cur, s_len[s_cur])) {
                    s_recording = false;
                    break;
                }
            } else {
                ESP_LOGI(TAG, "segment silent (max rms %d), dropped", seg_max_rms);
            }
            seg_max_rms = 0;
            s_cur = next;
            s_len[s_cur] = 0;
        }
    }

    /* 尾段 */
    if (s_len[s_cur] > OUT_BYTES_PER_SEC / 2 && seg_max_rms >= SILENCE_RMS) {
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
    if (!s_available || !afe_is_ready()) {
        ESP_LOGE(TAG, "recorder unavailable");
        return;
    }
    if (s_recording) {
        ESP_LOGI(TAG, "already recording, ignore start");
        return;
    }
    if (s_busy || s_asking) {
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
    screen_wake();
    show_recording();
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
    if (s_recording) s_recording = false;
}

/* 上面键短按入口：空闲→立即开始；录音中→忽略；收尾中→挂起 */
void recorder_request_start(void)
{
    if (s_recording) {
        ESP_LOGI(TAG, "already recording, ignore");
        return;
    }
    if (s_busy || s_asking) {
        ESP_LOGI(TAG, "busy, pending start");
        s_pending_start = true;
        return;
    }
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

const char *recorder_state(void)
{
    return s_recording ? "recording"
         : (s_busy ? "uploading"
         : (s_asking ? "asking" : "idle"));
}

const char *recorder_upload_url(void) { return s_hub_url; }
const char *recorder_upload_token(void) { return s_token; }

/* ---------------- 离线队列补传 ---------------- */

static void flush_task(void *arg)
{
    for (;;) {
        int left = queue_flush_periodic();
        if (left > 0) ESP_LOGI(TAG, "queue: %d 段待补传", left);
        /* 声纹样本若上传失败，这里自动重试 */
        int vp = queue_flush_enroll();
        if (vp > 0) ESP_LOGI(TAG, "voiceprint: %d 个样本待重试", vp);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* ---------------- 语音问答 ---------------- */

/* 发送已录好的问题并播放回答 */
extern bool prov_is_connected(void);

static void ask_send_task(void *arg)
{
    int qlen = (int)(intptr_t)arg;
    /* 断网时立即失败，不阻塞界面 45 秒 */
    if (!prov_is_connected()) {
        ESP_LOGW(TAG, "ask skipped: no wifi");
        s_asking = false;
        show_offline();
        vTaskDelay(pdMS_TO_TICKS(1500));
        show_idle();
        ask_resume_recording();
        vTaskDelete(NULL);
        return;
    }
    int total = 44 + qlen;
    uint8_t *wav = heap_caps_malloc(total, MALLOC_CAP_SPIRAM);
    if (!wav) {
        s_asking = false;
        show_idle();
        ask_resume_recording();
        vTaskDelete(NULL);
        return;
    }
    wav_header(wav, qlen, OUT_SAMPLE_RATE, 1, 16);
    memcpy(wav + 44, s_askbuf, qlen);

    esp_http_client_config_t cfg = {
        .url = s_ask_url_buf[0] ? s_ask_url_buf : s_ask_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        free(wav);
        s_asking = false;
        show_offline();
        ask_resume_recording();
        vTaskDelete(NULL);
        return;
    }
    esp_http_client_set_header(cli, "Content-Type", "audio/wav");
    if (s_token[0]) {
        char auth[110];
        snprintf(auth, sizeof(auth), "Bearer %s", s_token);
        esp_http_client_set_header(cli, "Authorization", auth);
    }
    esp_http_client_set_header(cli, "Accept", "audio/wav");
    show_thinking();
    if (esp_http_client_open(cli, total) == ESP_OK) {
        esp_http_client_write(cli, (const char *)wav, total);
        esp_http_client_fetch_headers(cli);
        int status = esp_http_client_get_status_code(cli);
        ESP_LOGI(TAG, "ask http=%d", status);
        if (status == 200) {
            uint8_t *pcm = heap_caps_malloc(1152000, MALLOC_CAP_SPIRAM);
            int got = 0, r;
            while (pcm && got < 1152000 &&
                   (r = esp_http_client_read(cli, (char *)pcm + got, 8192)) > 0) {
                got += r;
            }
            ESP_LOGI(TAG, "answer pcm %d bytes", got);
            if (pcm && got > 0) {
                show_playing();
                mic_set_playback_vol(75);      /* 播放期间开声音 */
                int off = 0;
                while (off + 6400 <= got) {
                    mic_play(pcm + off, 6400);
                    off += 6400;
                }
                if (off < got) mic_play(pcm + off, got - off);
                vTaskDelay(pdMS_TO_TICKS(300));
                mic_set_playback_vol(0);       /* 恢复静音，避免录音回声 */
            }
            free(pcm);
        } else {
            ESP_LOGW(TAG, "ask failed http=%d", status);
        }
    } else {
        ESP_LOGE(TAG, "ask open failed");
    }
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    free(wav);
    s_asking = false;
    ESP_LOGI(TAG, "ask done");
    show_idle();
    ask_resume_recording();
    vTaskDelete(NULL);
}

static void ask_resume_recording(void)
{
    bool resume = s_resume_after_ask || s_auto || s_pending_start;
    s_resume_after_ask = false;
    s_pending_start = false;
    if (resume && !s_recording && !s_busy) {
        vTaskDelay(pdMS_TO_TICKS(300));
        recorder_start();
    }
}

/* 自动录 6 秒问题（唤醒词触发） */
static void ask_task(void *arg)
{
    if (!afe_reset_output(1200)) {
        ESP_LOGE(TAG, "AFE reset timeout before ask");
        s_asking = false;
        show_idle();
        ask_resume_recording();
        vTaskDelete(NULL);
        return;
    }
    int qlen = 0;
    int want = OUT_BYTES_PER_SEC / 10;
    for (int i = 0; i < 6 * 10 && qlen < ASK_BYTES; i++) {
        int got = afe_read((int16_t *)(s_askbuf + qlen), want) ? want : -1;
        if (got <= 0) break;
        qlen += got;
    }
    ESP_LOGI(TAG, "ask captured %d bytes (%.1fs)", qlen, (float)qlen / OUT_BYTES_PER_SEC);
    if (qlen < OUT_BYTES_PER_SEC) {
        ESP_LOGW(TAG, "question too short");
        s_asking = false;
        show_idle();
        ask_resume_recording();
        vTaskDelete(NULL);
        return;
    }
    show_thinking();
    if (xTaskCreate(ask_send_task, "asksend", 8192, (void *)(intptr_t)qlen, 5, NULL) != pdPASS) {
        s_asking = false;
        show_idle();
        ask_resume_recording();
    }
    vTaskDelete(NULL);
}

/* 唤醒词触发：录音中先停录音，问答后自动续录 */
void recorder_ask_wake(void)
{
    if (s_asking || s_ask_pending) return;
    if (s_recording) {
        s_resume_after_ask = true;
        s_ask_pending = true;
        s_ptt_pending = false;
        s_recording = false;
        ESP_LOGI(TAG, "wake queued; sealing current recording segment");
        return;
    }
    if (s_busy) {
        s_ask_pending = true;
        ESP_LOGI(TAG, "wake queued during recorder finalization");
        return;
    }
    recorder_ask();
}

void recorder_ask(void)
{
    if (!s_available) return;
    if (s_asking || s_busy || s_recording) {
        ESP_LOGW(TAG, "busy, cannot ask");
        return;
    }
    screen_wake();
    s_asking = true;
    snprintf(s_ask_url_buf, sizeof(s_ask_url_buf), "%s", s_ask_url);
    show_asking();
    ESP_LOGI(TAG, "ask mode: speak now");
    if (xTaskCreate(ask_task, "ask", 8192, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "ask task create failed");
        s_asking = false;
        show_idle();
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
    show_asking();
    if (xTaskCreate(ptt_capture_task, "ptt", 4096, NULL, 5, NULL) != pdPASS) {
        s_ptt_active = false;
        s_asking = false;
        ESP_LOGE(TAG, "deferred ptt task create failed");
        show_idle();
        ask_resume_recording();
    }
}

void recorder_ptt_begin(void)
{
    if (!s_available || s_asking || menu_active()) return;
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
    s_resume_after_ask = false;
    s_ptt_len = 0;
    s_ptt_active = true;
    if (xTaskCreate(ptt_capture_task, "ptt", 4096, NULL, 5, NULL) != pdPASS) {
        s_ptt_active = false;
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
    vTaskDelay(pdMS_TO_TICKS(150));
    if (!send) {
        ESP_LOGI(TAG, "ptt cancelled (%d bytes)", s_ptt_len);
        s_asking = false;
        show_idle();
        ask_resume_recording();
        return;
    }
    if (s_ptt_len < OUT_BYTES_PER_SEC / 2) {
        ESP_LOGW(TAG, "ptt too short, ignored");
        s_asking = false;
        show_idle();
        ask_resume_recording();
        return;
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
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 20000,   /* 20s：录音8s+上传+服务端MFCC处理，之前8s太短 */
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return false;
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
    return ok;
}

/* ---------------- 按键 ---------------- */

void recorder_toggle_screen(void)
{
    s_screen_off = !s_screen_off;
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
                ESP_LOGI(TAG, "top key short press");
                recorder_request_start();
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
                recorder_toggle_screen();     /* 短按：熄屏/亮屏，录音不受影响 */
            }
        }

        if (was_down && !hold_handled) {
            int64_t held = ms - down_ms;
            if (s_recording) {
                if (held >= 3000) {
                    hold_handled = true;
                    ESP_LOGI(TAG, "bottom key LONG press -> save and stop recording");
                    s_recording = false;
                    screen_wake();
                }
            }
        }
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
            recorder_ptt_begin();
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
                recorder_ptt_end(true);      /* 松手 = 发送 */
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
