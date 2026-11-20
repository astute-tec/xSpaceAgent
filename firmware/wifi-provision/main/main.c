/*
 * 扫码配网固件 —— Waveshare ESP32-S3-Touch-AMOLED-1.75C
 *
 * 流程：开机 → 起热点 + 屏幕显示 WiFi 加入二维码 → 手机扫码入网
 *      → 强制门户选 WiFi 输密码 → 设备连上 → 绿屏
 *
 * 只做配网，不含语音功能（后续主固件复用本文件里的配网组件）。
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <dirent.h>
#include "esp_mac.h"
#include "driver/usb_serial_jtag.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "blufi_prov.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "blufi_prov.h"

#include "qrcodegen.h"
#include "font_cn.h"
#include "board.h"
#include "display.h"
#include "mic.h"
#include "afe.h"
#include "recorder.h"
#include "watchface.h"

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static const char *TAG = "prov";

static void random_code(char *out, int n);
static void nvs_save(const char *ssid, const char *pass);

#define WIFI_CONNECT_BIT BIT0
#define WIFI_FAIL_BIT    BIT1
#define WIFI_READY_BIT   BIT2

static EventGroupHandle_t s_evt;
static SemaphoreHandle_t s_lock;

static char s_ap_ssid[33];
static char s_pair_code[8];

static volatile int s_state;          /* 0=ap 1=connecting 2=connected 3=failed */
static char s_ip[16];
static char s_err[64];
static volatile int s_reason;
static volatile bool s_connected;
static bool s_first_boot_prov;

static char s_new_ssid[33];
static char s_new_pass[65];
static volatile bool s_has_pending;
static volatile bool s_connect_task_running;
static volatile bool s_wifi_started;
static volatile bool s_ap_active;
static volatile int s_retry_failures;
static volatile int64_t s_ignore_disconnect_until_us;
static volatile int64_t s_next_retry_ms;
static volatile int s_wifi_conn_state;  /* 0 空闲 / 1 连接中 / 2 成功 / 3 失败 */
static char s_wifi_target[33];

static void wifi_schedule_connect(const char *ssid, const char *pass);
static void wifi_creds_add_ex(const char *ssid, const char *pass, bool mark_last);

/* ---------------- 绘图 ---------------- */

static inline void fill_rect(int x, int y, int w, int h, uint16_t c)
{
    uint16_t *fb = display_fb();
    if (!fb) return;
    /* 面板按大端读取 RGB565，ESP32 内存是小端 → 交换字节，否则红变蓝/青变黄 */
    c = (uint16_t)__builtin_bswap16(c);
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_H_RES) w = LCD_H_RES - x;
    if (y + h > LCD_V_RES) h = LCD_V_RES - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) {
        uint16_t *row = fb + (size_t)(y + j) * LCD_H_RES + x;
        for (int i = 0; i < w; i++) row[i] = c;
    }
}

/* 七段数码管（指定颜色）：a 上, b 右上, c 右下, d 下, e 左下, f 左上, g 中 */
static void draw_seg_digit_c(int x, int y, int w, int h, int t, int mask, uint16_t color)
{
    if (mask & 0x01) fill_rect(x, y, w, t, color);
    if (mask & 0x02) fill_rect(x + w - t, y, t, h / 2, color);
    if (mask & 0x04) fill_rect(x + w - t, y + h / 2, t, h / 2, color);
    if (mask & 0x08) fill_rect(x, y + h - t, w, t, color);
    if (mask & 0x10) fill_rect(x, y + h / 2, t, h / 2, color);
    if (mask & 0x20) fill_rect(x, y, t, h / 2, color);
    if (mask & 0x40) fill_rect(x, y + h / 2 - t / 2, w, t, color);
}



static const uint8_t SEG_MAP[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F
};

/* 居中显示一个数字（用于把错误原因码画在红屏上） */
static void draw_number_center(int value, int digits, int y, int dw, int dh, int gap, int t, uint16_t color)
{
    char buf[12];
    snprintf(buf, sizeof(buf), "%0*d", digits, value);
    int total = digits * dw + (digits - 1) * gap;
    int x = (LCD_H_RES - total) / 2;
    for (int i = 0; i < digits; i++) {
        int d = buf[i] - '0';
        if (d < 0 || d > 9) d = 0;
        draw_seg_digit_c(x + i * (dw + gap), y, dw, dh, t, SEG_MAP[d], color);
    }
}

static void draw_pair_code(const char *code)
{
    int n = (int)strlen(code);
    if (n <= 0) return;
    int dw = 28, dh = 52, gap = 10, t = 8;
    int total = n * dw + (n - 1) * gap;
    int x = (LCD_H_RES - total) / 2;
    int y = 376;
    for (int i = 0; i < n; i++) {
        int d = code[i] - '0';
        if (d < 0 || d > 9) d = 0;
        /* 黑底上画白字（二维码卡片在 y<368 区域，配对码在其下方） */
        draw_seg_digit_c(x + i * (dw + gap), y, dw, dh, t, SEG_MAP[d], 0xFFFF);
    }
}



/* 成功界面：白底 + 黑色大勾（黑白色不受 RGB565 字节序影响） */

void prov_show_qr(void);
void prov_menu_show_qr(void);
void prov_start_ap(void);
bool menu_active(void);
void menu_close(void);

/* ---- UI 仲裁：屏幕只允许一个"主人"绘制 ----
 * 录音任务 / 设置菜单 / 配网页面 三者互斥；
 * 连接 WiFi 的后台流程绝不能覆盖正在显示的录音或菜单界面。 */
static volatile bool s_prov_ui = false;   /* 配网页面正在显示 */

bool prov_ui_active(void) { return s_prov_ui; }

/* ---- 配网入口（HTTP 网页 + BLE BluFi 共用） ---- */
static char s_ble_ssid[33];
static char s_ble_pass[65];

/* ssid/pass 传 NULL 表示"暂存"（BluFi 分两条消息到达） */
void prov_submit_credentials(const char *ssid, const char *pass)
{
    if (ssid) {
        strncpy(s_ble_ssid, ssid, sizeof(s_ble_ssid) - 1);
        s_ble_ssid[sizeof(s_ble_ssid) - 1] = 0;
    }
    if (pass) {
        strncpy(s_ble_pass, pass, sizeof(s_ble_pass) - 1);
        s_ble_pass[sizeof(s_ble_pass) - 1] = 0;
    }
    if (s_ble_ssid[0] && pass) {      /* SSID 和密码都到齐 → 落盘 + 提交连接 */
        ESP_LOGI(TAG, "prov credentials: ssid=\"%s\"", s_ble_ssid);
        wifi_creds_add_ex(s_ble_ssid, s_ble_pass, false);
        wifi_schedule_connect(s_ble_ssid, s_ble_pass);
        s_ble_ssid[0] = 0;
        s_ble_pass[0] = 0;
    }
}

bool prov_is_connected(void) { return s_connected; }
const char *prov_ap_ssid(void) { return s_ap_ssid; }


void display_idle_ui(void);

