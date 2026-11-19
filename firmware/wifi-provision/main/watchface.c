#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "board.h"
#include "display.h"
#include "font_cn.h"
#include "watchface.h"

#define CX (LCD_H_RES / 2)
#define CY 226
#define COLOR_WHITE 0xFFFF
#define COLOR_MUTED 0x5AEB
#define COLOR_TEAL 0x07FF
#define COLOR_ORANGE 0xFD20
#define COLOR_RED 0xF800

static portMUX_TYPE s_answer_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_answer[WATCHFACE_ANSWER_BYTES];
static int64_t s_answer_started;
static bool s_recording_hint;
typedef struct { int rssi; int percent; bool connected; bool charging; bool external; } face_status_t;
static face_status_t s_status={.rssi=-127,.percent=-1};
static unsigned s_status_version;
void watchface_set_status(bool connected,int rssi,int percent,bool charging,bool external_power)
{
    if (percent<0 || percent>100) percent=-1;
    taskENTER_CRITICAL(&s_answer_mux);
    if (s_status.connected!=connected || s_status.rssi!=rssi || s_status.percent!=percent ||
        s_status.charging!=charging || s_status.external!=external_power) {
        s_status=(face_status_t){rssi,percent,connected,charging,external_power};
        s_status_version++;
    }
    taskEXIT_CRITICAL(&s_answer_mux);
}
void watchface_set_recording(bool active) { s_recording_hint = active; }

void watchface_set_answer(const char *text)
{
    if (!text) text = "";
    char normalized[WATCHFACE_ANSWER_BYTES];
    size_t len = strnlen(text, sizeof(normalized) - 1);
    if (text[len]) while (len && ((unsigned char)text[len] & 0xc0) == 0x80) len--;
    memcpy(normalized, text, len);
    normalized[len] = 0;
    for (size_t i = 0; i < len; i++)
        if ((unsigned char)normalized[i] < 0x20) normalized[i] = ' ';
    taskENTER_CRITICAL(&s_answer_mux);
    if (strcmp(s_answer, normalized)) {
        memcpy(s_answer, normalized, len + 1);
        s_answer_started = esp_timer_get_time() / 1000;
    }
    taskEXIT_CRITICAL(&s_answer_mux);
}

bool watchface_has_answer(void)
{
    taskENTER_CRITICAL(&s_answer_mux);
    bool present = s_answer[0] != 0;
    taskEXIT_CRITICAL(&s_answer_mux);
    return present;
}

/* Three lines above six o'clock; pause at both ends and repeat. */
static bool draw_answer(void)
{
    char text[WATCHFACE_ANSWER_BYTES];
    int64_t started;
    taskENTER_CRITICAL(&s_answer_mux);
    memcpy(text, s_answer, sizeof(text));
    started = s_answer_started;
    taskEXIT_CRITICAL(&s_answer_mux);
    if (!text[0]) return false;
    uint16_t lines[173];
    uint16_t lengths[173];
    int n = 0, cols = 0;
    const char *begin = text, *p = text;
    while (*p && n < 172) {
        int bytes = 1;
        while (p[bytes] && ((unsigned char)p[bytes] & 0xc0) == 0x80) bytes++;
        p += bytes;
        if (++cols == 9) {
            lines[n] = (uint16_t)(begin - text); lengths[n++] = (uint16_t)(p - begin);
            begin = p; cols = 0;
        }
    }
    if (p > begin && n < 173) {
        lines[n] = (uint16_t)(begin - text); lengths[n++] = (uint16_t)(p - begin);
    }
    int top = 0;
    int offset = 0;
    if (n > 3) {
        int64_t elapsed = esp_timer_get_time() / 1000 - started;
        int64_t cycle = 4000 + (n - 3) * 2800LL + 4000;
        int64_t t = elapsed % cycle;
        int distance = t < 4000 ? 0 : (int)((t - 4000) / 100);
        if (distance > (n - 3) * 28) distance = (n - 3) * 28;
        top = distance / 28;
        offset = distance % 28;
    }
    for (int row = 0; row < 4 && top + row < n; row++) {
        char line_text[49];
        int len = lengths[top + row];
        if (len > 48) len = 48;
        memcpy(line_text, text + lines[top + row], len); line_text[len] = 0;
        ui_text_clipped(CX - 108, 300 + row * 28 - offset, line_text, COLOR_WHITE, 1, 300, 384);
    }
    return true;
}

