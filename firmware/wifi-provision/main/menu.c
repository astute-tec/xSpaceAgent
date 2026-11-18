/*
 * 触摸设置菜单（圆屏 466x466）
 *
 *  主页顶部下滑 → 设置页
 *    ┌ WiFi 图标 ┐  ┌ 声纹图标 ┐
 *    └───────────┘  └───────────┘
 *
 *  WiFi 页：启动配网热点 + 显示二维码（手机扫码连网）
 *  声纹页：5 个名额，点 + 录入（录 6 秒上传服务端提取声纹），录满 5 人不可再加
 */
#include <string.h>
#include <math.h>
#include "esp_timer.h"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "board.h"
#include "display.h"
#include "recorder.h"
#include "mic.h"
#include "afe.h"
#include "menu.h"
#include "font_cn.h"

static const char *TAG = "menu";

typedef enum { PAGE_NONE = 0, PAGE_SETTINGS, PAGE_WIFI, PAGE_VP } page_t;

/* 声纹名额位置（绘制与点击共用的唯一来源） */
static const int VP_X[5] = {150, 316, 150, 316, 233};
static const int VP_Y[5] = {152, 152, 252, 252, 348};
#define VP_R 44
static page_t s_page = PAGE_NONE;
static char s_slots[5][32];       /* 已录入的成员名（空=未录入） */
static int s_enrolling_slot = -1; /* 正在录入的名额 */
static int s_enroll_state = 0;    /* 0空闲 1录音中 2上传中 3成功 4失败 5无网 6删除中 */
static int s_enroll_countdown = 0;
static volatile bool s_enroll_abort = false;
static volatile bool s_vp_task_running = false;
static TaskHandle_t s_vp_task = NULL;
static portMUX_TYPE s_vp_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_hint[64] = "";

/* ---------- 绘图原语 ---------- */

/* 实心圆 */
static void disc(int cx, int cy, int r, uint16_t c)
{
    for (int y = -r; y <= r; y++) {
        int w = 0;
        while ((w + 1) * (w + 1) + y * y <= r * r) w++;
        if (w > 0) ui_fill_rect(cx - w, cy + y, 2 * w + 1, 1, c);
    }
}

/* 圆环 */
static void ring(int cx, int cy, int r, int thick, uint16_t c)
{
    int r_in = r - thick;
    for (int y = -r; y <= r; y++) {
        int w = 0;
        while ((w + 1) * (w + 1) + y * y <= r * r) w++;
        int w_in = 0;
        while ((w_in + 1) * (w_in + 1) + y * y <= r_in * r_in) w_in++;
        if (w > w_in)
            ui_fill_rect(cx - w, cy + y, w - w_in, 1, c),
            ui_fill_rect(cx + w_in + 1, cy + y, w - w_in, 1, c);
    }
}

/* WiFi 图标（三段弧，黑底擦除） */
static void icon_wifi(int cx, int cy, uint16_t c)
{
    ring(cx, cy + 26, 46, 8, c);
    /* 只保留上半弧：下半部分用背景色擦掉 */
    ui_fill_rect(cx - 50, cy + 26, 100, 40, 0x0000);
    ring(cx, cy + 26, 28, 8, c);
    ui_fill_rect(cx - 32, cy + 26, 64, 34, 0x0000);
    disc(cx, cy + 26, 9, c);
}

/* 人像图标 */
static void icon_person(int cx, int cy, uint16_t c)
{
    disc(cx, cy - 18, 24, c);                       /* 头 */
    ui_fill_rect(cx - 44, cy + 6, 88, 12, c);       /* 肩 */
    ui_fill_rect(cx - 34, cy + 18, 68, 10, c);
}

/* 加号 */
static void icon_plus(int cx, int cy, int size, uint16_t c)
{
    int t = size / 5;
    if (t < 4) t = 4;
    ui_fill_rect(cx - size / 2, cy - t / 2, size, t, c);
    ui_fill_rect(cx - t / 2, cy - size / 2, t, size, c);
}

static void clear_screen(void)
{
    display_fill(0x0000);      /* 黑底：省电、与录音界面统一 */
}

