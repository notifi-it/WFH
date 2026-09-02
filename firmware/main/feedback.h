#pragma once

typedef enum { CUE_BLOOM, CUE_SUCCESS, CUE_READY } cue_t;

void feedback_play(cue_t cue);
