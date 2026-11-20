/* 断网队列：上传失败的录音转单声道存 FATFS，联网后自动补传 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "queue.h"
#include "mic.h"
#include "recorder.h"
#include "menu.h"
#include "recording_codec.h"
#include "recording_integrity.h"
#include "psa/crypto.h"

static const char *TAG = "queue";
static int upload_file(const char *path, const char *url, const char *token);
#define MOUNT "/fat"
#define RECORDING_DIR MOUNT "/rec"
static wl_handle_t s_wlh = WL_INVALID_HANDLE;
static SemaphoreHandle_t s_fs_lock;
static SemaphoreHandle_t s_http_lock;
static SemaphoreHandle_t s_flush_lock;
static volatile bool s_flush_requested;
static char s_uploading_name[64];
static wifi_ps_type_t s_http_previous_ps;
static bool s_http_restore_ps;
static const char *s_save_error = "录音保存失败，尾段仅在内存，请勿关机";

const char *queue_save_error(void) { return s_save_error; }

#define MAX_QUEUE_FILES 2048
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
    if (name && strrchr(name, '/')) name=strrchr(name, '/')+1;
    size_t n = name ? strlen(name) : 0;
    return n > 6 && strncmp(name, "q_", 2) == 0 &&
        (strcmp(name + n - 4, ".wav") == 0 || (n > 7 && strcmp(name+n-5, ".opus") == 0));
}

static unsigned long name_sequence(const char *name)
{
    if (name && strrchr(name, '/')) name=strrchr(name, '/')+1;
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

typedef void (*file_visitor_t)(const char *relative, const char *path, void *arg);

/* New recordings live in a growable subdirectory. Keep reading the legacy root
 * without moving/deleting it, and preserve basename-based server segment IDs. */
static void visit_recording_files(file_visitor_t visitor, void *arg)
{
    const char *prefixes[]={"", "rec/"};
    for (unsigned i=0; i<sizeof(prefixes)/sizeof(prefixes[0]); i++) {
        char folder[32];
        snprintf(folder,sizeof(folder),MOUNT "/%s",prefixes[i]);
        DIR *d=opendir(folder);
        if (!d) continue;
        struct dirent *e;
        while ((e=readdir(d))) {
            if (e->d_name[0]=='.' || e->d_type==DT_DIR) continue;
            char relative[64],path[96];
            int n=snprintf(relative,sizeof(relative),"%s%s",prefixes[i],e->d_name);
            if (n<0 || n>=(int)sizeof(relative)) continue;
            snprintf(path,sizeof(path),MOUNT "/%s",relative);
            visitor(relative,path,arg);
        }
        closedir(d);
    }
}

typedef struct { queue_entry_t *entries; int count; int max; } collect_t;
static void collect_file(const char *relative, const char *path, void *arg)
{
    collect_t *c=arg;
    if (!is_wav_name(relative) || c->count>=c->max) return;
    queue_entry_t *entry=&c->entries[c->count++];
    snprintf(entry->name,sizeof(entry->name),"%s",relative);
    entry->seq=name_sequence(relative);
}

static int collect_entries(queue_entry_t *entries, int max)
{
    collect_t c={.entries=entries,.max=max};
    visit_recording_files(collect_file,&c);
    qsort(entries,c.count,sizeof(entries[0]),entry_compare);
    return c.count;
}

static bool ensure_recording_directory(void)
{
    struct stat st;
    if (stat(RECORDING_DIR,&st)==0) return S_ISDIR(st.st_mode);
    if (mkdir(RECORDING_DIR,0755)==0) return true;
    ESP_LOGE(TAG,"RECORDING_DIRECTORY unavailable errno=%d; preserving existing files",errno);
    return false;
}

static void recover_complete_part(const char *relative, const char *path, void *unused)
{
    const char *base=strrchr(relative,'/');
    base=base?base+1:relative;
    size_t length=strlen(base);
    if (length<12 || strncmp(base,"q_",2) || strcmp(base+length-10,".opus.part")) return;
    if (!recording_file_complete(path)) {
        ESP_LOGW(TAG,"incomplete temporary recording retained: %s",path);
        return;
    }
    char final[96];
    snprintf(final,sizeof(final),RECORDING_DIR "/%.*s",(int)length-5,base);
    /* Never replace another recording, even when interrupted migration is retried. */
    if (access(final,F_OK)==0) {
        ESP_LOGW(TAG,"recovery destination exists; keeping both: %s",path);
        return;
    }
    if (rename(path,final)==0) ESP_LOGI(TAG,"recovered complete recording: %s",final);
    else ESP_LOGW(TAG,"temporary recovery rename failed errno=%d: %s",errno,path);
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
    if (freeb >= needed + QUEUE_RESERVE_BYTES) return true;
    ESP_LOGE(TAG, "STORAGE_FULL free=%llu needed=%llu; preserving all unconfirmed recordings", freeb, needed);
    s_save_error="存储空间不足，录音已暂停，尾段仅在内存，请勿关机";
    return false;
}