/* ---------- 页面 ---------- */

static void draw_settings(void)
{
    clear_screen();
    ui_fill_rect(150, 96, 166, 6, 0x4208);          /* 顶部短横线（暗灰） */
    /* 两个圆钮：白圈白图标 */
    ring(150, 235, 78, 4, 0xFFFF);
    icon_wifi(150, 218, 0xFFFF);
    ring(316, 235, 78, 4, 0xFFFF);
    icon_person(316, 232, 0xFFFF);
    /* 标题与按钮文字 */
    int tw = 4 * UI_FONT_CELL;
    ui_text((LCD_H_RES - tw) / 2, 120, "设置", 0xFFFF, 1);
    tw = 2 * UI_FONT_CELL;
    ui_text(150 - tw / 2, 330, "WiFi", 0xFFFF, 1);
    tw = 2 * UI_FONT_CELL;
    ui_text(316 - tw / 2, 330, "声纹", 0xFFFF, 1);
    /* 底部：关闭条（点击关闭菜单） */
    ui_fill_rect(120, 430, 226, 6, 0x4208);
    display_flush();
}

/* WiFi 页：记忆的网络列表（可点选连接）+ 底部「新配WiFi」 */
static bool s_wifi_qr = false;      /* true = 正在显示二维码 */

static void draw_wifi_list(void)
{
    clear_screen();
    extern int prov_wifi_list(char names[][33], int max);
    extern const char *prov_wifi_current(void);

    int tw = 4 * UI_FONT_CELL;
    ui_text((LCD_H_RES - tw) / 2, 66, "选择网络", 0xFFFF, 1);

    char names[4][33];
    int n = prov_wifi_list(names, 4);
    const char *cur = prov_wifi_current();
    ESP_LOGI(TAG, "wifi list: %d entries, current=%s", n, cur ? cur : "(none)");

    for (int i = 0; i < 4; i++) {
        int cy = 122 + i * 62;
        ring(LCD_H_RES / 2, cy, 26, 2, (i < n) ? 0xFFFF : 0x4208);
        if (i < n) {
            bool conn = cur && strcmp(cur, names[i]) == 0;
            int len = (int)strlen(names[i]);
            int w = len * UI_FONT_CELL;
            ui_text((LCD_H_RES - w) / 2, cy - UI_FONT_CELL / 2, names[i], 0xFFFF, 1);
            if (conn) {
                int cw = 3 * UI_FONT_CELL;
                ui_text(LCD_H_RES / 2 + 120, cy - UI_FONT_CELL / 2, "已连接", 0xFFFF, 1);
                (void)cw;
            }
        } else {
            int w = 2 * UI_FONT_CELL;
            ui_text((LCD_H_RES - w) / 2, cy - UI_FONT_CELL / 2, "--", 0x4208, 1);
        }
    }
    /* 连接状态提示 */
    extern int prov_wifi_status(void);
    extern const char *prov_wifi_target(void);
    int st = prov_wifi_status();
    if (st == 1) {
        char msg[40];
        snprintf(msg, sizeof(msg), "连接中 %s", prov_wifi_target());
        int len = (int)strlen(msg);
        ui_text((LCD_H_RES - len * UI_FONT_CELL) / 2, 330, msg, 0xFFFF, 1);
    } else if (st == 3) {
        const char *msg = "连接失败";
        ui_text((LCD_H_RES - 4 * UI_FONT_CELL) / 2, 330, msg, 0xFFFF, 1);
    }

    /* 底部：新配 WiFi */
    int w = 7 * UI_FONT_CELL;
    ui_text((LCD_H_RES - w) / 2, 392, "新配WiFi", 0xFFFF, 1);
    display_flush();
}

static void draw_wifi(void)
{
    if (s_wifi_qr) {
        extern void prov_menu_show_qr(void);
        prov_menu_show_qr();          /* 画二维码（内部会 flush） */
    } else {
        draw_wifi_list();
    }
}