/* 配网页（极简）：白卡二维码居中 + 下方 6 位配对码 */
static void draw_qr_screen(const char *wifi_qr_text, const char *code)
{
    uint16_t *fb = display_fb();
    if (!fb) return;
    for (int i = 0; i < LCD_H_RES * LCD_V_RES; i++) fb[i] = 0x0000;

    /* 白色圆角卡片托住二维码 */
    const int card_cy = 196, card_r = 168;
    for (int y = -card_r; y <= card_r; y++) {
        int w = 0;
        while ((w + 1) * (w + 1) + y * y <= card_r * card_r) w++;
        if (w > 0) fill_rect(LCD_H_RES / 2 - w, card_cy + y, 2 * w + 1, 1, 0xFFFF);
    }

    static uint8_t qr[qrcodegen_BUFFER_LEN_MAX];
    static uint8_t tmp[qrcodegen_BUFFER_LEN_MAX];
    bool ok = qrcodegen_encodeText(wifi_qr_text, tmp, qr, qrcodegen_Ecc_MEDIUM,
                                   qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                                   qrcodegen_Mask_AUTO, true);
    if (!ok) {
        ESP_LOGE(TAG, "qr encode failed");
        display_flush();
        return;
    }
    int size = qrcodegen_getSize(qr);
    const int BOX = 272;                       /* 二维码 272x272 */
    int total = size + 8;
    int scale = BOX / total;
    if (scale < 1) scale = 1;
    int px = scale * total;
    int ox = (LCD_H_RES - px) / 2;
    int oy = card_cy - px / 2;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            if (!qrcodegen_getModule(qr, x, y)) continue;
            fill_rect(ox + (x + 4) * scale, oy + (y + 4) * scale, scale, scale, 0x0000);
        }
    }
    /* 配对码：白色七段数字，居中在二维码下方 */
    draw_pair_code(code);
    display_flush();
    ESP_LOGI(TAG, "QR drawn (%d modules, scale %d) pair=%s", size, scale, code);
}

void ui_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    fill_rect(x, y, w, h, color);
}

void ui_number(int value, int digits, int y, int dw, int dh, int gap, int t, uint16_t color)
{
    draw_number_center(value, digits, y, dw, dh, gap, t, color);
}

void prov_show_qr(void)
{
    /* 首次开机（从没配过 WiFi）→ 直接显示配网二维码；否则进待机 */
    if (s_first_boot_prov) {
        prov_start_ap();
    } else {
        display_idle_ui();
    }
}

/* ---------------- 工具 ---------------- */

static void random_digits(char *out, int n)
{
    for (int i = 0; i < n; i++) out[i] = '0' + (esp_random() % 10);
    out[n] = 0;
}

/* 配对码首位非 0，避免七段显示时被忽略 */
static void random_code(char *out, int n)
{
    out[0] = '1' + (esp_random() % 9);
    random_digits(out + 1, n - 1);
}

static void url_decode(char *dst, const char *src, size_t dstlen)
{
    size_t di = 0;
    for (size_t si = 0; src[si] && di + 1 < dstlen; si++) {
        char c = src[si];
        if (c == '+') {
            dst[di++] = ' ';
        } else if (c == '%' && src[si + 1] && src[si + 2]) {
            char hex[3] = {src[si + 1], src[si + 2], 0};
            dst[di++] = (char)strtol(hex, NULL, 16);
            si += 2;
        } else {
            dst[di++] = c;
        }
    }
    dst[di] = 0;
}

/* 从 body 里取 key=value */
static bool form_get(const char *body, const char *key, char *out, size_t outlen)
{
    char pat[32];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char *p = strstr(body, pat);
    if (!p) return false;
    p += strlen(pat);
    const char *end = strchr(p, '&');
    size_t n = end ? (size_t)(end - p) : strlen(p);
    char raw[512];
    if (n >= sizeof(raw)) n = sizeof(raw) - 1;
    memcpy(raw, p, n);
    raw[n] = 0;
    url_decode(out, raw, outlen);
    return true;
}

/* ---------------- DNS 劫持 ---------------- */

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { vTaskDelete(NULL); return; }
    struct sockaddr_in addr = {0}, from;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(53);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "dns bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    uint8_t buf[512];
    for (;;) {
        socklen_t fl = sizeof(from);
        int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (len < 12) continue;
        uint8_t resp[512];
        memcpy(resp, buf, len);
        resp[2] = 0x81; resp[3] = 0x80;          /* 标准响应 */
        resp[6] = 0x00; resp[7] = 0x01;          /* ANCOUNT = 1 */
        resp[8] = resp[9] = resp[10] = resp[11] = 0;
        int o = len;
        const uint8_t ans[] = {
            0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01,
            0x00, 0x00, 0x00, 0x3C, 0x00, 0x04,
            192, 168, 4, 1
        };
        if (o + (int)sizeof(ans) < (int)sizeof(resp)) {
            memcpy(resp + o, ans, sizeof(ans));
            o += sizeof(ans);
            sendto(sock, resp, o, 0, (struct sockaddr *)&from, fl);
        }
    }
}

/* ---------------- WiFi ---------------- */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            s_wifi_started = true;
            xEventGroupSetBits(s_evt, WIFI_READY_BIT);
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            s_connected = false;
            s_ip[0] = 0;
            wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
            if (esp_timer_get_time() < s_ignore_disconnect_until_us) {
                ESP_LOGI(TAG, "STA intentional disconnect, reason=%d", d->reason);
                return;
            }
            ESP_LOGW(TAG, "STA disconnected, reason=%d", d->reason);
            if (s_state == 1) {
                s_reason = d->reason;
                snprintf(s_err, sizeof(s_err), "reason=%d", d->reason);
                /* 连接任务等待 GOT_IP 或本轮超时。瞬时断开不再提前终止本轮。 */
            } else {
                s_state = 0;
                s_wifi_conn_state = 0;
                s_next_retry_ms = esp_timer_get_time() / 1000 + 1000;
            }
        } else if (id == WIFI_EVENT_AP_STACONNECTED) {
            ESP_LOGI(TAG, "phone joined AP");
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        /* 用阿里/腾讯公共 DNS（默认路由器 DNS 解析新域名会失败） */
        {
            esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (sta) {
                esp_netif_dns_info_t dns = {0};
                dns.ip.type = ESP_IPADDR_TYPE_V4;
                dns.ip.u_addr.ip4.addr = ipaddr_addr("223.5.5.5");
                esp_netif_set_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns);
                dns.ip.u_addr.ip4.addr = ipaddr_addr("119.29.29.29");
                esp_netif_set_dns_info(sta, ESP_NETIF_DNS_BACKUP, &dns);
                ESP_LOGI(TAG, "DNS set to 223.5.5.5 / 119.29.29.29");
            }
        }
        /* 校时（HTTPS 证书校验需要正确时间） */
        {
            extern void prov_sntp_start(void);
            prov_sntp_start();
        }
        s_state = 2;
        s_connected = true;
        xEventGroupSetBits(s_evt, WIFI_CONNECT_BIT);
        ESP_LOGI(TAG, "got ip: %s", s_ip);
    }
}

