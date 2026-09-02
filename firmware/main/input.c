// §7.4: every way of answering lands on these two functions.
#include "input.h"

#include "day.h"
#include "derive.h"
#include "feedback.h"

/** One slot ahead is the limit. An early tap answers the next slot ("I just
 *  drank one"), but a second early tap does nothing until that slot actually
 *  opens — otherwise mashing Done pre-fills the whole day. */
static bool answered_ahead(int action) {
    const time_t now = time(NULL);
    // Latest event per slot is the truth (§3.4): a future slot that was
    // answered and then undone is open again.
    for (int i = 0; i < g_day.events_len; i++) {
        const log_event_t *e = &g_day.events[i];
        if (e->action != action || e->slot <= now) continue;
        if (wfh_latest_event(&g_day, action, e->slot)->kind != KIND_UNDO) return true;
    }
    return false;
}

void input_done(int action) {
    if (answered_ahead(action)) return;
    log_event_t ev = {
        .action = action,
        .kind   = KIND_DONE,
        .ts     = time(NULL),
        .slot   = day_current_or_next_slot(action),
    };
    if (ev.slot == 0) return;          // nothing left today to answer
    // Gating the cue on the return value matters in use: confirming a card
    // that includes an already-answered row should chime once for what
    // actually landed, not once per row.
    if (day_apply_event(&ev)) feedback_play(CUE_SUCCESS);
}

void input_undo(int action, time_t slot) {
    // No guard: undo is the guard. It only ever takes back a tap.
    log_event_t ev = { .action = action, .kind = KIND_UNDO, .ts = time(NULL), .slot = slot };
    if (day_apply_event(&ev)) feedback_play(CUE_READY);
}
