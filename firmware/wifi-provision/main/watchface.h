#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    WATCHFACE_IDLE,
    WATCHFACE_RECORDING,
    WATCHFACE_LISTENING,
    WATCHFACE_THINKING,
    WATCHFACE_PLAYING,
    WATCHFACE_STOPPING,
    WATCHFACE_ERROR,
    WATCHFACE_SENDING,
    WATCHFACE_OFFLINE,
    WATCHFACE_RECOGNIZING,
    WATCHFACE_TANGSENG,
    WATCHFACE_TTS,
} watchface_state_t;

/* Publishing does not draw or block the audio capture task. */
#define WATCHFACE_ANSWER_BYTES 1537
void watchface_set_answer(const char *text);
bool watchface_has_answer(void);
void watchface_set_recording(bool active);
void watchface_set_status(bool connected, int rssi, int battery_percent, bool charging, bool external_power);
void watchface_draw(watchface_state_t state, const uint16_t *levels, int count, int progress_ms);
