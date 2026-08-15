#pragma once

#include "esp_err.h"

typedef enum { AUDIO_BLOOM, AUDIO_SUCCESS, AUDIO_READY } audio_cue_t;

esp_err_t audio_init(int volume);
void      audio_tone(int freq_hz, int ms);
void      audio_cue(audio_cue_t cue);
