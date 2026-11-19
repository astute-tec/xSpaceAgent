/* 20 kbit/s CBR Opus in standard Ogg, accepted by ffmpeg without custom server codecs. */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_opus_enc.h"
#include "recording_codec.h"
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#endif

#define FRAME_SAMPLES 320
#define PACKETS_PER_PAGE 50
#define PACKET_BYTES 50
#define OPUS_PRESKIP 312

static void le16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void le32(uint8_t *p, uint32_t v) { for (int i=0;i<4;i++) p[i] = v >> (i*8); }
static void le64(uint8_t *p, uint64_t v) { for (int i=0;i<8;i++) p[i] = v >> (i*8); }

/* All packets are less than 255 bytes, so each uses exactly one lacing entry. */
static bool page(uint8_t *out, size_t cap, size_t *used, uint32_t serial,
                 uint32_t seq, uint8_t flags, uint64_t granule,
                 const uint8_t *payload, const uint8_t *sizes, int count)
{
    size_t bytes = 0;
    for (int i=0;i<count;i++) bytes += sizes[i];
    size_t size = 27 + count + bytes;
    if (count <= 0 || count > 255 || *used + size > cap) return false;
    uint8_t *p = out + *used;
    memset(p, 0, 27);
    memcpy(p, "OggS", 4); p[5] = flags;
    le64(p+6, granule); le32(p+14, serial); le32(p+18, seq);
    p[26] = count;
    memcpy(p+27, sizes, count); memcpy(p+27+count, payload, bytes);
    uint32_t crc = 0;
    for (size_t i=0;i<size;i++) {
        crc ^= (uint32_t)p[i] << 24;
        for (int b=0;b<8;b++) crc = (crc << 1) ^ ((crc & 0x80000000U) ? 0x04c11db7U : 0);
    }
    le32(p+22, crc); *used += size;
    return true;
}

