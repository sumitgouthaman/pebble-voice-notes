#pragma once

#include <pebble.h>

typedef void (*DictationNoteCallback)(const char *transcription);

void dictation_init(DictationNoteCallback callback);
void dictation_deinit(void);
bool dictation_start(void);
bool dictation_is_supported(void);