static void draw_vp(void)
{
    clear_screen();
    for (int i = 0; i < 5; i++) {
        bool filled = (s_slots[i][0] != 0);
        ring(VP_X[i], VP_Y[i], VP_R - 1, 3, filled ? 0xFFFF : 0x4208);
        if (filled) {
            icon_person(VP_X[i], VP_Y[i], 0xFFFF);
            char label[16];
            snprintf(label, sizeof(label), "成员%d", i + 1);
            int tw = 3 * UI_FONT_CELL;
            ui_text(VP_X[i] - tw / 2, VP_Y[i] + 46, label, 0xFFFF, 1);
        } else if (i == s_enrolling_slot) {
            disc(VP_X[i], VP_Y[i], 16, 0xF800);      /* 录入中：红点 */
        } else {
            icon_plus(VP_X[i], VP_Y[i], 40, 0xFFFF);
        }
    }
    /* 标题和操作提示 */
    int tw = 4 * UI_FONT_CELL;
    ui_text((LCD_H_RES - tw) / 2, 76, "声纹管理", 0xFFFF, 1);
    if (s_enroll_state == 1) {
        char msg[32];
        snprintf(msg, sizeof(msg), "请说话 %d", s_enroll_countdown);
        int w = 6 * UI_FONT_CELL;
        ui_text((LCD_H_RES - w) / 2, 408, msg, 0xFFFF, 1);
        ui_text(200, 438, "取消", 0xFFFF, 1);
    } else if (s_enroll_state == 2) {
        ui_text(150, 408, "上传中", 0xFFFF, 1);
    } else if (s_enroll_state == 6) {
        ui_text(150, 408, "删除中", 0xFFFF, 1);
    } else if (s_enroll_state == 3) {
        ui_text(150, 408, "完成", 0xFFFF, 1);
    } else if (s_enroll_state == 5) {
        ui_text(60, 408, "无网络 请先配WiFi", 0xFFFF, 1);
    } else if (s_enroll_state == 4) {
        ui_text(150, 408, "失败", 0xFFFF, 1);
    } else {
        const char *hint = "点击加号录声纹";
        tw = 7 * UI_FONT_CELL;
        ui_text((LCD_H_RES - tw) / 2, 408, hint, 0xFFFF, 1);
    }
    display_flush();
}

void menu_draw(void)
{
    switch (s_page) {
    case PAGE_SETTINGS: draw_settings(); break;
    case PAGE_WIFI:     draw_wifi(); break;
    case PAGE_VP:       draw_vp(); break;
    default: break;
    }
}

/* ---------- 声纹录入任务 ---------- */

static void vp_redraw(void)
{
    if (s_page != PAGE_VP) return;
    if (display_frame_take(100)) {
        menu_draw();
        display_frame_give();
    }
}

static bool vp_claim(int slot, int state)
{
    bool claimed = false;
    taskENTER_CRITICAL(&s_vp_mux);
    if (!s_vp_task_running) {
        s_vp_task_running = true;
        s_enrolling_slot = slot;
        s_enroll_state = state;
        s_enroll_abort = false;
        s_hint[0] = 0;
        claimed = true;
    }
    taskEXIT_CRITICAL(&s_vp_mux);
    return claimed;
}

static void vp_release_immediate(void)
{
    taskENTER_CRITICAL(&s_vp_mux);
    s_vp_task_running = false;
    s_vp_task = NULL;
    s_enrolling_slot = -1;
    s_enroll_state = 0;
    s_enroll_abort = false;
    taskEXIT_CRITICAL(&s_vp_mux);
    vp_redraw();
}

static void vp_finish_task(int final_state, const char *hint, int hold_ms)
{
    s_enroll_state = final_state;
    s_enrolling_slot = -1;
    snprintf(s_hint, sizeof(s_hint), "%s", hint ? hint : "");
    vp_redraw();
    if (hold_ms > 0) vTaskDelay(pdMS_TO_TICKS(hold_ms));
    vp_release_immediate();
    vTaskDelete(NULL);
}