esp_err_t queue_init(void)
{
    if (!s_fs_lock) s_fs_lock = xSemaphoreCreateMutex();
    if (!s_flush_lock) s_flush_lock = xSemaphoreCreateMutex();
    if (!s_http_lock) s_http_lock = xSemaphoreCreateMutex();
    if (!s_fs_lock || !s_flush_lock || !s_http_lock) return ESP_ERR_NO_MEM;
    esp_vfs_fat_mount_config_t cfg = {
        .max_files = 4,
        .format_if_mount_failed = false,
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
    if (!ensure_recording_directory()) return ESP_FAIL;
    visit_recording_files(recover_complete_part,NULL);
    return recording_codec_init();
}

void queue_clear(void)
{
    /* Even a maintenance command cannot silently destroy pending work. */
    queue_flush_async();
    ESP_LOGI(TAG, "CLEAN scheduled: only verified server receipts allow local removal");
}

typedef struct { uint64_t queued,other; unsigned pending,retained; } storage_stats_t;
static void storage_file(const char *relative, const char *path, void *arg)
{
    storage_stats_t *stats=arg;
    struct stat st;
    if (stat(path,&st) || !S_ISREG(st.st_mode)) return;
    if (is_wav_name(relative)) { stats->queued+=st.st_size; stats->pending++; }
    else { stats->other+=st.st_size; stats->retained++; }
}

void queue_log_storage(void)
{
    if (!s_fs_lock || xSemaphoreTake(s_fs_lock, pdMS_TO_TICKS(2000)) != pdTRUE) return;
    uint64_t total=0, freeb=0;
    storage_stats_t stats={0};
    if (esp_vfs_fat_info(MOUNT, &total, &freeb) != ESP_OK) {
        ESP_LOGW(TAG, "STORAGE unavailable; no automatic format");
        xSemaphoreGive(s_fs_lock); return;
    }
    visit_recording_files(storage_file,&stats);
    /* Budget is deliberately larger than actual CBR Ogg output; round to 8 KiB clusters. */
    const uint64_t segment_alloc=(RECORDING_MAX_SEGMENT_BYTES+8191)/8192*8192;
    uint64_t available=freeb>QUEUE_RESERVE_BYTES ? freeb-QUEUE_RESERVE_BYTES : 0;
    uint64_t seconds=(available/segment_alloc)*RECORDING_SEGMENT_SECONDS;
    ESP_LOGI(TAG,"STORAGE total=%llu free=%llu queue_files=%u queue_bytes=%llu retained_files=%u retained_bytes=%llu reserve=%u opus_bps=%d offline_seconds=%llu two_hours=%d",
             total,freeb,stats.pending,stats.queued,stats.retained,stats.other,(unsigned)QUEUE_RESERVE_BYTES,
             RECORDING_BITRATE,seconds,seconds>=RECORDING_MIN_OFFLINE_SECONDS);
    xSemaphoreGive(s_fs_lock);
}

static void list_file(const char *relative, const char *path, void *arg)
{
    int *count=arg;
    if (is_wav_name(relative)) { ESP_LOGI(TAG,"QUEUE_FILE %s",relative); (*count)++; }
    else if (strstr(relative,".bad")) ESP_LOGW(TAG,"QUEUE_BAD %s",relative);
}

void queue_list(void)
{
    if (!s_fs_lock || xSemaphoreTake(s_fs_lock, pdMS_TO_TICKS(2000)) != pdTRUE) return;
    int n = 0;
    visit_recording_files(list_file,&n);
    xSemaphoreGive(s_fs_lock);
    ESP_LOGI(TAG, "QUEUE_COUNT %d", n);
}

static void count_file(const char *relative, const char *path, void *arg)
{
    if (is_wav_name(relative)) (*(int *)arg)++;
}

int queue_count(void)
{
    if (!s_fs_lock || xSemaphoreTake(s_fs_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return 0;
    int n = 0;
    visit_recording_files(count_file,&n);
    xSemaphoreGive(s_fs_lock);
    return n;
}

esp_err_t queue_save(const uint8_t *pcm, int len, int64_t captured_at)
{
    if (!pcm || len <= 0 || len % 2 || !s_fs_lock) return ESP_ERR_INVALID_ARG;
    uint8_t *encoded=NULL; size_t encoded_len=0;
    /* Encode outside the filesystem mutex; capture and upload remain independent. */
    esp_err_t result=recording_encode((const int16_t *)pcm,len/2,(uint32_t)esp_timer_get_time(),&encoded,&encoded_len);
    if (result != ESP_OK) { s_save_error="录音编码失败，尾段仅在内存，请勿关机"; return result; }
    if (xSemaphoreTake(s_fs_lock, pdMS_TO_TICKS(5000)) != pdTRUE) { free(encoded); return ESP_ERR_TIMEOUT; }

    if (!ensure_recording_directory()) {
        s_save_error="录音目录不可写，尾段仅在内存，请勿关机";
        free(encoded); xSemaphoreGive(s_fs_lock); return ESP_FAIL;
    }
    if (!ensure_space_locked((encoded_len+8191)/8192*8192)) {
        free(encoded);
        xSemaphoreGive(s_fs_lock);
        return ESP_ERR_NO_MEM;
    }

    uint32_t seq = next_sequence_locked();
    char path[96], part[104];
    snprintf(path, sizeof(path), RECORDING_DIR "/q_%010lu_%010lld.opus", (unsigned long)seq, (long long)captured_at);
    snprintf(part, sizeof(part), "%s.part", path);
    FILE *f = fopen(part, "wb");
    if (!f) {
        s_save_error="无法创建录音文件，尾段仅在内存，请勿关机";
        free(encoded);
        ESP_LOGE(TAG, "open part failed errno=%d: %s", errno, part);
        xSemaphoreGive(s_fs_lock);
        return ESP_FAIL;
    }

    bool ok = fwrite(encoded, 1, encoded_len, f) == encoded_len &&
              fflush(f) == 0 && fsync(fileno(f)) == 0;
    free(encoded);
    if (fclose(f) != 0) ok = false;
    if (!ok || rename(part, path) != 0) {
        s_save_error=ok?"录音重命名失败，文件已保留，请勿关机":"录音写盘失败，尾段仅在内存，请勿关机";
        ESP_LOGE(TAG, "atomic queue write failed errno=%d: %s", errno, part);
        xSemaphoreGive(s_fs_lock);
        return ESP_FAIL;
    }
    xSemaphoreGive(s_fs_lock);
    ESP_LOGI(TAG, "queued Opus: %s bytes=%u pcm=%d", path, (unsigned)encoded_len, len);
    return ESP_OK;
}

static int upload_file_to(const char *path, const char *url, const char *token)
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
    bool compressed = strstr(path,".opus") != NULL;
    if (total < (compressed ? 80 : 44) || total > 12 * 1024 * 1024) {
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
        .timeout_ms = 5000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) { fclose(f); return -1; }
    esp_http_client_set_header(cli, "Content-Type", compressed ? "audio/ogg" : "audio/wav");
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char segment_id[80];
    snprintf(segment_id, sizeof(segment_id), "%s", base);
    char *ext = strrchr(segment_id, '.');
    if (ext) *ext = 0;
    esp_http_client_set_header(cli, "X-Segment-Id", segment_id);
    uint8_t hash[32]; char digest[65]; uint8_t hash_buf[1024]; size_t nr;
    psa_hash_operation_t sha=PSA_HASH_OPERATION_INIT;
    int hash_rc=psa_crypto_init();
    if (!hash_rc) hash_rc=psa_hash_setup(&sha,PSA_ALG_SHA_256);
    while (!hash_rc && (nr=fread(hash_buf,1,sizeof(hash_buf),f))>0)
        hash_rc=psa_hash_update(&sha,hash_buf,nr);
    size_t hash_length=0;
    bool hash_ok=!hash_rc && !ferror(f) && psa_hash_finish(&sha,hash,sizeof(hash),&hash_length)==0 && hash_length==32;
    psa_hash_abort(&sha);
    rewind(f);
    if (!hash_ok) { esp_http_client_cleanup(cli); fclose(f); return UPLOAD_RETRY; }
    for (int i=0;i<32;i++) snprintf(digest+i*2,3,"%02x",hash[i]);
    esp_http_client_set_header(cli,"X-Content-SHA256",digest);
    if (compressed) {
        const char *stamp=strrchr(segment_id,'_');
        if (stamp && strtoll(stamp+1,NULL,10)>=1700000000LL)
            esp_http_client_set_header(cli,"X-Captured-At",stamp+1);
    }
    if (token && token[0]) {
        char auth[110];
        snprintf(auth, sizeof(auth), "Bearer %s", token);
        esp_http_client_set_header(cli, "Authorization", auth);
    }
    int status = -1;
    bool receipt_ok=false;
    if (esp_http_client_open(cli, total) == ESP_OK) {
        http_low_latency(cli);
        /* 分块流式上传，不再整文件加载内存 */
        static uint8_t *chunk = NULL;
        if (!chunk) {
            chunk = heap_caps_malloc(16384, MALLOC_CAP_SPIRAM);
            if (!chunk) {
                esp_http_client_close(cli);
                esp_http_client_cleanup(cli);
                fclose(f);
                return UPLOAD_RETRY;
            }
        }
        int written = 0;
        bool io_ok = true;
        bool paused = false;
        while (written < total) {
            if (recorder_question_active() ||
                (recorder_is_busy() && strcmp(recorder_state(), "recording") != 0) ||
                menu_voice_capture_active()) {
                paused = true;
                io_ok = false;
                break;
            }
            int n = fread(chunk, 1, 16384, f);
            if (n <= 0) { io_ok = false; break; }
            int off = 0;
            while (off < n) {
                if (recorder_question_active() ||
                    (recorder_is_busy() && strcmp(recorder_state(), "recording") != 0) ||
                    menu_voice_capture_active()) { paused = true; io_ok = false; break; }
                int part = n - off < 2048 ? n - off : 2048;
                int w = esp_http_client_write(cli, (const char *)chunk + off, part);
                if (w <= 0) { io_ok = false; break; }
                off += w;
                written += w;
            }
            if (!io_ok) break;
        }
        if (io_ok && written == total) {
            esp_http_client_set_timeout_ms(cli, 60000);
            esp_http_client_fetch_headers(cli);
            status = esp_http_client_get_status_code(cli);
            char response[512]={0};
            int bytes=esp_http_client_read_response(cli,response,sizeof(response)-1);
            if ((status==200 || status==202) && bytes>0 && esp_http_client_is_complete_data_received(cli)) {
                response[bytes]=0;
                cJSON *json=cJSON_ParseWithOpts(response,NULL,true);
                const cJSON *id=cJSON_GetObjectItemCaseSensitive(json,"id");
                const cJSON *size=cJSON_GetObjectItemCaseSensitive(json,"bytes");
                const cJSON *sum=cJSON_GetObjectItemCaseSensitive(json,"sha256");
                receipt_ok=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json,"ok")) &&
                    cJSON_IsString(id) && !strcmp(id->valuestring,segment_id) &&
                    cJSON_IsNumber(size) && size->valuedouble==total &&
                    cJSON_IsString(sum) && !strcmp(sum->valuestring,digest);
                cJSON_Delete(json);
            }
        }
        if (paused) status = -2;
        ESP_LOGI(TAG, "queue upload %s: %d/%d bytes http=%d", path, written, total, status);
    } else {
        ESP_LOGW(TAG, "queue upload open failed: %s url=%s", path, url);
    }
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    fclose(f);

    return receipt_ok ? UPLOAD_OK :
        (status == -2 ? UPLOAD_PAUSED : UPLOAD_RETRY);
}

