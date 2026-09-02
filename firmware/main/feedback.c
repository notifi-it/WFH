// §9's cue table — the one place a sound is chosen.
#include "feedback.h"

#include "audio.h"

void feedback_play(cue_t cue) {
    switch (cue) {
    case CUE_BLOOM:    audio_cue(AUDIO_BLOOM);   break;   // prompt appears
    case CUE_SUCCESS:  audio_cue(AUDIO_SUCCESS); break;   // action logged
    case CUE_READY:    audio_cue(AUDIO_READY);   break;   // stretch step done
    }
}