static void log_scan_results(void)
{
    wifi_scan_config_t sc = {0};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        ESP_LOGW(TAG, "scan failed");
        return;
    }
    static wifi_ap_record_t recs[24];
    uint16_t n = 24;
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK || n == 0) {
        ESP_LOGW(TAG, "scan: 0 AP");
        return;
    }
    ESP_LOGI(TAG, "scan: %d APs visible", n);
    for (int i = 0; i < n; i++) {
        ESP_LOGI(TAG, "  [%02d] ssid=\"%s\" rssi=%d ch=%d auth=%d",
                 i, (char *)recs[i].ssid, recs[i].rssi, recs[i].primary, recs[i].authmode);
    }
}

/* 幂等 WiFi 初始化：netif/事件循环/wifi_init 只做一次，重复调用不会断言 */
static void wifi_ensure_init(void)
{
    static bool inited = false;
    static esp_netif_t *ap_netif = NULL;
    static esp_netif_t *sta_netif = NULL;
    if (inited) return;

    esp_netif_init();
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    if (!sta_netif) sta_netif = esp_netif_create_default_wifi_sta();
    if (!ap_netif)  ap_netif  = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        wifi_event_handler, NULL, NULL));
    inited = true;
    ESP_LOGI(TAG, "wifi stack ready");
}

/* ---------- 多 WiFi 记忆 ---------- */
#define WIFI_MAX_CREDS 4
static char s_cur_ssid[33];      /* 当前已连接的 SSID */

static void wifi_creds_add_ex(const char *ssid, const char *pass, bool mark_last)
{
    if (!ssid || !ssid[0]) return;
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;
    uint8_t n = 0;
    nvs_get_u8(h, "n", &n);
    char keys[8][12];
    int found = -1;
    for (int i = 0; i < n && i < WIFI_MAX_CREDS; i++) {
        snprintf(keys[i], sizeof(keys[i]), "s%d", (int)i);
        
        char ss[33] = {0};
        size_t ls = sizeof(ss);
        if (nvs_get_str(h, keys[i], ss, &ls) == ESP_OK && strcmp(ss, ssid) == 0) found = i;
    }
    if (found >= 0) {
        char key[12];
        snprintf(key, sizeof(key), "p%d", (int)found);
        nvs_set_str(h, key, pass ? pass : "");
    } else {
        if (n >= WIFI_MAX_CREDS) {          /* 满了：挤掉最早的 */
            for (int i = 1; i < WIFI_MAX_CREDS; i++) {
                char ss[33] = {0}, pp[65] = {0};
                size_t ls = sizeof(ss), lp = sizeof(pp);
                char k1[12], k2[12];
                snprintf(k1, sizeof(k1), "s%d", (int)i);
                snprintf(k2, sizeof(k2), "p%d", (int)i);
                nvs_get_str(h, k1, ss, &ls);
                nvs_get_str(h, k2, pp, &lp);
                char nk1[12], nk2[12];
                snprintf(nk1, sizeof(nk1), "s%u", (unsigned)(i - 1));
                snprintf(nk2, sizeof(nk2), "p%u", (unsigned)(i - 1));
                nvs_set_str(h, nk1, ss);
                nvs_set_str(h, nk2, pp);
            }
            n = WIFI_MAX_CREDS - 1;
        }
        char k1[12], k2[12];
        snprintf(k1, sizeof(k1), "s%d", (int)n);
        snprintf(k2, sizeof(k2), "p%d", (int)n);
        nvs_set_str(h, k1, ssid);
        nvs_set_str(h, k2, pass ? pass : "");
        n++;
        nvs_set_u8(h, "n", n);
    }
    /* 兼容旧字段（仅表示"上次成功连接的网络"） */
    if (mark_last) {
        nvs_set_str(h, "ssid", ssid);
        nvs_set_str(h, "pass", pass ? pass : "");
    }
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "wifi creds stored: %s (total %d, mark_last=%d)", ssid, n, (int)mark_last);
}

/* 读取记忆列表；返回数量 */
int prov_wifi_list(char names[][33], int max)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return 0;
    uint8_t n = 0;
    nvs_get_u8(h, "n", &n);
    int cnt = 0;
    for (int i = 0; i < n && cnt < max; i++) {
        char k[12], ss[33] = {0};
        snprintf(k, sizeof(k), "s%d", (int)i);
        size_t ls = sizeof(ss);
        if (nvs_get_str(h, k, ss, &ls) == ESP_OK && ss[0]) {
            snprintf(names[cnt], 33, "%s", ss);
            cnt++;
        }
    }
    nvs_close(h);
    return cnt;
}

/* 按索引连接记忆中的 WiFi */
bool prov_wifi_connect_idx(int idx)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return false;
    char k[12], ss[33] = {0}, pp[65] = {0};
    snprintf(k, sizeof(k), "s%d", (int)idx);
    size_t ls = sizeof(ss);
    bool ok = false;
    if (nvs_get_str(h, k, ss, &ls) == ESP_OK && ss[0]) {
        snprintf(k, sizeof(k), "p%d", (int)idx);
        size_t lp = sizeof(pp);
        nvs_get_str(h, k, pp, &lp);
        wifi_schedule_connect(ss, pp);
        ok = true;
        ESP_LOGI(TAG, "connect saved wifi #%d: %s", idx, ss);
    }
    nvs_close(h);
    return ok;
}

const char *prov_wifi_current(void) { return s_cur_ssid; }

int prov_wifi_status(void) { return s_wifi_conn_state; }
void prov_wifi_reset_status(void) { s_wifi_conn_state = 0; }
const char *prov_wifi_target(void) { return s_wifi_target; }

static void wifi_schedule_connect(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) return;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_new_ssid, sizeof(s_new_ssid), "%s", ssid);
    snprintf(s_new_pass, sizeof(s_new_pass), "%s", pass ? pass : "");
    s_has_pending = true;
    if (s_lock) xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "wifi connect scheduled: %s", ssid);
}

void nvs_save_public(const char *ssid, const char *pass) { wifi_creds_add_ex(ssid, pass, false); }

static void nvs_save(const char *ssid, const char *pass)
{
    wifi_creds_add_ex(ssid, pass, true);   /* 连接成功：更新记忆 + 标记为上次使用 */
}

static void ap_ssid_ensure(void)
{
    if (s_ap_ssid[0] == 0) {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
        snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s-%02X%02X", AP_SSID_PREFIX, mac[4], mac[5]);
    }
    if (s_pair_code[0] == 0) {
        random_code(s_pair_code, 6);      /* 配对码：无论走哪条路径进配网都保证有值 */
    }
}