static bool enroll_quality_ok(const int16_t *pcm, int bytes)
{
    int samples = bytes / 2;
    if (samples < 16000 * 4) {
        ESP_LOGW(TAG, "enroll quality: too short samples=%d", samples);
        return false;
    }
    int64_t total_sq = 0;
    int peak = 0;
    int clipped = 0;
    int active_frames = 0;
    int frame_count = 0;
    const int frame_samples = 320; /* 20ms */
    for (int start = 0; start < samples; start += frame_samples) {
        int end = start + frame_samples;
        if (end > samples) end = samples;
        int64_t frame_sq = 0;
        for (int i = start; i < end; i++) {
            int32_t v = pcm[i];
            int a = v < 0 ? -v : v;
            if (a > peak) peak = a;
            if (a >= 32300) clipped++;
            frame_sq += (int64_t)v * v;
        }
        int count = end - start;
        total_sq += frame_sq;
        if (count > 0 && sqrtf((float)frame_sq / count) >= 80.0f) active_frames++;
        frame_count++;
    }
    float rms = sqrtf((float)total_sq / samples);
    float active_ratio = frame_count ? (float)active_frames / frame_count : 0.0f;
    float clip_ratio = (float)clipped / samples;
    ESP_LOGI(TAG, "enroll quality: rms=%.1f peak=%d active=%.2f clip=%.4f",
             rms, peak, active_ratio, clip_ratio);
    return rms >= 45.0f && peak >= 200 && active_ratio >= 0.08f && clip_ratio <= 0.05f;
}

