#pragma once
#include <stdbool.h>
#include "esp_http_client.h"
/* HTTP response is open, headers validated. Bounded producer/consumer buffer. */
bool audio_play_http(esp_http_client_handle_t client, bool mono, int *played);