static void wifi_start_ap(void)
{
    if (s_ap_active) return;
    wifi_ensure_init();
    ap_ssid_ensure();
    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len = strlen(s_ap_ssid);
    ap.ap.channel = 6;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.password[0] = 0;
    ap.ap.pmf_cfg.required = false;

    wifi_mode_t mode = WIFI_MODE_APSTA;
    esp_err_t err = esp_wifi_set_mode(mode);
    if (err != ESP_OK) { ESP_LOGE(TAG, "set APSTA mode failed: %d", err); return; }
    err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err != ESP_OK) { ESP_LOGE(TAG, "set AP config failed: %d", err); return; }
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) ESP_LOGW(TAG, "disable wifi power save failed: %d", err);
    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_INIT_STATE) {
        ESP_LOGE(TAG, "wifi start failed: %d", err);
        return;
    }
    s_ap_active = true;
    ESP_LOGI(TAG, "AP started (open): %s", s_ap_ssid);
}

/* 菜单"WiFi 设置"：启动配网热点（幂等）并显示二维码 */
void prov_start_ap(void)
{
    ESP_LOGW(TAG, "prov_start_ap() called");
    wifi_start_ap();
    extern bool recorder_is_busy(void);
    extern bool menu_active(void);
    ESP_LOGW(TAG, "prov guards: busy=%d menu=%d", (int)recorder_is_busy(), (int)menu_active());
    if (!recorder_is_busy() && !menu_active()) {
        s_prov_ui = true;
        prov_menu_show_qr();
    }
    /* 同时开启蓝牙配网（ESP Blufi App 可用） */
    blufi_prov_start();
}

void prov_menu_show_qr(void)
{
    ap_ssid_ensure();
    /* 手机扫这个码会自动加入设备热点（开放网络），随后自动弹出配网页面 */
    char qr[160];
    snprintf(qr, sizeof(qr), "WIFI:T:nopass;S:%s;;", s_ap_ssid);
    s_prov_ui = true;
    draw_qr_screen(qr, s_pair_code);
}

/* ---------------- SNTP 校时（HTTPS 证书需要正确时间） ---------------- */
#include "esp_sntp.h"

static volatile bool s_clock_synced;
bool prov_clock_is_synced(void) { return s_clock_synced; }

static void sntp_sync_cb(struct timeval *tv)
{
    s_clock_synced = true;
    ESP_LOGI(TAG, "SNTP synced: %lld", (long long)tv->tv_sec);
    nvs_handle_t h;
    if (nvs_open("clock", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u64(h, "epoch", (uint64_t)tv->tv_sec);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void restore_clock(void)
{
    nvs_handle_t h;
    uint64_t epoch = 0;
    if (nvs_open("clock", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u64(h, "epoch", &epoch);
        nvs_close(h);
    }
    if (epoch > 1700000000ULL) {
        struct timeval tv = {.tv_sec = (time_t)epoch, .tv_usec = 0};
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "restored clock from NVS: %llu", (unsigned long long)epoch);
    }
}

void prov_sntp_start(void)
{
    static bool started = false;
    if (started) return;
    started = true;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "cn.pool.ntp.org");
    sntp_set_time_sync_notification_cb(sntp_sync_cb);
    esp_sntp_init();
}

/* 配网成功的短暂提示，2 秒后回待机 */
void prov_show_connected(void)
{
    extern bool menu_active(void);
    extern void menu_close(void);
    s_prov_ui = false;
    if (menu_active()) {
        menu_close();                       /* 菜单自己会画待机界面 */
        return;
    }
    if (!display_frame_take(500)) return;
    display_fill(0x0000);
    int tw = 4 * UI_FONT_CELL;
    ui_text((LCD_H_RES - tw) / 2, 200, "连接成功", 0xFFFF, 1);
    display_flush();
    display_frame_give();
    vTaskDelay(pdMS_TO_TICKS(1800));
    if (!display_frame_take(200)) return;
    display_idle_ui();
    display_frame_give();
}

/* 菜单关闭配网页时调用，交还屏幕 */
void prov_menu_hide_qr(void)
{
    s_prov_ui = false;
}



/* ---------------- HTTP ---------------- */

static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start);
}