static void enroll_task(void *arg)
{
    int slot = (int)(intptr_t)arg;

    /* ① 先检查网络 */
    extern bool prov_is_connected(void);
    if (!prov_is_connected()) {
        ESP_LOGW(TAG, "enroll skipped: no wifi");
        vp_finish_task(5, "无网络", 2000);
        return;
    }

    /* ② 等录音任务彻底停止（最多 30 秒），避免和 record_task 竞争 AFE。 */
    int guard = 0;
    while (recorder_is_busy() && guard++ < 600 && !s_enroll_abort) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_enroll_abort) {
        vp_finish_task(0, "", 0);
        return;
    }
    if (recorder_is_busy()) {
        ESP_LOGW(TAG, "enroll: recorder still busy after 30s, abort");
        vp_finish_task(4, "录音收尾超时", 2000);
        return;
    }

    /* ③ 分配录音缓冲（8 秒 × 32000 B/s = 256000 字节，存在 PSRAM） */
    const int secs = 8;
    int pcm_bytes = secs * 16000 * 2;
    uint8_t *wav = heap_caps_malloc(pcm_bytes + 44, MALLOC_CAP_SPIRAM);
    if (!wav) {
        ESP_LOGE(TAG, "enroll: no psram for wav buffer");
        vp_finish_task(4, "内存不足", 2000);
        return;
    }

    /* ④ 立即开始录音，同时更新倒计时 UI
     *    原来的逻辑是先倒计时再录，用户说话时根本没在录！
     *    现在：边录边显示「请说话 N」，录满 8 秒后上传。 */
    s_enroll_countdown = secs;
    vp_redraw();   /* 立即显示「请说话 8」 */

    if (!afe_reset_output(1200)) {
        ESP_LOGE(TAG, "enroll: AFE reset timeout");
        free(wav);
        vp_finish_task(4, "音频初始化失败", 2000);
        return;
    }

    int got = 0;
    int64_t start_ms = esp_timer_get_time() / 1000;
    int last_shown = secs;
    const int chunk = 16000 * 2 / 10;  /* 100ms 一块 = 3200 字节 */

    while (got < pcm_bytes && !s_enroll_abort) {
        int want = pcm_bytes - got;
        if (want > chunk) want = chunk;

        /* 读 100ms 音频，afe_read 参数为字节数 */
        bool ok = afe_read((int16_t *)(wav + 44 + got), want);
        if (ok) {
            got += want;
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        /* 每秒更新一次倒计时 */
        int elapsed = (int)((esp_timer_get_time() / 1000 - start_ms) / 1000);
        int remain = secs - elapsed;
        if (remain < 0) remain = 0;
        if (remain != last_shown) {
            last_shown = remain;
            s_enroll_countdown = remain;
            vp_redraw();
        }
    }

    ESP_LOGI(TAG, "enroll slot %d captured %d bytes (%.1f s)",
             slot, got, (float)got / (16000 * 2));

    if (s_enroll_abort) {
        free(wav);
        vp_finish_task(0, "", 0);
        return;
    }

    if (!enroll_quality_ok((const int16_t *)(wav + 44), got)) {
        free(wav);
        vp_finish_task(4, "请靠近并持续说话", 2500);
        return;
    }

    /* ⑤ 写 WAV 头（16k 单声道 16bit） */
    uint32_t dl = (uint32_t)got, rate = 16000;
    memcpy(wav + 0, "RIFF", 4);
    uint32_t riff = 36 + dl; memcpy(wav + 4, &riff, 4);
    memcpy(wav + 8, "WAVEfmt ", 8);
    uint32_t fmt = 16; memcpy(wav + 16, &fmt, 4);
    uint16_t pcm_fmt = 1, ch = 1, bits = 16;
    memcpy(wav + 20, &pcm_fmt, 2);
    memcpy(wav + 22, &ch, 2); memcpy(wav + 24, &rate, 4);
    uint32_t br = rate * 2; memcpy(wav + 28, &br, 4);
    uint16_t ba = 2; memcpy(wav + 32, &ba, 2); memcpy(wav + 34, &bits, 2);
    memcpy(wav + 36, "data", 4); memcpy(wav + 40, &dl, 4);

    /* ⑥ 先落盘（上传失败也不丢样本，后台任务会自动重试） */
    extern esp_err_t queue_save_enroll(int slot, const uint8_t *wav, int len);
    extern void queue_remove_enroll(int slot);
    extern int queue_flush_enroll(void);
    bool saved = (queue_save_enroll(slot, wav, got + 44) == ESP_OK);

    /* ⑦ 立即尝试上传 */
    s_enroll_state = 2;
    vp_redraw();

    char name[32] = {0};
    bool uploaded = recorder_upload_enroll(wav, got + 44, slot, name, sizeof(name));
    free(wav);
    if (uploaded) {
        queue_remove_enroll(slot);
        snprintf(s_slots[slot], sizeof(s_slots[slot]), "%s", name);
        menu_save_slot(slot);
        ESP_LOGI(TAG, "enroll ok slot=%d name=%s", slot, name);
        vp_finish_task(3, "完成", 2000);
    } else if (saved) {
        ESP_LOGW(TAG, "enroll upload failed slot=%d, sample kept for retry", slot);
        vp_finish_task(2, "已保存待重试", 2500);   /* 状态 2 = 上传中样式文字 */
    } else {
        ESP_LOGW(TAG, "enroll failed slot=%d (not saved)", slot);
        vp_finish_task(4, "上传失败", 2000);
    }
    return;
}

static void unenroll_task(void *arg)
{
    int slot = (int)(intptr_t)arg;
    extern bool prov_is_connected(void);
    if (!prov_is_connected()) {
        vp_finish_task(5, "无网络", 2000);
        return;
    }

    if (!recorder_upload_unenroll(slot)) {
        ESP_LOGW(TAG, "delete voiceprint failed, keep local slot %d", slot);
        vp_finish_task(4, "删除失败", 2000);
        return;
    }

    s_slots[slot][0] = 0;
    nvs_handle_t h;
    if (nvs_open("voice", NVS_READWRITE, &h) == ESP_OK) {
        char key[12];
        snprintf(key, sizeof(key), "s%d", slot);
        nvs_erase_key(h, key);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "delete voiceprint slot %d committed locally", slot);
    vp_finish_task(3, "已删除", 1500);
    return;
}


/* ---------- 对外接口 ---------- */

void menu_init(void)
{
    memset(s_slots, 0, sizeof(s_slots));
    /* 从 NVS 读取已录入的成员（名字由服务端分配，本地缓存） */
    extern bool menu_load_slots(void);
    menu_load_slots();
}

