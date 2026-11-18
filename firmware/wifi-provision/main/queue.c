/* 断网队列：上传失败的录音转单声道存 FATFS，联网后自动补传 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "queue.h"
#include "mic.h"
#include "recorder.h"

static const char *TAG = "queue";
static int upload_file(const char *path, const char *url, const char *token);
static const char *CF_FALLBACK_URL = "https://104.21.50.72/v1/upload";
static const char *CF_FALLBACK_HOST = "xiaozhi.halmir.top";
#define MOUNT "/fat"
static wl_handle_t s_wlh = WL_INVALID_HANDLE;
static SemaphoreHandle_t s_fs_lock;
static SemaphoreHandle_t s_flush_lock;
static volatile bool s_flush_requested;

#define MAX_QUEUE_FILES 64
#define QUEUE_RESERVE_BYTES (1024 * 1024)

enum {
    UPLOAD_RETRY = -1,
    UPLOAD_OK = 0,
    UPLOAD_QUARANTINED = 1,
    UPLOAD_PAUSED = 2,
};

typedef struct {
    char name[64];
    unsigned long seq;
} queue_entry_t;

static bool is_wav_name(const char *name)
{
    size_t n = name ? strlen(name) : 0;
    return n > 6 && strncmp(name, "q_", 2) == 0 && strcmp(name + n - 4, ".wav") == 0;
}

static unsigned long name_sequence(const char *name)
{
    if (!name || strncmp(name, "q_", 2) != 0) return 0;
    return strtoul(name + 2, NULL, 10);
}

static int entry_compare(const void *a, const void *b)
{
    const queue_entry_t *ea = a;
    const queue_entry_t *eb = b;
    if (ea->seq < eb->seq) return -1;
    if (ea->seq > eb->seq) return 1;
    return strcmp(ea->name, eb->name);
}

static int collect_entries(queue_entry_t *entries, int max)
{
    DIR *d = opendir(MOUNT);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (!is_wav_name(e->d_name)) continue;
        size_t name_len = strnlen(e->d_name, sizeof(entries[n].name));
        if (name_len >= sizeof(entries[n].name)) continue;
        memcpy(entries[n].name, e->d_name, name_len + 1);
        entries[n].seq = name_sequence(e->d_name);
        n++;
    }
    closedir(d);
    qsort(entries, n, sizeof(entries[0]), entry_compare);
    return n;
}

static uint32_t next_sequence_locked(void)
{
    uint32_t seq = 0;
    nvs_handle_t h;
    if (nvs_open("queue", NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, "seq", &seq);
        if (seq == 0) {
            queue_entry_t *entries = heap_caps_malloc(sizeof(*entries) * MAX_QUEUE_FILES,
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            int n = entries ? collect_entries(entries, MAX_QUEUE_FILES) : 0;
            if (n > 0 && entries[n - 1].seq < UINT32_MAX) seq = (uint32_t)entries[n - 1].seq;
            free(entries);
        }
        seq++;
        if (seq == 0) seq = 1;
        nvs_set_u32(h, "seq", seq);
        nvs_commit(h);
        nvs_close(h);
    } else {
        seq = (uint32_t)esp_log_timestamp();
    }
    return seq;
}

static bool ensure_space_locked(uint64_t needed)
{
    uint64_t total = 0, freeb = 0;
    if (esp_vfs_fat_info(MOUNT, &total, &freeb) != ESP_OK) return false;
    queue_entry_t *entries = heap_caps_malloc(sizeof(*entries) * MAX_QUEUE_FILES,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!entries) return false;
    while (freeb < needed + QUEUE_RESERVE_BYTES) {
        int n = collect_entries(entries, MAX_QUEUE_FILES);
        if (n <= 0) { free(entries); return false; }
        char oldest[96];
        snprintf(oldest, sizeof(oldest), MOUNT "/%s", entries[0].name);
        ESP_LOGW(TAG, "flash full: remove oldest queued segment %s to preserve newest", entries[0].name);
        if (unlink(oldest) != 0) { free(entries); return false; }
        if (esp_vfs_fat_info(MOUNT, &total, &freeb) != ESP_OK) { free(entries); return false; }
    }
    free(entries);
    return true;
}

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

esp_err_t queue_init(void)
{
    if (!s_fs_lock) s_fs_lock = xSemaphoreCreateMutex();
    if (!s_flush_lock) s_flush_lock = xSemaphoreCreateMutex();
    if (!s_fs_lock || !s_flush_lock) return ESP_ERR_NO_MEM;
    esp_vfs_fat_mount_config_t cfg = {
        .max_files = 4,
        .format_if_mount_failed = true,
        .allocation_unit_size = 8192,
    };
    esp_err_t e = esp_vfs_fat_spiflash_mount_rw_wl(MOUNT, "storage", &cfg, &s_wlh);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "fatfs mount failed: %d", e);
        return e;
    }
    uint64_t total = 0, freeb = 0;
    if (esp_vfs_fat_info(MOUNT, &total, &freeb) == ESP_OK) {
        ESP_LOGI(TAG, "fatfs ready: %llu MB total, %llu MB free",
                 total / (1024 * 1024), freeb / (1024 * 1024));
    }
    return ESP_OK;
}

void queue_clear(void)
{
    if (!s_fs_lock || xSemaphoreTake(s_fs_lock, pdMS_TO_TICKS(2000)) != pdTRUE) return;
    DIR *d = opendir(MOUNT);
    if (!d) { xSemaphoreGive(s_fs_lock); return; }
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "q_", 2) == 0) {
            char path[280];
            snprintf(path, sizeof(path), "%s/%s", MOUNT, e->d_name);
            unlink(path);
            n++;
        }
    }
    closedir(d);
    xSemaphoreGive(s_fs_lock);
    ESP_LOGW(TAG, "QUEUE_CLEARED %d files removed", n);
}

void queue_list(void)
{
    if (!s_fs_lock || xSemaphoreTake(s_fs_lock, pdMS_TO_TICKS(2000)) != pdTRUE) return;
    DIR *d = opendir(MOUNT);
    int n = 0;
    if (!d) { ESP_LOGW(TAG, "QUEUE none (not mounted)"); xSemaphoreGive(s_fs_lock); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (is_wav_name(e->d_name)) {
            ESP_LOGI(TAG, "QUEUE_FILE %s", e->d_name);
            n++;
        } else if (strncmp(e->d_name, "q_", 2) == 0 && strstr(e->d_name, ".bad")) {
            ESP_LOGW(TAG, "QUEUE_BAD %s", e->d_name);
        }
    }
    closedir(d);
    xSemaphoreGive(s_fs_lock);
    ESP_LOGI(TAG, "QUEUE_COUNT %d", n);
}

int queue_count(void)
{
    if (!s_fs_lock || xSemaphoreTake(s_fs_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return 0;
    int n = 0;
    DIR *d = opendir(MOUNT);
    if (!d) { xSemaphoreGive(s_fs_lock); return 0; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (is_wav_name(e->d_name)) n++;
    }
    closedir(d);
    xSemaphoreGive(s_fs_lock);
    return n;
}

esp_err_t queue_save(const uint8_t *pcm, int len)
{
    if (!pcm || len <= 0 || !s_fs_lock) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_fs_lock, pdMS_TO_TICKS(5000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    int wav_len = 44 + len;
    if (!ensure_space_locked((uint64_t)wav_len)) {
        ESP_LOGE(TAG, "not enough flash for newest segment (%d bytes)", wav_len);
        xSemaphoreGive(s_fs_lock);
        return ESP_ERR_NO_MEM;
    }

    uint32_t seq = next_sequence_locked();
    char path[96], part[104];
    snprintf(path, sizeof(path), MOUNT "/q_%010lu.wav", (unsigned long)seq);
    snprintf(part, sizeof(part), "%s.part", path);
    FILE *f = fopen(part, "wb");
    if (!f) {
        ESP_LOGE(TAG, "open part failed errno=%d: %s", errno, part);
        xSemaphoreGive(s_fs_lock);
        return ESP_FAIL;
    }

    uint8_t header[44];
    wav_header(header, len, 16000, 1, 16);
    bool ok = fwrite(header, 1, sizeof(header), f) == sizeof(header) &&
              fwrite(pcm, 1, len, f) == (size_t)len &&
              fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = false;
    if (!ok || rename(part, path) != 0) {
        ESP_LOGE(TAG, "atomic queue write failed errno=%d: %s", errno, part);
        xSemaphoreGive(s_fs_lock);
        return ESP_FAIL;
    }
    xSemaphoreGive(s_fs_lock);
    ESP_LOGW(TAG, "queued segment: %s (%d KB)", path, wav_len / 1024);
    return ESP_OK;
}

static int upload_file_to(const char *path, const char *url, const char *token,
                          const char *host_override)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "fopen failed (transient?), keep in queue: %s", path);
        return -1;      /* 稍后重试，绝不删除数据 */
    }
    fseek(f, 0, SEEK_END);
    int total = ftell(f);
    fseek(f, 0, SEEK_SET);
    /* 损坏文件防护：移出上传队列但保留数据，便于后续人工恢复。 */
    if (total < 1000 || total > 12 * 1024 * 1024) {
        ESP_LOGW(TAG, "bad file size %d, quarantine: %s", total, path);
        fclose(f);
        char bad[112];
        snprintf(bad, sizeof(bad), "%s.bad", path);
        if (rename(path, bad) != 0) {
            ESP_LOGW(TAG, "quarantine rename failed errno=%d", errno);
            return UPLOAD_RETRY;
        }
        return UPLOAD_QUARANTINED;
    }

    esp_http_client_config_t cfg = {
        .url = url, .method = HTTP_METHOD_POST,
        .timeout_ms = 120000,
        .crt_bundle_attach = host_override ? NULL : esp_crt_bundle_attach,
        .skip_cert_common_name_check = host_override != NULL,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) { fclose(f); return -1; }
    esp_http_client_set_header(cli, "Content-Type", "audio/wav");
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char segment_id[80];
    snprintf(segment_id, sizeof(segment_id), "%s", base);
    char *ext = strstr(segment_id, ".wav");
    if (ext) *ext = 0;
    esp_http_client_set_header(cli, "X-Segment-Id", segment_id);
    if (host_override) esp_http_client_set_header(cli, "Host", host_override);
    if (token && token[0]) {
        char auth[110];
        snprintf(auth, sizeof(auth), "Bearer %s", token);
        esp_http_client_set_header(cli, "Authorization", auth);
    }
    int status = -1;
    bool paused_for_recording = false;
    if (esp_http_client_open(cli, total) == ESP_OK) {
        /* 分块流式上传，不再整文件加载内存 */
        static uint8_t *chunk = NULL;
        if (!chunk) {
            chunk = heap_caps_malloc(16384, MALLOC_CAP_SPIRAM);
            if (!chunk) { fclose(f); return UPLOAD_RETRY; }
        }
        int written = 0;
        bool io_ok = true;
        while (written < total) {
            if (recorder_is_busy()) {
                paused_for_recording = true;
                io_ok = false;
                break;
            }
            int n = fread(chunk, 1, 16384, f);
            if (n <= 0) { io_ok = false; break; }
            int off = 0;
            while (off < n) {
                if (recorder_is_busy()) {
                    paused_for_recording = true;
                    io_ok = false;
                    break;
                }
                int w = esp_http_client_write(cli, (const char *)chunk + off, n - off);
                if (w <= 0) { io_ok = false; break; }
                off += w;
                written += w;
            }
            if (!io_ok) break;
        }
        if (paused_for_recording) {
            ESP_LOGI(TAG, "queue upload paused for recording: %s (%d/%d)",
                     path, written, total);
        }
        if (io_ok && written == total) {
            esp_http_client_fetch_headers(cli);
            status = esp_http_client_get_status_code(cli);
        }
        ESP_LOGI(TAG, "queue upload %s: %d/%d bytes http=%d", path, written, total, status);
    } else {
        ESP_LOGW(TAG, "queue upload open failed: %s url=%s", path, url);
    }
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    fclose(f);

    if (paused_for_recording) return UPLOAD_PAUSED;
    return (status == 200 || status == 202) ? UPLOAD_OK : UPLOAD_RETRY;
}

