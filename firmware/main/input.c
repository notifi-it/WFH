// §7.4: every way of answering lands on these two functions.
#include "input.h"

#include "day.h"
#include "feedback.h"
#include "ui.h"

void input_done(int action) {
    log_event_t ev = {
        .action = action,
        .kind   = KIND_DONE,
        .ts     = time(NULL),
        .slot   = day_current_or_next_slot(action),
    };
    // Gating the cue on the return value matters in use: confirming a card
    // that includes an already-answered row should chime once for what
    // actually landed, not once per row.
    if (day_apply_event(&ev)) feedback_play(CUE_SUCCESS);
}

void input_skip(int action) {
    log_event_t ev = {
        .action = action,
        .kind   = KIND_SKIP,
        .ts     = time(NULL),
        .slot   = day_current_or_next_slot(action),
    };
    if (day_apply_event(&ev)) feedback_play(CUE_READY);
}

void input_toggle_sound(void) {
    g_settings_sound = !g_settings_sound;
    // Order matters: feedback_play gates on the flag, so flipping first makes
    // this naturally silent when switching off — no special case, and no
    // chirp from a board you just muted. The icon carries that half (§7.5).
    if (g_settings_sound) feedback_play(CUE_SOUND_ON);
    ui_sound_icon_update();
}