static uint16_t *pixels(void)
{
    return display_fb();
}

static void pixel(int x, int y, uint16_t color)
{
    if (x < 0 || x >= LCD_H_RES || y < 0 || y >= LCD_V_RES) return;
    uint16_t *fb = pixels();
    if (fb) fb[(size_t)y * LCD_H_RES + x] = __builtin_bswap16(color);
}

static void line(int x0, int y0, int x1, int y1, uint16_t color, int width)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        for (int oy = -width / 2; oy <= width / 2; oy++)
            for (int ox = -width / 2; ox <= width / 2; ox++) pixel(x0 + ox, y0 + oy, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void draw_mic(int cx, int cy, uint16_t color, int pulse)
{
    int r = 19 + pulse;
    for (int y = -22; y <= 22; y++) {
        int x2 = r * r - y * y;
        if (x2 <= 0) continue;
        int x = (int)sqrtf((float)x2);
        pixel(cx - x, cy + y, color);
        pixel(cx + x, cy + y, color);
    }
    for (int x = -12; x <= 12; x++) {
        pixel(cx + x, cy - 20, color);
        pixel(cx + x, cy + 20, color);
    }
    for (int deg = 0; deg <= 180; deg++) {
        float a = (float)deg * 0.0174532925f;
        pixel(cx + (int)(25 * cosf(a)), cy + (int)(25 * sinf(a)), color);
    }
    line(cx, cy + 25, cx, cy + 39, color, 3);
    line(cx - 13, cy + 39, cx + 13, cy + 39, color, 3);
}

static void draw_status(face_status_t status)
{
    const int x=CX-84,y=94;
    ui_fill_rect(CX-110,78,220,48,0);
    int bars=status.rssi>=-55 ? 4 : status.rssi>=-67 ? 3 : status.rssi>=-78 ? 2 : 1;
    for (int i=0;i<4;i++) {
        int h=5+i*5;
        ui_fill_rect(x+i*8,y+20-h,5,h,status.connected && i<bars ? COLOR_TEAL : COLOR_MUTED);
    }
    if (!status.connected) line(x-2,y-2,x+31,y+22,COLOR_RED,2);
    int bx=CX-15;
    uint16_t color=status.percent>=0 && status.percent<=15 ? COLOR_RED : COLOR_WHITE;
    line(bx,y,bx+30,y,color,2); line(bx,y+19,bx+30,y+19,color,2);
    line(bx,y,bx,y+19,color,2); line(bx+30,y,bx+30,y+19,color,2);
    ui_fill_rect(bx+32,y+6,3,8,color);
    if (status.percent>=0) ui_fill_rect(bx+3,y+3,status.percent*25/100,14,
                                      status.charging ? COLOR_TEAL : color);
    if (status.charging || status.external) {
        int cx=bx+14;
        line(cx+2,y-4,cx-3,y+8,COLOR_ORANGE,2);
        line(cx-3,y+8,cx+4,y+8,COLOR_ORANGE,2);
        line(cx+4,y+8,cx-1,y+23,COLOR_ORANGE,2);
    }
    char percent[16];
    if (status.percent<0) snprintf(percent,sizeof(percent),"--%%");
    else snprintf(percent,sizeof(percent),"%d%%",status.percent);
    ui_text(bx+44,y-3,percent,color,1);
}

static float level_db(uint16_t raw)
{
    float db = 20.0f * log10f(((float)raw + 1.0f) / 32768.0f);
    float amp = (db + 68.0f) / 52.0f;
    if (amp < 0.04f) amp = 0.04f;
    if (amp > 1.0f) amp = 1.0f;
    return amp;
}

static void draw_wave(const uint16_t *levels, int count)
{
    const int x0 = CX - 82, span = 164, samples = 48;
    float ys[51];
    for (int i = 0; i < samples + 3; i++) {
        int src = count > 1 ? (i * (count - 1)) / (samples + 2) : 0;
        if (src >= count) src = count - 1;
        ys[i] = 350.0f - (count > 0 ? level_db(levels[src]) * 38.0f : 2.0f);
    }
    for (int i = 0; i < samples; i++) {
        float p0 = ys[i], p1 = ys[i + 1], p2 = ys[i + 2], p3 = ys[i + 3];
        int xa = x0 + i * span / samples;
        int xb = x0 + (i + 1) * span / samples;
        int steps = 4;
        int prev_y = (int)p1;
        for (int j = 1; j <= steps; j++) {
            float t = (float)j / steps;
            float y = 0.5f * ((2 * p1) + (-p0 + p2) * t +
                              (2 * p0 - 5 * p1 + 4 * p2 - p3) * t * t +
                              (-p0 + 3 * p1 - 3 * p2 + p3) * t * t * t);
            int x = xa + (xb - xa) * j / steps;
            int iy = (int)y;
            line(xa + (xb - xa) * (j - 1) / steps, prev_y, x, iy, COLOR_TEAL, 3);
            prev_y = iy;
        }
    }
}

static void draw_ticks(void)
{
    for (int i = 0; i < 60; i++) {
        float a = (float)i * 0.104719755f - 1.570796327f;
        int r0 = (i % 5 == 0) ? 187 : 193;
        int r1 = 202;
        uint16_t c = (i % 5 == 0) ? COLOR_WHITE : COLOR_MUTED;
        line(CX + (int)(r0 * cosf(a)), CY + (int)(r0 * sinf(a)),
             CX + (int)(r1 * cosf(a)), CY + (int)(r1 * sinf(a)), c,
             (i % 5 == 0) ? 3 : 1);
    }
}

static void draw_face_base(void)
{
    display_fill(0x0000);
    draw_ticks();
    time_t now = time(NULL);
    struct tm tm_now = {0};
    localtime_r(&now, &tm_now);
    char date[16];
    snprintf(date, sizeof(date), "%02d.%02d", tm_now.tm_mon + 1, tm_now.tm_mday);
    int dw = 5 * UI_FONT_CELL;
    ui_text((LCD_H_RES - dw) / 2, 274, date, COLOR_MUTED, 1);
}

static void draw_digit(int x, int y, int digit, uint16_t color)
{
    static const uint8_t segments[10] = {
        0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f
    };
    const int w = 48, h = 76, t = 6;
    uint8_t m = segments[digit % 10];
    if (m & 0x01) ui_fill_rect(x + t, y, w - 2 * t, t, color);
    if (m & 0x02) ui_fill_rect(x + w - t, y + t, t, h / 2 - t, color);
    if (m & 0x04) ui_fill_rect(x + w - t, y + h / 2, t, h / 2 - t, color);
    if (m & 0x08) ui_fill_rect(x + t, y + h - t, w - 2 * t, t, color);
    if (m & 0x10) ui_fill_rect(x, y + h / 2, t, h / 2 - t, color);
    if (m & 0x20) ui_fill_rect(x, y + t, t, h / 2 - t, color);
    if (m & 0x40) ui_fill_rect(x + t, y + h / 2 - t / 2, w - 2 * t, t, color);
}

void watchface_draw(watchface_state_t state, const uint16_t *levels, int count, int progress_ms)
{
    static time_t last_minute = -1;
    static uint32_t last_generation;
    static unsigned last_status_version=~0U;
    face_status_t status; unsigned status_version;
    taskENTER_CRITICAL(&s_answer_mux);
    status=s_status; status_version=s_status_version;
    taskEXIT_CRITICAL(&s_answer_mux);
    time_t now = time(NULL);
    bool full = last_minute != now / 60 ||
                last_generation != display_full_generation();
    if (full) draw_face_base();
    else {
        ui_fill_rect(CX - 120, 300, 240, 132, 0x0000);
        draw_ticks(); /* Restore static ticks intersecting the dynamic rectangle. */
    }
    bool status_changed=full || last_status_version!=status_version;
    if (status_changed) draw_status(status);
    struct tm tm_now = {0};
    localtime_r(&now, &tm_now);
    int x = (LCD_H_RES - 242) / 2;
    draw_digit(x, 178, tm_now.tm_hour / 10, COLOR_WHITE);
    draw_digit(x + 55, 178, tm_now.tm_hour % 10, COLOR_WHITE);
    ui_fill_rect(x + 115, 198, 6, 6, COLOR_ORANGE);
    ui_fill_rect(x + 115, 226, 6, 6, COLOR_ORANGE);
    draw_digit(x + 139, 178, tm_now.tm_min / 10, COLOR_WHITE);
    draw_digit(x + 194, 178, tm_now.tm_min % 10, COLOR_WHITE);

    bool answer = state != WATCHFACE_LISTENING && state != WATCHFACE_STOPPING && draw_answer();
    if (answer) {
        const char *label = state == WATCHFACE_PLAYING ? "回答中"
            : (state == WATCHFACE_ERROR ? "语音失败"
            : ((state == WATCHFACE_RECORDING || s_recording_hint) ? "录音中" : "唐僧答复"));
        int cells = (state == WATCHFACE_PLAYING || state == WATCHFACE_RECORDING ||
                     (s_recording_hint && state != WATCHFACE_ERROR)) ? 3 : 4;
        ui_text(CX - UI_FONT_CELL * cells / 2, 405, label, COLOR_TEAL, 1);
    } else if (state == WATCHFACE_IDLE) {
        draw_mic(CX, 347, COLOR_MUTED, 0);
        ui_text(CX - UI_FONT_CELL, 405, "待机", COLOR_MUTED, 1);
    } else if (state == WATCHFACE_RECORDING) {
        if (levels && count > 0) draw_wave(levels, count);
        ui_text(CX - UI_FONT_CELL * 3 / 2, 405, "录音中", COLOR_ORANGE, 1);
        pixel(CX - 42, 411, COLOR_RED);
    } else if (state == WATCHFACE_LISTENING) {
        int pulse = count > 0 ? levels[count - 1] / 400 : 0;
        if (pulse > 6) pulse = 6;
        draw_mic(CX, 345, COLOR_TEAL, pulse);
        ui_text(CX - UI_FONT_CELL * 3 / 2, 405, "聆听中", COLOR_TEAL, 1);
    } else if (state == WATCHFACE_STOPPING) {
        ui_fill_rect(CX - 13, 331, 8, 28, COLOR_ORANGE);
        ui_fill_rect(CX + 5, 331, 8, 28, COLOR_ORANGE);
        ui_fill_rect(CX - 82, 377, 164, 5, COLOR_MUTED);
        int fill = progress_ms * 164 / 3000;
        if (fill < 0) fill = 0;
        if (fill > 164) fill = 164;
        ui_fill_rect(CX - 82, 377, fill, 5, COLOR_ORANGE);
        char remain[8];
        int seconds = (3000 - progress_ms + 999) / 1000;
        if (seconds < 0) seconds = 0;
        snprintf(remain, sizeof(remain), "%d", seconds);
        ui_text(CX - UI_FONT_CELL / 2, 396, remain, COLOR_WHITE, 1);
    } else {
        draw_mic(CX, 345, state == WATCHFACE_PLAYING ? COLOR_ORANGE : COLOR_TEAL, 0);
        const char *label = state == WATCHFACE_PLAYING ? "回答中"
            : (state == WATCHFACE_ERROR ? "问答失败请重试"
            : (state == WATCHFACE_SENDING ? "发送中"
            : (state == WATCHFACE_RECOGNIZING ? "识别中"
            : (state == WATCHFACE_TANGSENG ? "等唐僧答复"
            : (state == WATCHFACE_TTS ? "合成语音中"
            : (state == WATCHFACE_OFFLINE ? "无网络" : "思考中"))))));
        int n = state == WATCHFACE_ERROR ? 7 :
            ((state == WATCHFACE_TANGSENG || state == WATCHFACE_TTS) ? 5 : 3);
        if (s_recording_hint && state != WATCHFACE_PLAYING) {
            char combined[64];
            snprintf(combined, sizeof(combined), "录音·%s", label);
            ui_text(CX - UI_FONT_CELL * (n + 3) / 2, 405, combined, COLOR_WHITE, 1);
        } else ui_text(CX - UI_FONT_CELL * n / 2, 405, label, COLOR_WHITE, 1);
    }
    bool status_ok=full || !status_changed || display_flush_rows(78,126);
    if (status_ok && display_flush_rows(full ? 0 : 300, full ? LCD_V_RES : 432)) {
        last_minute = now / 60;
        last_generation = display_full_generation();
        last_status_version=status_version;
    } else {
        last_minute = -1;
    }
}