static int upload_file(const char *path, const char *url, const char *token)
{
    int rc = upload_file_to(path, url, token, NULL);
    if (rc == UPLOAD_RETRY) {
        ESP_LOGW(TAG, "域名上传失败，尝试 IP 直连兜底");
        rc = upload_file_to(path, CF_FALLBACK_URL, token, CF_FALLBACK_HOST);
    }
    return rc;
}

void queue_flush_async(void) { s_flush_requested = true; }

/* 上传一个声纹样本文件；成功返回 true */
static bool upload_enroll_file(const char *path, const char *url, const char *token, int slot)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 1000 || len > 1024 * 1024) { fclose(f); return false; }

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    esp_http_client_set_header(cli, "Content-Type", "audio/wav");
    char slot_hdr[8];
    snprintf(slot_hdr, sizeof(slot_hdr), "%d", slot);
    esp_http_client_set_header(cli, "X-Slot", slot_hdr);
    if (token && token[0]) {
        char auth[110];
        snprintf(auth, sizeof(auth), "Bearer %s", token);
        esp_http_client_set_header(cli, "Authorization", auth);
    }
    bool ok = false;
    if (esp_http_client_open(cli, (int)len) == ESP_OK) {
        uint8_t *buf = heap_caps_malloc(16384, MALLOC_CAP_SPIRAM);
        int sent = 0;
        while (buf && sent < len) {
            int n = fread(buf, 1, 16384, f);
            if (n <= 0) break;
            int off = 0;
            while (off < n) {
                int w = esp_http_client_write(cli, (const char *)buf + off, n - off);
                if (w <= 0) break;
                off += w;
            }
            if (off < n) break;
            sent += n;
        }
        free(buf);
        esp_http_client_fetch_headers(cli);
        int status = esp_http_client_get_status_code(cli);
        ok = (status == 200);
        ESP_LOGI(TAG, "vp post %s http=%d (%d/%ld bytes)", path, status, sent, len);
    }
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    fclose(f);
    return ok;
}