void menu_open(void)
{
    display_brightness(0xC0);     /* 确保屏幕亮着 */
    s_page = PAGE_SETTINGS;
    s_hint[0] = 0;
    if (display_frame_take(100)) { menu_draw(); display_frame_give(); }
    ESP_LOGI(TAG, "menu open");
}

/* 由触摸任务周期调用：处理 WiFi 连接结果 */
void menu_poll(void)
{
    if (s_page != PAGE_WIFI || s_wifi_qr) return;
    extern int prov_wifi_status(void);
    int st = prov_wifi_status();
    if (st == 2) {                   /* 连接成功 → 关菜单回待机 */
        extern void prov_wifi_reset_status(void);
        prov_wifi_reset_status();
        menu_close();
    } else if (st == 3) {            /* 失败 → 刷新显示「连接失败」 */
        static int last = 0;
        if (last != 3) { last = 3; if (display_frame_take(100)) { menu_draw(); display_frame_give(); } }
        extern void prov_wifi_reset_status(void);
        prov_wifi_reset_status();
        if (display_frame_take(100)) { menu_draw(); display_frame_give(); }
        last = 0;
    }
}

void menu_close(void)
{
    ESP_LOGW(TAG, "menu_close called");
    extern void prov_menu_hide_qr(void);
    prov_menu_hide_qr();          /* 交还屏幕所有权 */
    s_wifi_qr = false;
    s_page = PAGE_NONE;
    if (display_frame_take(100)) {
        extern void display_idle_ui(void);
        display_idle_ui();
        display_flush();
        display_frame_give();
    }
    ESP_LOGI(TAG, "menu close");
}

bool menu_active(void) { return s_page != PAGE_NONE; }

void menu_set_slot(int slot, const char *name)
{
    if (slot < 0 || slot > 4) return;
    snprintf(s_slots[slot], sizeof(s_slots[slot]), "%s", name ? name : "");
}

const char *menu_get_slot(int slot)
{
    if (slot < 0 || slot > 4) return "";
    return s_slots[slot];
}

/* 长按：已录入的名额 → 删除（本地 + 服务器），变回加号 */
bool menu_long_press(int x, int y)
{
    ESP_LOGI(TAG, "long press (%d,%d) page=%d", x, y, (int)s_page);
    if (s_page != PAGE_VP) return false;
    for (int i = 0; i < 5; i++) {
        int dx = x - VP_X[i], dy = y - VP_Y[i];
        if (dx * dx + dy * dy <= (VP_R + 12) * (VP_R + 12)) {
            if (s_slots[i][0] == 0 || s_vp_task_running) return true;
            ESP_LOGI(TAG, "delete voiceprint slot %d (%s)", i, s_slots[i]);
            if (!vp_claim(i, 6)) return true;
            vp_redraw();
            if (xTaskCreate(unenroll_task, "vpunenroll", 6144,
                            (void *)(intptr_t)i, 5, &s_vp_task) != pdPASS) {
                ESP_LOGE(TAG, "unenroll task create failed");
                vp_release_immediate();
            }
            return true;
        }
    }
    return true;
}

