// The checklist card (§6): several things are due at once, so show them at
// once. Pure state machine — every side effect goes through card_host_t, so
// this builds and runs on a laptop exactly like derive does.
#pragma once

#include <stdbool.h>

#include "wfh_types.h"

/** One snooze length everywhere: the card's delay-all, the card's X and the
 *  guided flow's X are the same promise. Absolute, not additive. */
#define CARD_SNOOZE_S (15 * 60)

typedef struct {
    int  action;
    bool checked;
} card_row_t;

typedef struct {
    card_row_t rows[ACTIONS_MAX];
    int        len;
    time_t     raised_at;      // when the card last went from empty to not
    bool       escalated;      // §9's re-cue is one-shot per card
} card_t;

/** Everything the card reaches outside itself. The firmware wires these to
 *  input_done (§7.4) and the LVGL screens (§7.3); the test wires them to a
 *  recorder. */
typedef struct {
    void (*log_done)(int action, void *ctx);
    void (*show_card)(void *ctx);
    void (*show_grid)(void *ctx);
    void (*take_stretch)(int action, void *ctx);
    void  *ctx;
} card_host_t;

/** Reconcile the card against derive's due set. Called every tick (§5.2).
 *  Rows are sticky: one still due keeps the checked state the user gave it,
 *  one no longer due disappears. Returns true if the row set changed. */
bool card_sync(card_t *c, const int *due, int n_due, const settings_t *s,
               time_t now, const card_host_t *host);

void card_toggle(card_t *c, int row);
void card_confirm(card_t *c, day_log_t *log, time_t now, const card_host_t *host);
void card_delay_all(card_t *c, day_log_t *log, time_t now, const card_host_t *host);