/* ---------------- 声纹样本（/fat/vp_<slot>.wav） ---------------- */

/* 把声纹 WAV 原子写入 Flash（上传失败也不丢） */
esp_err_t queue_save_enroll(int slot, const uint8_t *wav, int len)
{
    if (s_fs_lock) xSemaphoreTake(s_fs_lock, portMAX_DELAY);
    char path[48];
    snprintf(path, sizeof(path), "%s/vp_%d.wav", MOUNT, slot);
    FILE *f = fopen(path, "wb");
    if (!f) {
        if (s_fs_lock) xSemaphoreGive(s_fs_lock);
        ESP_LOGW(TAG, "vp save fopen failed: %s", path);
        return ESP_FAIL;
    }
    size_t w = fwrite(wav, 1, len, f);
    fclose(f);
    if (s_fs_lock) xSemaphoreGive(s_fs_lock);
    if ((int)w != len) return ESP_FAIL;
    ESP_LOGI(TAG, "vp saved: %s (%d bytes)", path, len);
    return ESP_OK;
}

void queue_remove_enroll(int slot)
{
    char path[48];
    snprintf(path, sizeof(path), "%s/vp_%d.wav", MOUNT, slot);
    unlink(path);
}

/* 重试上传尚未入库的声纹样本；返回剩余数量 */
int queue_flush_enroll(void)
{
    extern bool prov_is_connected(void);
    extern bool recorder_is_busy(void);
    extern const char *recorder_upload_url(void);
    extern const char *recorder_upload_token(void);
    if (!prov_is_connected() || recorder_is_busy()) return 0;

    const char *base = recorder_upload_url();
    const char *tok = recorder_upload_token();
    char url[192];
    snprintf(url, sizeof(url), "%s", base);
    char *cut = strstr(url, "/v1/upload");
    if (cut) snprintf(cut, sizeof(url) - (cut - url), "/v1/enroll");
    else return 0;

    int left = 0;
    if (s_fs_lock) xSemaphoreTake(s_fs_lock, portMAX_DELAY);
    DIR *d = opendir(MOUNT);
    if (!d) {
        if (s_fs_lock) xSemaphoreGive(s_fs_lock);
        return 0;
    }
    struct dirent *e;
    int tried = 0;
    while ((e = readdir(d)) && tried < 3) {
        if (strncmp(e->d_name, "vp_", 3) != 0 || !strstr(e->d_name, ".wav")) continue;
        tried++;
        int slot = 0;
        sscanf(e->d_name + 3, "%d", &slot);
        char path[320];
        snprintf(path, sizeof(path), "%s/%.200s", MOUNT, e->d_name);
        if (upload_enroll_file(path, url, tok, slot)) {
            unlink(path);
            ESP_LOGI(TAG, "vp upload ok slot=%d", slot);
        } else {
            left++;
            ESP_LOGW(TAG, "vp upload failed slot=%d, keep for retry", slot);
        }
    }
    closedir(d);
    if (s_fs_lock) xSemaphoreGive(s_fs_lock);
    return left;
}