/* 返回 true 表示已消费该点击 */
bool menu_tap(int x, int y)
{
    ESP_LOGI(TAG, "tap (%d,%d) page=%d", x, y, (int)s_page);
    if (s_page == PAGE_NONE) return false;

    if (s_page == PAGE_SETTINGS) {
        /* 关闭区域：底部 */
        if (y > 400) { menu_close(); return true; }
        /* WiFi 圆钮 */
        int dx = x - 150, dy = y - 235;
        if (dx * dx + dy * dy <= 78 * 78) {
            s_page = PAGE_WIFI;
            s_wifi_qr = false;                 /* 默认显示记忆列表 */
            if (display_frame_take(100)) { menu_draw(); display_frame_give(); }
            return true;
        }
        /* 声纹圆钮 */
        dx = x - 316; dy = y - 235;
        if (dx * dx + dy * dy <= 78 * 78) {
            s_page = PAGE_VP;
            if (display_frame_take(100)) { menu_draw(); display_frame_give(); }
            return true;
        }
        return true;
    }

    if (s_page == PAGE_WIFI) {
        if (s_wifi_qr) {                    /* 二维码页：点击返回列表 */
            s_wifi_qr = false;
            if (display_frame_take(100)) { menu_draw(); display_frame_give(); }
            return true;
        }
        extern int prov_wifi_list(char names[][33], int max);
        extern bool prov_wifi_connect_idx(int idx);
        extern void prov_start_ap(void);
        char names[4][33];
        int n = prov_wifi_list(names, 4);
        /* 列表行：点选连接 */
        for (int i = 0; i < n && i < 4; i++) {
            int cy = 122 + i * 62;
            int dx = x - LCD_H_RES / 2, dy = y - cy;
            if (dx * dx + dy * dy <= 40 * 40) {
                ESP_LOGI(TAG, "select wifi #%d %s", i, names[i]);
                prov_wifi_connect_idx(i);
                if (display_frame_take(100)) { menu_draw(); display_frame_give(); }   /* 显示「连接中」 */
                return true;
            }
        }
        /* 底部「新配WiFi」→ 二维码 */
        if (y > 370) {
            ESP_LOGI(TAG, "new wifi -> show QR");
            s_wifi_qr = true;
            if (display_frame_take(100)) { menu_draw(); display_frame_give(); }
            prov_start_ap();
            return true;
        }
        return true;
    }

    if (s_page == PAGE_VP) {
        ESP_LOGI(TAG, "vp tap at (%d,%d)", x, y);
        if (s_vp_task_running) {
            /* 录音阶段只有底部明确区域可取消；上传/删除阶段不允许制造半状态。 */
            if (s_enroll_state == 1 && y > 390) {
                ESP_LOGI(TAG, "explicit enroll cancel requested");
                s_enroll_abort = true;
            } else {
                ESP_LOGI(TAG, "tap ignored while voiceprint task is running");
            }
            return true;
        }
        if (y > 430) { s_page = PAGE_SETTINGS; menu_draw(); return true; }   /* 返回设置 */
        for (int i = 0; i < 5; i++) {
            int dx = x - VP_X[i], dy = y - VP_Y[i];
            if (dx * dx + dy * dy <= (VP_R + 12) * (VP_R + 12)) {
                if (s_slots[i][0]) {
                    ESP_LOGI(TAG, "slot %d already enrolled (%s)", i, s_slots[i]);
                } else {
                    if (!vp_claim(i, 1)) return true;
                    vp_redraw();
                    /* 录音中先停录音，保证能进声纹录入 */
                    extern void recorder_stop(void);
                    if (recorder_is_busy()) {
                        ESP_LOGI(TAG, "stop recording before enroll");
                        recorder_stop();
                    }
                    ESP_LOGI(TAG, "enroll start slot %d", i);
                    if (xTaskCreate(enroll_task, "enroll", 8192,
                                    (void *)(intptr_t)i, 5, &s_vp_task) != pdPASS) {
                        ESP_LOGE(TAG, "enroll task create failed");
                        vp_release_immediate();
                    }
                }
                return true;
            }
        }
        return true;
    }
    return true;
}

/* NVS 存取（名字由服务端分配后本地缓存） */
bool menu_load_slots(void)
{
    nvs_handle_t h;
    if (nvs_open("voice", NVS_READONLY, &h) != ESP_OK) return false;
    for (int i = 0; i < 5; i++) {
        char key[8], val[32] = {0};
        snprintf(key, sizeof(key), "s%d", i);
        size_t len = sizeof(val);
        if (nvs_get_str(h, key, val, &len) == ESP_OK) {
            snprintf(s_slots[i], sizeof(s_slots[i]), "%s", val);
        }
    }
    nvs_close(h);
    return true;
}

void menu_save_slot(int slot)
{
    nvs_handle_t h;
    if (nvs_open("voice", NVS_READWRITE, &h) != ESP_OK) return;
    char key[8];
    snprintf(key, sizeof(key), "s%d", slot);
    nvs_set_str(h, key, s_slots[slot]);
    nvs_commit(h);
    nvs_close(h);
}