static int upload_file(const char *path, const char *url, const char *token)
{
    if (!http_lock_take(0)) return UPLOAD_PAUSED;
    if (recorder_question_active() ||
        (recorder_is_busy() && strcmp(recorder_state(), "recording") != 0) ||
        menu_voice_capture_active()) {
        http_lock_give();
        return UPLOAD_PAUSED;
    }
    int rc = upload_file_to(path, url, token);
    http_lock_give();
    return rc;
}

void queue_flush_async(void) { s_flush_requested = true; }

/* 成功返回 200，样本不合格返回 400，其余情况保留待重试。 */
static int upload_enroll_file(const char *path, const char *url, const char *token, int slot,
                              char name[32])
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 1000 || len > 1024 * 1024) { fclose(f); return 400; }

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        fclose(f);
        return -1;
    }
    esp_http_client_set_header(cli, "Content-Type", "audio/wav");
    char slot_hdr[8];
    snprintf(slot_hdr, sizeof(slot_hdr), "%d", slot);
    esp_http_client_set_header(cli, "X-Slot", slot_hdr);
    if (token && token[0]) {
        char auth[110];
        snprintf(auth, sizeof(auth), "Bearer %s", token);
        esp_http_client_set_header(cli, "Authorization", auth);
    }
    int status = -1;
    if (esp_http_client_open(cli, (int)len) == ESP_OK) {
        http_low_latency(cli);
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
        if (sent == len) {
            if (esp_http_client_fetch_headers(cli) >= 0) {
                status = esp_http_client_get_status_code(cli);
                if (status == 200) {
                    char body[768];
                    int length = esp_http_client_read_response(cli, body, sizeof(body) - 1);
                    cJSON *json = NULL;
                    if (length > 0 && esp_http_client_is_complete_data_received(cli)) {
                        body[length] = 0;
                        json = cJSON_ParseWithOpts(body, NULL, true);
                    }
                    const cJSON *ok = cJSON_GetObjectItemCaseSensitive(json, "ok");
                    const cJSON *returned_slot = cJSON_GetObjectItemCaseSensitive(json, "slot");
                    const cJSON *returned_name = cJSON_GetObjectItemCaseSensitive(json, "name");
                    if (cJSON_IsTrue(ok) && cJSON_IsNumber(returned_slot) &&
                        returned_slot->valuedouble == slot && cJSON_IsString(returned_name) &&
                        returned_name->valuestring[0] && strlen(returned_name->valuestring) < 32) {
                        snprintf(name, 32, "%s", returned_name->valuestring);
                    } else {
                        ESP_LOGW(TAG, "vp invalid confirmation slot=%d", slot);
                        status = -1;
                    }
                    cJSON_Delete(json);
                }
            }
            ESP_LOGI(TAG, "vp post %s http=%d (%d/%ld bytes)", path, status, sent, len);
        } else {
            ESP_LOGW(TAG, "vp post incomplete %s (%d/%ld bytes)", path, sent, len);
        }
    }
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    fclose(f);
    return status;
}

