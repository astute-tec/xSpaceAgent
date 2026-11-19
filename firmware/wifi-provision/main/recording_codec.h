#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define RECORDING_RATE 16000
#define RECORDING_BITRATE 20000
#define RECORDING_SEGMENT_SECONDS 20
/* CBR, no DTX. Includes encoder drain, Ogg pages, headers and tags. */
#define RECORDING_MAX_SEGMENT_BYTES 54000
#define RECORDING_MIN_OFFLINE_SECONDS 7200
esp_err_t recording_codec_init(void);

/* Owned by caller; invoke only from the dedicated save task. */
esp_err_t recording_encode(const int16_t *pcm, size_t samples, uint32_t serial,
                           uint8_t **data, size_t *length);