static esp_err_t h_scan(httpd_req_t *req)
{
    wifi_scan_config_t sc = {.show_hidden = false};
    esp_err_t e = esp_wifi_scan_start(&sc, true);
    static wifi_ap_record_t recs[24];
    uint16_t cnt = 24;
    if (e != ESP_OK) {
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }
    esp_wifi_scan_get_ap_records(&cnt, recs);

    char *json = calloc(1, 4096);
    if (!json) { httpd_resp_send_500(req); return ESP_FAIL; }
    strcat(json, "[");
    int first = 1;
    for (int i = 0; i < cnt && strlen(json) < 3800; i++) {
        if (recs[i].ssid[0] == 0) continue;
        char esc[80] = {0};
        int k = 0;
        for (int j = 0; recs[i].ssid[j] && k < 70; j++) {
            char c = (char)recs[i].ssid[j];
            if (c == '"' || c == '\\') esc[k++] = '\\';
            if ((unsigned char)c < 0x20) c = '?';
            esc[k++] = c;
        }
        char item[160];
        snprintf(item, sizeof(item), "%s{\"ssid\":\"%s\",\"rssi\":%d}",
                 first ? "" : ",", esc, recs[i].rssi);
        strcat(json, item);
        first = 0;
    }
    strcat(json, "]");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static esp_err_t h_save(httpd_req_t *req)
{
    char body[768] = {0};
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"内容过长\"}");
        return ESP_OK;
    }
    int got = httpd_req_recv(req, body, total);
    if (got <= 0) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"读取失败\"}");
        return ESP_OK;
    }
    body[got] = 0;

    char ssid[33] = {0}, pass[65] = {0}, code[16] = {0};
    form_get(body, "ssid", ssid, sizeof(ssid));
    form_get(body, "pass", pass, sizeof(pass));
    form_get(body, "code", code, sizeof(code));

    if (ssid[0] == 0) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"请选择 WiFi\"}");
        return ESP_OK;
    }
    if (strcmp(code, s_pair_code) != 0) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"配对码错误\"}");
        return ESP_OK;
    }

    wifi_creds_add_ex(ssid, pass, false);    /* 先记住，成功后才标记为上次使用 */
    wifi_schedule_connect(ssid, pass);
    ESP_LOGI(TAG, "save request: ssid=\"%s\" passlen=%d (saved to NVS)", ssid, (int)strlen(pass));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t h_status(httpd_req_t *req)
{
    const char *st = "ap";
    if (s_state == 1) st = "connecting";
    else if (s_state == 2) st = "connected";
    else if (s_state == 3) st = "failed";

    char buf[192];
    snprintf(buf, sizeof(buf), "{\"state\":\"%s\",\"ip\":\"%s\",\"err\":\"%s\"}",
             st, s_ip, s_err);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

/* 强制门户：其它请求全部 302 到配置页 */
static esp_err_t h_portal(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static httpd_handle_t start_http(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 12;
    cfg.stack_size = 10240;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) return NULL;

    httpd_uri_t u_index  = {.uri = "/",       .method = HTTP_GET,  .handler = h_index};
    httpd_uri_t u_scan   = {.uri = "/scan",   .method = HTTP_GET,  .handler = h_scan};
    httpd_uri_t u_save   = {.uri = "/save",   .method = HTTP_POST, .handler = h_save};
    httpd_uri_t u_status = {.uri = "/status", .method = HTTP_GET,  .handler = h_status};
    httpd_uri_t u_portal = {.uri = "/*",      .method = HTTP_GET,  .handler = h_portal};

    httpd_register_uri_handler(srv, &u_index);
    httpd_register_uri_handler(srv, &u_scan);
    httpd_register_uri_handler(srv, &u_save);
    httpd_register_uri_handler(srv, &u_status);
    httpd_register_uri_handler(srv, &u_portal);
    return srv;
}

/* ---------------- 串口命令行（USB 直连配置） ---------------- */

static void serial_cli_task(void *arg)
{
    char line[160];
    int n = 0;
    uint8_t ch;
    for (;;) {
        int r = usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(200));
        if (r <= 0) continue;
        if (ch == '\r') continue;
        if (ch != '\n') {
            if (n < (int)sizeof(line) - 1) line[n++] = (char)ch;
            continue;
        }
        line[n] = 0;
        n = 0;
        if (line[0] == 0) continue;

        if (strncmp(line, "WIFI ", 5) == 0) {
            char *rest = line + 5;
            char *sp = strchr(rest, ' ');
            if (!sp) {
                ESP_LOGI(TAG, "CLI_ERR usage: WIFI <ssid> <password>");
                continue;
            }
            *sp = 0;
            const char *ssid = rest;
            const char *pass = sp + 1;
            wifi_schedule_connect(ssid, pass);
            ESP_LOGI(TAG, "CLI_SET ssid=\"%s\" passlen=%d", s_new_ssid, (int)strlen(s_new_pass));
        } else if (strncmp(line, "STATUS", 6) == 0) {
            extern const char *recorder_state(void);
            extern bool recorder_is_busy(void);
            const char *ssid = s_cur_ssid[0] ? s_cur_ssid : s_wifi_target;
            ESP_LOGI(TAG, "CLI_STATUS state=%d ip=%s ssid=%s rec=%s busy=%d",
                     s_state, s_ip, ssid, recorder_state(), (int)recorder_is_busy());
            recorder_log_diagnostics();
        } else if (strncmp(line, "TEST ", 5) == 0) {
            /* TEST <ip> <port> —— 测试能否连到 hub */
            char ip[64] = {0};
            int port = 0;
            if (sscanf(line + 5, "%63s %d", ip, &port) == 2) {
                int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                struct sockaddr_in addr = {0};
                addr.sin_family = AF_INET;
                addr.sin_port = htons(port);
                addr.sin_addr.s_addr = inet_addr(ip);
                struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
                setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
                int rc = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
                ESP_LOGI(TAG, "CLI_TEST %s:%d rc=%d errno=%d", ip, port, rc, errno);
                if (rc == 0) close(sock);
            } else {
                ESP_LOGI(TAG, "CLI_ERR usage: TEST <ip> <port>");
            }
        } else if (strncmp(line, "HUB ", 4) == 0) {
            char ip[32] = {0};
            int port = 0;
            if (sscanf(line + 4, "%31s %d", ip, &port) == 2) {
                char url[96];
                snprintf(url, sizeof(url), "http://%s:%d/upload", ip, port);
                recorder_set_url(url, "");
                ESP_LOGI(TAG, "CLI_HUB %s:%d", ip, port);
            } else {
                ESP_LOGI(TAG, "CLI_ERR usage: HUB <ip> <port>");
            }
        } else if (strncmp(line, "REGS", 4) == 0) {
            i2c_device_config_t dc = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address = 0x34,
                .scl_speed_hz = 100000,
            };
            i2c_master_dev_handle_t d34 = NULL, d40 = NULL;
            i2c_master_bus_add_device(display_i2c_bus(), &dc, &d34);
            dc.device_address = 0x40;
            i2c_master_bus_add_device(display_i2c_bus(), &dc, &d40);

            const uint8_t aregs[] = {0x90, 0x92, 0x82};
            for (int i = 0; i < 3; i++) {
                uint8_t v = 0, r = aregs[i];
                if (i2c_master_transmit_receive(d34, &r, 1, &v, 1, 100) == ESP_OK)
                    ESP_LOGI(TAG, "AXP[%02X]=%02X", r, v);
            }
            for (uint8_t r = 0x00; r <= 0x05; r++) {
                uint8_t v = 0;
                if (i2c_master_transmit_receive(d40, &r, 1, &v, 1, 100) == ESP_OK)
                    ESP_LOGI(TAG, "ES7210[%02X]=%02X", r, v);
            }
            for (uint8_t r = 0x40; r <= 0x47; r++) {
                uint8_t v = 0;
                if (i2c_master_transmit_receive(d40, &r, 1, &v, 1, 100) == ESP_OK)
                    ESP_LOGI(TAG, "ES7210[%02X]=%02X", r, v);
            }
            i2c_master_bus_rm_device(d34);
            i2c_master_bus_rm_device(d40);
        } else if (strncmp(line, "I2CSCAN", 7) == 0) {
            int found = 0;
            for (uint8_t a = 0x08; a < 0x78; a++) {
                if (i2c_master_probe(display_i2c_bus(), a, 50) == ESP_OK) {
                    ESP_LOGI(TAG, "I2C_DEV 0x%02X", a);
                    found++;
                }
            }
            ESP_LOGI(TAG, "I2C_SCAN_DONE found=%d", found);
        } else if (strncmp(line, "HUBURL ", 7) == 0) {
            char url[192] = {0}, tok[80] = {0};
            int fields=sscanf(line + 7, "%191s %79s", url, tok);
            if (fields >= 1) {
                recorder_set_url(url, fields >= 2 ? tok : NULL);
                ESP_LOGI(TAG, "CLI_HUBURL %s", url);
            } else {
                ESP_LOGI(TAG, "CLI_ERR usage: HUBURL <url> [token]");
            }
        } else if (strncmp(line, "QUEUELS", 7) == 0) {
            extern void queue_list(void);
            queue_list();
        } else if (!strcmp(line, "STORAGE")) {
            extern void queue_log_storage(void);
            queue_log_storage();
        } else if (!strcmp(line, "CLEAR") || !strcmp(line, "CLEAN")) {
            extern void queue_clear(void);
            queue_clear();
            ESP_LOGI(TAG, "CLI_CLEAN scheduled; pending recordings preserved");
        } else if (strncmp(line, "HEAP", 4) == 0) {
            ESP_LOGI(TAG, "HEAP internal=%u psram=%u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        } else if (strncmp(line, "PROV", 4) == 0) {
            extern void prov_start_ap(void);
            ESP_LOGI(TAG, "HEAP before PROV: internal=%u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            prov_start_ap();
            ESP_LOGI(TAG, "CLI_PROV provisioning mode on");
        } else if (strncmp(line, "PROVSTOP", 8) == 0) {
            extern void prov_menu_hide_qr(void);
            prov_menu_hide_qr();
            blufi_prov_stop();
            display_idle_ui();
            ESP_LOGI(TAG, "CLI_PROVSTOP provisioning closed");
        } else if (strncmp(line, "CONNECT ", 8) == 0) {
            int idx = atoi(line + 8);
            extern bool prov_wifi_connect_idx(int idx);
            if (prov_wifi_connect_idx(idx)) ESP_LOGI(TAG, "CLI_CONNECT idx=%d", idx);
            else ESP_LOGI(TAG, "CLI_CONNECT idx=%d not found", idx);
        } else if (strncmp(line, "LISTWIFI", 8) == 0) {
            char names[4][33];
            extern int prov_wifi_list(char names[][33], int max);
            extern const char *prov_wifi_current(void);
            int n = prov_wifi_list(names, 4);
            for (int i = 0; i < n; i++) {
                ESP_LOGI(TAG, "CLI_WIFI #%d %s%s", i, names[i],
                         (prov_wifi_current() && strcmp(prov_wifi_current(), names[i]) == 0) ? " (current)" : "");
            }
            if (n == 0) ESP_LOGI(TAG, "CLI_WIFI none");
        } else if (strncmp(line, "SAVEWIFI ", 9) == 0) {
            char ssid[33] = {0}, pass[65] = {0};
            if (sscanf(line + 9, "%32s %64s", ssid, pass) >= 1) {
                extern void nvs_save_public(const char *ssid, const char *pass);
                nvs_save_public(ssid, pass);
                ESP_LOGI(TAG, "CLI_SAVEWIFI saved: %s", ssid);
            } else {
                ESP_LOGI(TAG, "CLI_SAVEWIFI usage: SAVEWIFI <ssid> [pass]");
            }
        } else if (strncmp(line, "FORGET", 6) == 0) {
            nvs_handle_t h;
            if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) {
                nvs_erase_all(h);
                nvs_commit(h);
                nvs_close(h);
            }
            ESP_LOGI(TAG, "CLI_FORGET wifi credentials cleared");
        } else if (strncmp(line, "UNENROLL ", 9) == 0) {
            int slot = atoi(line + 9);
            extern bool recorder_upload_unenroll(int slot);
            extern void menu_set_slot(int slot, const char *name);
            extern void menu_save_slot(int slot);
            recorder_upload_unenroll(slot);
            menu_set_slot(slot, "");
            menu_save_slot(slot);
            ESP_LOGI(TAG, "CLI_UNENROLL slot=%d done", slot);
        } else if (strncmp(line, "NAME ", 5) == 0) {
            int slot = 0;
            char nm[16] = {0};
            if (sscanf(line + 5, "%d %15s", &slot, nm) == 2 && slot >= 0 && slot <= 4) {
                extern void menu_set_slot(int slot, const char *name);
                extern void menu_save_slot(int slot);
                extern bool recorder_upload_rename(int slot, const char *name);
                bool synced = recorder_upload_rename(slot, nm);
                if (synced) {
                    menu_set_slot(slot, nm);
                    menu_save_slot(slot);
                }
                ESP_LOGI(TAG, "CLI_NAME slot=%d name=%s synced=%d stack_free=%u", slot, nm,
                         synced, (unsigned)uxTaskGetStackHighWaterMark(NULL));
            } else {
                ESP_LOGI(TAG, "CLI_NAME usage: NAME <0-4> <名字>");
            }
        } else if (strncmp(line, "MENU", 4) == 0) {
            extern void menu_open(void);
            menu_open();
            ESP_LOGI(TAG, "CLI_MENU opened");
        } else if (strncmp(line, "TAP ", 4) == 0) {
            int tx = 0, ty = 0;
            if (sscanf(line + 4, "%d %d", &tx, &ty) == 2) {
                extern bool menu_tap(int x, int y);
                bool used = menu_tap(tx, ty);
                ESP_LOGI(TAG, "CLI_TAP (%d,%d) used=%d", tx, ty, (int)used);
            } else {
                ESP_LOGI(TAG, "CLI_TAP bad args");
            }
        } else if (strncmp(line, "STOP", 4) == 0) {
            extern void recorder_stop(void);
            recorder_stop();
            ESP_LOGI(TAG, "CLI_STOP");
        } else if (!strcmp(line,"KEYBOTTOM")) {
            extern void recorder_toggle_screen(void);
            recorder_toggle_screen();
            ESP_LOGI(TAG,"CLI_KEYBOTTOM");
        } else if (strncmp(line, "KEYTOP", 6) == 0) {
            extern void recorder_top_key(void);
            recorder_top_key();
            ESP_LOGI(TAG, "CLI_KEYTOP");
        } else if (strncmp(line, "START", 5) == 0) {
            extern void recorder_request_start(void);
            recorder_request_start();
            ESP_LOGI(TAG, "CLI_START");
        } else if (strncmp(line, "AUTO", 4) == 0) {
            bool on = strstr(line, "ON") != NULL || strstr(line, "on") != NULL;
            extern void recorder_auto_set(bool);
            recorder_auto_set(on);
            ESP_LOGI(TAG, "CLI_AUTO on=%d", on);
        } else if (strncmp(line, "ASK", 3) == 0) {
            extern void recorder_ask(void);
            recorder_ask();
            ESP_LOGI(TAG, "CLI_ASK");
        } else if (strncmp(line, "REC", 3) == 0) {
            recorder_start();
            ESP_LOGI(TAG, "CLI_REC state=%s", recorder_state());
        } else if (strncmp(line, "SCAN", 4) == 0) {
            log_scan_results();
        } else {
            ESP_LOGI(TAG, "CLI_ERR unknown command");
        }
    }
}