/* ---------------- 声纹样本（/fat/vp_<slot>.wav） ---------------- */

/* 把声纹 WAV 原子写入 Flash（上传失败也不丢） */
esp_err_t queue_save_enroll(int slot, const uint8_t *wav, int len)
{
    if (!s_fs_lock || slot < 0 || slot >= 5) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_fs_lock, portMAX_DELAY);
    char path[48], temp[48];
    snprintf(path, sizeof(path), "%s/vp_%d.wav", MOUNT, slot);
    snprintf(temp, sizeof(temp), "%s/vp_%d.part", MOUNT, slot);
    FILE *f = fopen(temp, "wb");
    if (!f) {
        if (s_fs_lock) xSemaphoreGive(s_fs_lock);
        ESP_LOGW(TAG, "vp save fopen failed: %s", path);
        return ESP_FAIL;
    }
    bool ok = fwrite(wav, 1, len, f) == (size_t)len;
    if (fclose(f) != 0) ok = false;
    if (ok) ok = rename(temp, path) == 0;
    if (!ok) unlink(temp);
    xSemaphoreGive(s_fs_lock);
    if (!ok) return ESP_FAIL;
    ESP_LOGI(TAG, "vp saved: %s (%d bytes)", path, len);
    return ESP_OK;
}

bool queue_enroll_pending(int slot)
{
    if (!s_fs_lock || slot < 0 || slot >= 5) return false;
    char path[48];
    snprintf(path, sizeof(path), "%s/vp_%d.wav", MOUNT, slot);
    return access(path, F_OK) == 0;
}

