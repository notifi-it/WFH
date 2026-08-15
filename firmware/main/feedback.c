// §9's cue table — the one place a sound is chosen.
#include "feedback.h"

#include "audio.h"
#include "input.h"

bool g_settings_sound = true;

void feedback_play(cue_t cue) {
    if (!g_settings_sound) return;

    switch (cue) {
    case CUE_BLOOM:    audio_cue(AUDIO_BLOOM);   break;   // prompt appears
    case CUE_SUCCESS:  audio_cue(AUDIO_SUCCESS); break;   // action logged
    case CUE_READY:    audio_cue(AUDIO_READY);   break;   // stretch step done
    case CUE_SOUND_ON: audio_cue(AUDIO_SUCCESS); break;   // proof you can hear it
    }
    // No CUE_SOUND_OFF: muting is confirmed by the icon's pulse (§7.5).
}