static esp_err_t encode_impl(const int16_t *pcm, size_t samples, uint32_t serial,
                            uint8_t **data, size_t *length)
{
    if (!data || !length) return ESP_ERR_INVALID_ARG;
    *data = NULL; *length = 0;
    if (!pcm || !samples || samples > RECORDING_RATE * 30)
        return ESP_ERR_INVALID_ARG;
    esp_opus_enc_config_t cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    cfg.sample_rate = RECORDING_RATE; cfg.channel = 1; cfg.bits_per_sample = 16;
    cfg.bitrate = RECORDING_BITRATE;
    cfg.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
    cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    cfg.complexity = 1; cfg.enable_vbr = false; cfg.enable_dtx = false; cfg.enable_fec = false;
    void *enc = NULL;
    if (esp_opus_enc_open(&cfg, sizeof(cfg), &enc) != ESP_AUDIO_ERR_OK || !enc)
        return ESP_ERR_NO_MEM;
    size_t capacity=samples<=RECORDING_RATE*RECORDING_SEGMENT_SECONDS ? RECORDING_MAX_SEGMENT_BYTES : 81000;
    uint8_t *out = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    /* Encoder output is not stored on the small save-task stack. */
    uint8_t *packet = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *payload = heap_caps_malloc(PACKETS_PER_PAGE * PACKET_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    esp_err_t ret = ESP_FAIL;
    if (!out || !packet || !payload) { ret = ESP_ERR_NO_MEM; goto done; }
    int in_bytes = 0, out_bytes = 0;
    if (esp_opus_enc_get_frame_size(enc, &in_bytes, &out_bytes) != ESP_AUDIO_ERR_OK ||
        in_bytes != FRAME_SAMPLES*2 || out_bytes > 2048) goto done;
    uint16_t preskip = OPUS_PRESKIP;
    esp_audio_enc_info_t info = {0};
    if (esp_opus_enc_get_info(enc, &info) == ESP_AUDIO_ERR_OK &&
        info.spec_info_len >= 19 && info.codec_spec_info &&
        !memcmp(info.codec_spec_info, "OpusHead", 8))
        preskip = info.codec_spec_info[10] | ((uint16_t)info.codec_spec_info[11] << 8);
    if (preskip > 3840) goto done;
    uint8_t head[19] = "OpusHead";
    head[8] = 1; head[9] = 1; le16(head+10, preskip); le32(head+12, RECORDING_RATE);
    uint8_t size = sizeof(head);
    size_t used = 0;
    uint32_t seq = 0;
    if (!page(out, capacity, &used, serial, seq++, 2, 0, head, &size, 1)) goto done;
    uint8_t tags[24] = "OpusTags";
    le32(tags+8, 8); memcpy(tags+12, "WorkLog1", 8); le32(tags+20, 0);
    size = sizeof(tags);
    if (!page(out, capacity, &used, serial, seq++, 0, 0, tags, &size, 1)) goto done;
    /* Drain the algorithmic delay, then trim padding with final Ogg granule. */
    size_t frames = (samples + (preskip+2)/3 + FRAME_SAMPLES-1) / FRAME_SAMPLES;
    uint8_t sizes[PACKETS_PER_PAGE];
    size_t payload_used = 0;
    int count = 0;
    for (size_t i=0;i<frames;i++) {
#ifdef ESP_PLATFORM
        if (i%25==0) vTaskDelay(1); /* Give idle/network work a turn during batch encoding. */
#endif
        int16_t tail[FRAME_SAMPLES] = {0};
        size_t at = i * FRAME_SAMPLES;
        size_t n = at < samples ? samples - at : 0;
        if (n > FRAME_SAMPLES) n = FRAME_SAMPLES;
        const int16_t *input = pcm + (at < samples ? at : samples);
        if (n < FRAME_SAMPLES) { if (n) memcpy(tail, input, n*2); input = tail; }
        esp_audio_enc_in_frame_t in = {.buffer=(uint8_t *)input, .len=FRAME_SAMPLES*2};
        esp_audio_enc_out_frame_t encoded = {.buffer=packet, .len=2048};
        if (esp_opus_enc_process(enc, &in, &encoded) != ESP_AUDIO_ERR_OK ||
            encoded.encoded_bytes != PACKET_BYTES) goto done;
        memcpy(payload+payload_used, packet, encoded.encoded_bytes);
        payload_used += encoded.encoded_bytes; sizes[count++] = encoded.encoded_bytes;
        bool last = i+1 == frames;
        if (count == PACKETS_PER_PAGE || last) {
            uint64_t granule = last ? samples*3 + preskip : (i+1)*960;
            if (!page(out, capacity, &used, serial, seq++, last ? 4 : 0,
                      granule, payload, sizes, count)) goto done;
            count = 0; payload_used = 0;
        }
    }
    *data = out; *length = used; out = NULL; ret = ESP_OK;
done:
    esp_opus_enc_close(enc); free(out); free(packet); free(payload);
    return ret;
}

#ifdef ESP_PLATFORM
typedef struct {
    const int16_t *pcm;
    size_t samples;
    uint32_t serial;
    uint8_t **data;
    size_t *length;
    esp_err_t result;
    SemaphoreHandle_t completed;
} encode_job_t;
static QueueHandle_t s_jobs;
static void codec_worker(void *unused)
{
    encode_job_t *job;
    for (;;) {
        if (xQueueReceive(s_jobs,&job,portMAX_DELAY)!=pdTRUE) continue;
        job->result=encode_impl(job->pcm,job->samples,job->serial,job->data,job->length);
        ESP_LOGI("opus","encoded samples=%u result=%d stack_free=%u",(unsigned)job->samples,
                 job->result,(unsigned)uxTaskGetStackHighWaterMark(NULL));
        xSemaphoreGive(job->completed);
    }
}

esp_err_t recording_codec_init(void)
{
    if (s_jobs) return ESP_OK;
    s_jobs=xQueueCreate(2,sizeof(encode_job_t *));
    if (!s_jobs) return ESP_ERR_NO_MEM;
    /* Codec only: no Flash/NVS/filesystem calls on an external stack. */
    if (xTaskCreateWithCaps(codec_worker,"opus_encode",40960,NULL,4,NULL,
                            MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS) {
        vQueueDelete(s_jobs); s_jobs=NULL; return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t recording_encode(const int16_t *pcm,size_t samples,uint32_t serial,
                           uint8_t **data,size_t *length)
{
    if (!s_jobs) return ESP_ERR_INVALID_STATE;
    encode_job_t job={.pcm=pcm,.samples=samples,.serial=serial,.data=data,.length=length};
    job.completed=xSemaphoreCreateBinary();
    if (!job.completed) return ESP_ERR_NO_MEM;
    encode_job_t *pointer=&job;
    if (xQueueSend(s_jobs,&pointer,pdMS_TO_TICKS(1000))!=pdTRUE) {
        vSemaphoreDelete(job.completed); return ESP_ERR_TIMEOUT;
    }
    /* Caller retains its PCM buffer until the worker has finished. */
    xSemaphoreTake(job.completed,portMAX_DELAY);
    vSemaphoreDelete(job.completed);
    return job.result;
}
#else
esp_err_t recording_codec_init(void) { return ESP_OK; }
esp_err_t recording_encode(const int16_t *pcm,size_t samples,uint32_t serial,
                           uint8_t **data,size_t *length)
{ return encode_impl(pcm,samples,serial,data,length); }
#endif
