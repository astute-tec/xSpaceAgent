/*
 * 官方 BluFi 蓝牙配网（Espressif 成熟方案，NimBLE host）
 *
 * 手机端：安装官方 "ESP Blufi" App（App Store / 应用市场）→ 搜索连接
 *         "Xiaozhi-xxxx" → 输入办公室 WiFi 名称和密码 → 设备自动连接。
 *
 * 设备端：只在配网模式（设置页 → WiFi）启动，平时不占 CPU，避免与 AFE 争抢。
 */
#include <string.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_blufi_api.h"
#include "esp_blufi.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

#include "blufi_prov.h"

void ble_store_config_init(void);      /* NimBLE 内部函数 */

#define PROV_DEVICE_NAME "Xiaozhi-54D9"   /* 手机 BluFi App 里看到的名字 */

static const char *TAG = "blufi";
static bool s_initialized = false;

/* main.c 提供的入口 */
extern void prov_submit_credentials(const char *ssid, const char *pass);
extern bool prov_is_connected(void);
extern const char *prov_ap_ssid(void);

/* ---------------- 事件处理 ---------------- */

static void blufi_event_cb(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param)
{
    switch (event) {
    case ESP_BLUFI_EVENT_INIT_FINISH:
        ESP_LOGI(TAG, "init finish");
        esp_blufi_adv_start();          /* 显式开始广播，手机才能搜到 */
        break;
    case ESP_BLUFI_EVENT_BLE_CONNECT:
        ESP_LOGI(TAG, "phone connected");
        break;
    case ESP_BLUFI_EVENT_BLE_DISCONNECT:
        ESP_LOGI(TAG, "phone disconnected");
        break;
    case ESP_BLUFI_EVENT_RECV_STA_SSID: {
        char ssid[33] = {0};
        int n = param->sta_ssid.ssid_len;
        if (n > 32) n = 32;
        memcpy(ssid, param->sta_ssid.ssid, n);
        ESP_LOGI(TAG, "recv ssid=\"%s\"", ssid);
        prov_submit_credentials(ssid, NULL);
        break;
    }
    case ESP_BLUFI_EVENT_RECV_STA_PASSWD: {
        char pass[65] = {0};
        int n = param->sta_passwd.passwd_len;
        if (n > 64) n = 64;
        memcpy(pass, param->sta_passwd.passwd, n);
        ESP_LOGI(TAG, "recv passwd (len=%d)", n);
        prov_submit_credentials(NULL, pass);
        break;
    }
    case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP:
        ESP_LOGI(TAG, "request connect to AP");
        esp_blufi_send_wifi_conn_report(WIFI_MODE_APSTA, ESP_BLUFI_STA_CONNECTING, 0, NULL);
        break;
    case ESP_BLUFI_EVENT_GET_WIFI_STATUS: {
        const char *ap = prov_ap_ssid();
        esp_blufi_extra_info_t info = {0};
        if (ap && ap[0]) {
            info.softap_ssid = (uint8_t *)ap;
            info.softap_ssid_len = strlen(ap);
        }
        esp_blufi_send_wifi_conn_report(WIFI_MODE_APSTA,
                                        prov_is_connected() ? ESP_BLUFI_STA_CONN_SUCCESS
                                                            : ESP_BLUFI_STA_CONN_FAIL,
                                        0, &info);
        break;
    }
    default:
        break;
    }
}

/* ---------------- BluFi 协议回调（不启用加密，简化配网） ---------------- */

static void blufi_negotiate_data_handler(uint8_t *data, int len, uint8_t **output_data,
                                         int *output_len, bool *need_free)
{
    *output_data = NULL;
    *output_len = 0;
    *need_free = false;
}

static esp_err_t blufi_aes_encrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len)
{
    return ESP_OK;
}

static esp_err_t blufi_aes_decrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len)
{
    return ESP_OK;
}

static uint16_t blufi_crc_checksum(uint8_t iv8, uint8_t *data, int len)
{
    return 0;
}

static esp_blufi_callbacks_t s_blufi_cb = {
    .event_cb = blufi_event_cb,
    .negotiate_data_handler = blufi_negotiate_data_handler,
    .encrypt_func = blufi_aes_encrypt,
    .decrypt_func = blufi_aes_decrypt,
    .checksum_func = blufi_crc_checksum,
};

/* ---------------- NimBLE host ---------------- */

static void blufi_on_sync(void)
{
    esp_blufi_profile_init();          /* 同步后初始化 profile，会自动开始广播 */
}

static void blufi_on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset, reason=%d", reason);
}

static void blufi_host_task(void *param)
{
    ESP_LOGI(TAG, "BLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t blufi_prov_init(void)
{
    if (s_initialized) return ESP_OK;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", err);
        return err;
    }

    ble_hs_cfg.reset_cb = blufi_on_reset;
    ble_hs_cfg.sync_cb = blufi_on_sync;
    ble_hs_cfg.gatts_register_cb = esp_blufi_gatt_svr_register_cb;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    if (esp_blufi_gatt_svr_init() != 0) {
        ESP_LOGE(TAG, "gatt svr init failed");
        return ESP_FAIL;
    }
    ble_svc_gap_device_name_set(PROV_DEVICE_NAME);
    ble_store_config_init();
    esp_blufi_btc_init();

    if (esp_blufi_register_callbacks(&s_blufi_cb) != ESP_OK) {
        ESP_LOGE(TAG, "register callbacks failed");
        return ESP_FAIL;
    }

    nimble_port_freertos_init(blufi_host_task);
    s_initialized = true;
    ESP_LOGI(TAG, "BLUFI ready (device name: %s)", PROV_DEVICE_NAME);
    return ESP_OK;
}

void blufi_prov_start(void)
{
    if (!s_initialized) {
        blufi_prov_init();       /* 初始化 NimBLE host + BluFi profile（同步后自动广播） */
    }
}

static void blufi_teardown_task(void *arg)
{
    ESP_LOGI(TAG, "BLUFI teardown start");
    if (nimble_port_stop() == ESP_OK) {
        esp_blufi_profile_deinit();
        esp_blufi_gatt_svr_deinit();
        nimble_port_deinit();
        s_initialized = false;
        ESP_LOGI(TAG, "BLUFI stopped, memory released");
    } else {
        ESP_LOGW(TAG, "BLUFI stop failed");
    }
    vTaskDelete(NULL);
}

void blufi_prov_stop(void)
{
    if (!s_initialized) return;
    /* 必须异步：nimble_port_stop 会等待 host 任务退出 */
    xTaskCreate(blufi_teardown_task, "blufi_down", 4096, NULL, 3, NULL);
}

bool blufi_prov_running(void) { return s_initialized; }
