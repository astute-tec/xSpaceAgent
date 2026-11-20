#pragma once
#include <stdbool.h>

/* Read-only: complete single-stream Opus/Ogg with correct page CRCs and EOS.
 * Used only for interrupted .part recovery; incomplete files remain untouched. */
bool recording_file_complete(const char *path);