/* 重试上传尚未入库的声纹样本；返回剩余数量 */
int queue_flush_enroll(void)
{
    extern bool prov_is_connected(void);
    extern bool recorder_is_busy(void);
    extern const char *recorder_upload_url(void);
    extern const char *recorder_upload_token(void);
    if (!s_fs_lock || !prov_is_connected() || recorder_is_busy() || menu_voice_capture_active()) return 0;

    const char *base = recorder_upload_url();
    const char *tok = recorder_upload_token();
    char url[192];
    snprintf(url, sizeof(url), "%s", base);
    char *cut = strstr(url, "/v1/upload");
    if (!cut) cut = strstr(url, "/upload");
    if (cut) snprintf(cut, sizeof(url) - (cut - url), "/v1/enroll");
    else return 0;

    int left = 0;
    for (int slot = 0; slot < 5; slot++) {
        if (!queue_enroll_pending(slot)) continue;
        char path[48], name[32] = {0};
        snprintf(path, sizeof(path), "%s/vp_%d.wav", MOUNT, slot);
        int status = -1;
        if (http_lock_take(0)) {
            if (!recorder_is_busy() && !menu_voice_capture_active())
                status = upload_enroll_file(path, url, tok, slot, name);
            http_lock_give();
        }
        bool confirmed = status == 200 && menu_confirm_enroll(slot, name);
        if (confirmed || status == 400) {
            xSemaphoreTake(s_fs_lock, portMAX_DELAY);
            bool removed = unlink(path) == 0;
            xSemaphoreGive(s_fs_lock);
            if (removed) {
                if (status == 400) menu_reject_enroll(slot);
                continue;
            }
        }
        left++;
    }
    return left;
}

