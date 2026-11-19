#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "mic.h"
#include "audio_playback.h"

#define PLAYBACK_BUFFER_BYTES 65536
typedef struct {
    StaticStreamBuffer_t state;
    StreamBufferHandle_t stream;
    SemaphoreHandle_t finished;
    uint8_t *storage;
    bool mono;
    volatile bool input_done;
    volatile bool failed;
    int played;
} playback_t;

static void speaker_task(void *arg)
{
    playback_t *p=arg;
    uint8_t *raw=heap_caps_malloc(2048,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    int16_t *stereo=heap_caps_malloc(4096,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!raw || !stereo) p->failed=true;
    while (!p->failed) {
        size_t n=xStreamBufferReceive(p->stream,raw,2048,pdMS_TO_TICKS(200));
        if (!n) { if (p->input_done) break; else continue; }
        if (n%(p->mono ? 2 : 4)) { p->failed=true; break; }
        const void *output=raw; int bytes=n;
        if (p->mono) {
            const int16_t *in=(const int16_t *)raw;
            for (size_t i=0;i<n/2;i++) stereo[i*2]=stereo[i*2+1]=in[i];
            output=stereo; bytes=n*2;
        }
        if (mic_play(output,bytes)!=bytes) { p->failed=true; break; }
        p->played+=n;
    }
    free(raw); free(stereo);
    xSemaphoreGive(p->finished);
    vTaskDelete(NULL);
}

bool audio_play_http(esp_http_client_handle_t client, bool mono, int *played)
{
    if (played) *played=0;
    playback_t *p=calloc(1,sizeof(*p));
    if (!p) return false;
    p->mono=mono;
    p->storage=heap_caps_malloc(PLAYBACK_BUFFER_BYTES+1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    p->finished=xSemaphoreCreateBinary();
    uint8_t *input=heap_caps_malloc(4096+4,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (p->storage) p->stream=xStreamBufferCreateStatic(PLAYBACK_BUFFER_BYTES+1,4096,p->storage,&p->state);
    bool started=false, complete=false;
    if (!p->storage || !p->stream || !p->finished || !input) goto cleanup;
    if (xTaskCreate(speaker_task,"speaker",4096,p,5,NULL)!=pdPASS) goto cleanup;
    started=true;
    int total=0, tail=0, align=mono ? 2 : 4;
    int64_t deadline=esp_timer_get_time()/1000+120000;
    while (!p->failed && esp_timer_get_time()/1000<deadline) {
        int got=esp_http_client_read(client,(char *)input+tail,4096-tail);
        if (got<=0) {
            complete=got==0 && esp_http_client_is_complete_data_received(client) && tail==0;
            break;
        }
        total+=got;
        if (total>(mono ? 960000 : 1920000)) break;
        int available=got+tail;
        int n=available-available%align;
        int sent=0;
        while (sent<n && !p->failed && esp_timer_get_time()/1000<deadline) {
            sent+=xStreamBufferSend(p->stream,input+sent,n-sent,pdMS_TO_TICKS(500));
        }
        if (sent!=n) break;
        tail=available-n;
        if (tail) memmove(input,input+n,tail);
        if (esp_http_client_is_complete_data_received(client)) { complete=tail==0; break; }
    }
cleanup:
    if (started) {
        p->input_done=true;
        /* Speaker can need up to the bounded buffer duration to drain. */
        xSemaphoreTake(p->finished,portMAX_DELAY);
    }
    bool success=started && complete && !p->failed && p->played>0;
    if (played) *played=p->played;
    if (p->stream) vStreamBufferDelete(p->stream);
    if (p->finished) vSemaphoreDelete(p->finished);
    free(input); free(p->storage); free(p);
    return success;
}