int queue_flush_periodic(void)
{
    int count = queue_count();
    if (count <= 0) return 0;
    extern bool prov_is_connected(void);
    if (!prov_is_connected()) return count;
    if (recorder_is_busy()) return count;
    static TickType_t last_flush;
    TickType_t now = xTaskGetTickCount();
    if (!s_flush_requested && last_flush != 0 &&
        (now - last_flush) < pdMS_TO_TICKS(30000)) return count;
    s_flush_requested = false;
    last_flush = now;
    return queue_flush();
}

int queue_flush(void)
{
    if (!s_flush_lock || xSemaphoreTake(s_flush_lock, 0) != pdTRUE) return queue_count();
    extern const char *recorder_upload_url(void);
    extern const char *recorder_upload_token(void);
    const char *url = recorder_upload_url();
    const char *tok = recorder_upload_token();
    queue_entry_t *entries = heap_caps_malloc(sizeof(*entries) * MAX_QUEUE_FILES,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!entries) {
        xSemaphoreGive(s_flush_lock);
        return queue_count();
    }
    int n = 0;
    if (xSemaphoreTake(s_fs_lock, pdMS_TO_TICKS(2000)) == pdTRUE) {
        n = collect_entries(entries, MAX_QUEUE_FILES);
        xSemaphoreGive(s_fs_lock);
    }
    int failed = 0;
    for (int i = 0; i < n; i++) {
        if (recorder_is_busy()) break;
        char path[96];
        snprintf(path, sizeof(path), MOUNT "/%s", entries[i].name);
        int upload_rc = upload_file(path, url, tok);
        if (upload_rc == UPLOAD_OK) {
            if (xSemaphoreTake(s_fs_lock, pdMS_TO_TICKS(2000)) == pdTRUE) {
                if (unlink(path) != 0) ESP_LOGW(TAG, "uploaded but unlink failed errno=%d: %s", errno, path);
                xSemaphoreGive(s_fs_lock);
            }
        }
        else if (upload_rc == UPLOAD_RETRY) {
            failed++;
            if (failed >= 3) break;   /* 连续 3 个失败才停 */
        }
        else if (upload_rc == UPLOAD_PAUSED) break;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    int left = queue_count();
    free(entries);
    xSemaphoreGive(s_flush_lock);
    return left;
}