/* ---------------- 主流程 ---------------- */

static void nvs_save_last_error(const char *msg)
{
    nvs_handle_t h;
    if (nvs_open("prov", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "last_err", msg);
    nvs_commit(h);
    nvs_close(h);
}



typedef struct {
    char ssid[33];
    char pass[65];
} connect_ctx_t;

static bool wifi_load_password(const char *ssid, char *pass, size_t pass_size)
{
    nvs_handle_t h;
    if (!ssid || !ssid[0] || nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t n = 0;
    nvs_get_u8(h, "n", &n);
    bool found = false;
    for (int i = 0; i < n && i < WIFI_MAX_CREDS; i++) {
        char key[12], saved[33] = {0};
        size_t saved_len = sizeof(saved);
        snprintf(key, sizeof(key), "s%d", i);
        if (nvs_get_str(h, key, saved, &saved_len) == ESP_OK && strcmp(saved, ssid) == 0) {
            snprintf(key, sizeof(key), "p%d", i);
            size_t len = pass_size;
            if (nvs_get_str(h, key, pass, &len) != ESP_OK) pass[0] = 0;
            found = true;
            break;
        }
    }
    nvs_close(h);
    return found;
}

static void connect_task(void *arg)
{
    connect_ctx_t *ctx = arg;
    char prev_ssid[33] = {0}, prev_pass[65] = {0};
    snprintf(prev_ssid, sizeof(prev_ssid), "%s", s_cur_ssid);
    wifi_load_password(prev_ssid, prev_pass, sizeof(prev_pass));

    wifi_config_t sta = {0};
    size_t ssid_len = strnlen(ctx->ssid, sizeof(ctx->ssid));
    if (ssid_len > sizeof(sta.sta.ssid)) ssid_len = sizeof(sta.sta.ssid);
    memcpy(sta.sta.ssid, ctx->ssid, ssid_len);
    size_t pass_len = strnlen(ctx->pass, sizeof(ctx->pass));
    if (pass_len > sizeof(sta.sta.password)) pass_len = sizeof(sta.sta.password);
    memcpy(sta.sta.password, ctx->pass, pass_len);
    sta.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    snprintf(s_wifi_target, sizeof(s_wifi_target), "%s", ctx->ssid);
    s_wifi_conn_state = 1;
    s_state = 1;
    s_err[0] = 0;
    s_reason = 0;
    ESP_LOGI(TAG, "connecting to \"%s\" (pass len=%d)", ctx->ssid, (int)strlen(ctx->pass));

    wifi_ensure_init();
    wifi_mode_t wanted_mode = s_ap_active ? WIFI_MODE_APSTA : WIFI_MODE_STA;
    esp_err_t err = esp_wifi_set_mode(wanted_mode);
    if (err != ESP_OK) ESP_LOGW(TAG, "set wifi mode rc=%d", err);
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) ESP_LOGW(TAG, "disable wifi power save rc=%d", err);
    err = esp_wifi_start();
    if (err != ESP_OK && !s_wifi_started) ESP_LOGW(TAG, "esp_wifi_start rc=%d", err);
    if (!s_wifi_started) {
        EventBits_t ready = xEventGroupWaitBits(s_evt, WIFI_READY_BIT, pdFALSE, pdFALSE,
                                                pdMS_TO_TICKS(5000));
        if (!(ready & WIFI_READY_BIT)) ESP_LOGW(TAG, "wifi start event timeout");
    }

    s_ignore_disconnect_until_us = esp_timer_get_time() + 1500000;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(150));
    xEventGroupClearBits(s_evt, WIFI_CONNECT_BIT | WIFI_FAIL_BIT);
    err = esp_wifi_set_config(WIFI_IF_STA, &sta);
    if (err != ESP_OK) {
        snprintf(s_err, sizeof(s_err), "set_config=%d", err);
        ESP_LOGE(TAG, "set STA config failed: %d", err);
    }

    s_connected = false;
    bool ok = false;
    for (int attempt = 0; err == ESP_OK && attempt < 3 && !ok; attempt++) {
        xEventGroupClearBits(s_evt, WIFI_CONNECT_BIT);
        ESP_LOGI(TAG, "wifi attempt %d/3: %s", attempt + 1, ctx->ssid);
        esp_err_t rc = esp_wifi_connect();
        if (rc != ESP_OK) ESP_LOGW(TAG, "esp_wifi_connect rc=%d", rc);
        EventBits_t bits = xEventGroupWaitBits(s_evt, WIFI_CONNECT_BIT, pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(12000));
        /* GOT_IP 事件和断开事件可能在相邻调度点到达，必须同时确认连接标志，
         * 避免把已经掉线的旧 CONNECT_BIT 当成成功。 */
        ok = (bits & WIFI_CONNECT_BIT) != 0 && s_connected;
        if (!ok) {
            ESP_LOGW(TAG, "wifi attempt %d timeout/fail reason=%d", attempt + 1, s_reason);
            s_ignore_disconnect_until_us = esp_timer_get_time() + 1500000;
            esp_wifi_disconnect();
            int delay_ms = attempt == 0 ? 1000 : 2000;
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }

    if (ok) {
        nvs_save(ctx->ssid, ctx->pass);
        snprintf(s_cur_ssid, sizeof(s_cur_ssid), "%s", ctx->ssid);
        s_wifi_conn_state = 2;
        s_state = 2;
        s_connected = true;
        s_retry_failures = 0;
        s_next_retry_ms = 0;
        /* 连接建立后开启最小调制解调器省电；录音本地采集不依赖 WiFi，
         * 上传任务仍会由 ESP-IDF 自动唤醒射频。配网阶段继续保持常开。 */
        err = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        if (err != ESP_OK) ESP_LOGW(TAG, "enable wifi power save rc=%d", err);
        ESP_LOGI(TAG, "WIFI_OK ssid=%s ip=%s", ctx->ssid, s_ip);
        if (s_ap_active && esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK) {
            s_ap_active = false;
            ESP_LOGI(TAG, "provisioning AP stopped after successful connection");
        }
    } else {
        if (s_err[0] == 0) snprintf(s_err, sizeof(s_err), "reason=%d timeout", s_reason);
        s_wifi_conn_state = 3;
        s_state = 0;
        s_connected = false;
        s_retry_failures++;
        static const int retry_delay_ms[] = {1000, 2000, 5000, 10000, 30000};
        int delay_idx = s_retry_failures - 1;
        if (delay_idx > 4) delay_idx = 4;
        s_next_retry_ms = esp_timer_get_time() / 1000 + retry_delay_ms[delay_idx];
        char msg[192];
        snprintf(msg, sizeof(msg), "ssid=%.32s err=%.64s", ctx->ssid, s_err);
        nvs_save_last_error(msg);
        ESP_LOGW(TAG, "WIFI_FAIL ssid=%s %s", ctx->ssid, s_err);
        if (prev_ssid[0] && strcmp(prev_ssid, ctx->ssid) != 0) {
            ESP_LOGW(TAG, "schedule rollback to previous wifi %s", prev_ssid);
            wifi_schedule_connect(prev_ssid, prev_pass);
        }
    }

    free(ctx);
    s_connect_task_running = false;
    vTaskDelete(NULL);
}

static void start_pending_connect(void)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_has_pending || s_connect_task_running) {
        if (s_lock) xSemaphoreGive(s_lock);
        return;
    }
    connect_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        if (s_lock) xSemaphoreGive(s_lock);
        ESP_LOGE(TAG, "no memory for wifi connect context");
        return;
    }
    snprintf(ctx->ssid, sizeof(ctx->ssid), "%s", s_new_ssid);
    snprintf(ctx->pass, sizeof(ctx->pass), "%s", s_new_pass);
    s_has_pending = false;
    s_connect_task_running = true;
    if (s_lock) xSemaphoreGive(s_lock);
    if (xTaskCreate(connect_task, "conn", 6144, ctx, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "wifi connect task create failed");
        free(ctx);
        s_connect_task_running = false;
        wifi_schedule_connect(s_new_ssid, s_new_pass);
    }
}