/* 全局 HTTP 串行锁：任何时刻只允许一个 HTTPS 请求（内部 RAM 只够一个 TLS 会话） */
void http_low_latency(esp_http_client_handle_t client)
{
    int fd = esp_http_client_get_socket(client);
    int enabled = 1;
    if (fd >= 0 && setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) != 0)
        ESP_LOGW(TAG, "TCP_NODELAY could not be enabled errno=%d", errno);
}

bool http_lock_take(uint32_t timeout_ms)
{
    if (!s_http_lock) return true;
    if (xSemaphoreTake(s_http_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return false;
    s_http_restore_ps = esp_wifi_get_ps(&s_http_previous_ps) == ESP_OK &&
                        s_http_previous_ps != WIFI_PS_NONE;
    if (s_http_restore_ps) esp_wifi_set_ps(WIFI_PS_NONE);
    return true;
}

void http_lock_give(void)
{
    if (s_http_restore_ps) esp_wifi_set_ps(s_http_previous_ps);
    s_http_restore_ps = false;
    if (s_http_lock) xSemaphoreGive(s_http_lock);
}

int queue_flush_periodic(void)
{
    int count = queue_count();
    if (count <= 0) return 0;
    extern bool prov_is_connected(void);
    if (!prov_is_connected()) return count;
    bool recording = strcmp(recorder_state(), "recording") == 0;
    if (recorder_question_active() || (recorder_is_busy() && !recording)) return count;
    static TickType_t last_flush;
    TickType_t now = xTaskGetTickCount();
    if (!s_flush_requested && last_flush != 0 &&
        (now - last_flush) < pdMS_TO_TICKS(recording ? 5000 : 30000)) return count;
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
    bool recording = strcmp(recorder_state(), "recording") == 0;
    for (int i = 0; i < n; i++) {
        if (recorder_question_active() ||
            (recorder_is_busy() && strcmp(recorder_state(), "recording") != 0) ||
            menu_voice_capture_active()) break;
        char path[96];
        snprintf(path, sizeof(path), MOUNT "/%s", entries[i].name);
        if (xSemaphoreTake(s_fs_lock, portMAX_DELAY) != pdTRUE) break;
        snprintf(s_uploading_name, sizeof(s_uploading_name), "%s", entries[i].name);
        xSemaphoreGive(s_fs_lock);
        int upload_rc = upload_file(path, url, tok);
        if (xSemaphoreTake(s_fs_lock, portMAX_DELAY) == pdTRUE) {
            if (upload_rc == UPLOAD_OK && unlink(path) != 0) {
                ESP_LOGW(TAG, "uploaded but unlink failed errno=%d: %s", errno, path);
            }
            s_uploading_name[0] = 0;
            xSemaphoreGive(s_fs_lock);
        }
        if (upload_rc == UPLOAD_PAUSED) break;
        if (upload_rc == UPLOAD_RETRY) {
            failed++;
            if (failed >= 3) break;   /* 连续 3 个失败才停 */
        }
        if (recording) break;          /* 录音期间每轮只传一段，限制网络占用 */
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    int left = queue_count();
    free(entries);
    xSemaphoreGive(s_flush_lock);
    return left;
}