void app_main(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    restore_clock();

    s_evt = xEventGroupCreate();
    s_lock = xSemaphoreCreateMutex();

    if (display_init() != ESP_OK) {
        ESP_LOGE(TAG, "display init failed");
    }
    if (mic_init() != ESP_OK) {
        ESP_LOGE(TAG, "mic init failed");
    }

    /* WiFi 凭据如果已保存就不起热点，直接连 */
    bool need_ap = true;
    {
        nvs_handle_t h;
        if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK) {
            char ssid[33] = {0};
            size_t ls = sizeof(ssid);
            if (nvs_get_str(h, "ssid", ssid, &ls) == ESP_OK && ssid[0]) {
                need_ap = false;
            }
            nvs_close(h);
        }
    }
    if (need_ap) {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
        snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s-%02X%02X", AP_SSID_PREFIX, mac[4], mac[5]);
        random_code(s_pair_code, 6);
        wifi_start_ap();
        s_first_boot_prov = true;        /* 首次开机无 WiFi → 直接显示配网二维码 */
    } else {
        /* 已有凭据：直接连 WiFi */
        nvs_handle_t h;
        char ssid[33] = {0}, pass[65] = {0};
        size_t ls = sizeof(ssid), lp = sizeof(pass);
        if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK) {
            nvs_get_str(h, "ssid", ssid, &ls);
            nvs_get_str(h, "pass", pass, &lp);
            nvs_close(h);
        }
        if (ssid[0]) {
            /* 有凭据：统一交给 connect_task 处理（唯一连接路径，避免重复连接） */
            wifi_ensure_init();
            ESP_LOGI(TAG, "saved wifi \"%s\", will connect", ssid);
        } else {
            /* 无凭据：起热点配网 */
            uint8_t mac[6] = {0};
            esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
            snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s-%02X%02X", AP_SSID_PREFIX, mac[4], mac[5]);
            random_code(s_pair_code, 6);
            wifi_start_ap();
        }
    }
    /* 必须在网络栈就绪后再启动录音/队列（flush 任务会发网络请求） */
    recorder_init(NULL, 0);

    /* 首次开机（从没配过 WiFi）→ 直接显示配网二维码；否则进待机 */
    if (s_first_boot_prov) {
        prov_start_ap();
    } else {
        display_idle_ui();
    }

    start_http();
    xTaskCreate(dns_task, "dns", 4096, NULL, 4, NULL);

    /* 开机若已有凭据，自动重连（成功则覆盖为成功界面） */
    {
        nvs_handle_t h;
        if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK) {
            char ssid[33] = {0}, pass[65] = {0};
            size_t ls = sizeof(ssid), lp = sizeof(pass);
            if (nvs_get_str(h, "ssid", ssid, &ls) == ESP_OK && ssid[0]) {
                nvs_get_str(h, "pass", pass, &lp);
            } else {
                ls = sizeof(ssid);
                if (nvs_get_str(h, "s0", ssid, &ls) == ESP_OK && ssid[0]) {
                    lp = sizeof(pass);
                    nvs_get_str(h, "p0", pass, &lp);   /* 没有"上次使用"就用第一个 */
                }
            }
            if (ssid[0]) {
                wifi_schedule_connect(ssid, pass);
                ESP_LOGI(TAG, "auto-connect saved ssid=\"%s\"", ssid);
            }
            nvs_close(h);
        }
    }

    usb_serial_jtag_driver_config_t ujcfg = {
        .rx_buffer_size = 1024,
        .tx_buffer_size = 1024,
    };
    if (usb_serial_jtag_driver_install(&ujcfg) == ESP_OK) {
        /* NAME/UNENROLL perform a TLS request; 4KB overflowed on real hardware. */
        xTaskCreate(serial_cli_task, "cli", 8192, NULL, 6, NULL);
        ESP_LOGI(TAG, "serial CLI ready: WIFI <ssid> <pass> | SCAN | STATUS");
    } else {
        ESP_LOGW(TAG, "serial CLI install failed");
    }
    {
        nvs_handle_t h;
        if (nvs_open("prov", NVS_READONLY, &h) == ESP_OK) {
            char buf[192] = {0};
            size_t len = sizeof(buf);
            if (nvs_get_str(h, "last_err", buf, &len) == ESP_OK && buf[0]) {
                ESP_LOGW(TAG, "LAST_ERROR %s", buf);
            }
            nvs_close(h);
        }
    }
    ESP_LOGI(TAG, "READY ap=%s (open) pair=%s url=http://%s",
             s_ap_ssid, s_pair_code, AP_IP_STR);

    for (;;) {
        start_pending_connect();
        /* 只在"刚连上"的瞬间处理配网页退出（不干扰用户打开的菜单） */
        {
            static bool prev_conn = false;
            if (s_connected && !prev_conn) {
                if (s_prov_ui || blufi_prov_running()) {
                    ESP_LOGI(TAG, "wifi connected -> leave provisioning UI");
                    blufi_prov_stop();
                    prov_show_connected();
                }
            }
            prev_conn = s_connected;
        }
        /* 断网重连：1/2/5/10/30 秒退避；同一网络失败三轮后轮换记忆网络。 */
        int64_t now = esp_timer_get_time() / 1000;
        if (!s_connected && !s_connect_task_running && !s_has_pending &&
            s_state == 0 && now >= s_next_retry_ms) {
            char names[4][33];
            int n = prov_wifi_list(names, 4);
            if (n > 0) {
                int base_idx = 0;
                for (int i = 0; i < n; i++) {
                    if (s_cur_ssid[0] && strcmp(names[i], s_cur_ssid) == 0) {
                        base_idx = i;
                        break;
                    }
                }
                int rotate = s_retry_failures / 3;
                int try_idx = (base_idx + rotate) % n;
                ESP_LOGI(TAG, "wifi retry #%d failure_count=%d ssid=%s",
                         try_idx, s_retry_failures, names[try_idx]);
                prov_wifi_connect_idx(try_idx);
                s_next_retry_ms = now + 30000; /* pending 保护之外的兜底 */
            }
        }
        vTaskDelay(pdMS_TO_TICKS(300));
    }
}

/* 待机界面：麦克风图标（外圈 + 底座） */
void display_idle_ui(void)
{
    watchface_draw(WATCHFACE_IDLE, NULL, 0, 0);
}

/* 给 BLE 模块的状态 JSON（不回传任何密钥） */
void prov_status_json(char *out, int outlen)
{
    extern const char *recorder_state(void);
    extern bool recorder_auto_enabled(void);
    extern int queue_count(void);
    int up = (int)(esp_timer_get_time() / 1000000);
    snprintf(out, outlen,
             "{\"state\":\"%s\",\"ip\":\"%s\",\"ssid\":\"%s\","
             "\"queue\":%d,\"auto\":%d,\"uptime\":%d}",
             recorder_state(), s_ip, s_cur_ssid[0] ? s_cur_ssid : s_wifi_target, queue_count(),
             recorder_auto_enabled() ? 1 : 0, up);
}

/* 给 BLE 配网用的 WiFi 连接入口（复用配网 connect 流程） */
void prov_wifi_connect(const char *ssid, const char *pass)
{
    wifi_creds_add_ex(ssid, pass, false);
    wifi_schedule_connect(ssid, pass);
    ESP_LOGI("ble", "wifi creds from BLE: \"%s\"", ssid);
}
