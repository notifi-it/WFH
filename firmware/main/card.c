#include "card.h"

#include <string.h>

#define SNOOZE_SECONDS (15 * 60)

static int row_of(const card_t *c, int action) {
    for (int i = 0; i < c->len; i++) if (c->rows[i].action == action) return i;
    return -1;
}

static bool in_list(const int *list, int n, int action) {
    for (int i = 0; i < n; i++) if (list[i] == action) return true;
    return false;
}

static void remove_row(card_t *c, int row) {
    for (int i = row; i < c->len - 1; i++) c->rows[i] = c->rows[i + 1];
    c->len--;
}

static void clear(card_t *c) {
    c->len = 0;
    c->escalated = false;
}

bool card_sync(card_t *c, const int *due, int n_due, const settings_t *s,
               time_t now, const card_host_t *host) {
    const int was_len = c->len;
    bool changed = false;

    // Drop rows that are no longer due — answered from a grid tile, or their
    // window closed and derive now calls them missed.
    for (int i = c->len - 1; i >= 0; i--) {
        if (!in_list(due, n_due, c->rows[i].action)) { remove_row(c, i); changed = true; }
    }

    // Add newly-due rows, unchecked. Stretches never join the checklist (§6):
    // a due stretch takes the screen on its own and the rest keep waiting.
    int stretch = -1;
    for (int i = 0; i < n_due; i++) {
        if (s->actions[due[i]].flow == FLOW_STRETCH) { if (stretch < 0) stretch = due[i]; continue; }
        if (row_of(c, due[i]) < 0) {
            c->rows[c->len].action  = due[i];
            c->rows[c->len].checked = false;
            c->len++;
            changed = true;
        }
    }

    if (was_len == 0 && c->len > 0) { c->raised_at = now; c->escalated = false; }

    if (stretch >= 0) { host->take_stretch(stretch, host->ctx); return changed; }

    // Only redraw on a change. ui_show_card rebuilds the screen and re-runs
    // its load animation, so calling it every tick would wipe the user's
    // half-made decision once a second.
    if (changed && c->len > 0) host->show_card(host->ctx);
    if (changed && c->len == 0 && was_len > 0) host->show_grid(host->ctx);
    return changed;
}

void card_toggle(card_t *c, int row) {
    if (row >= 0 && row < c->len) c->rows[row].checked = !c->rows[row].checked;
}

void card_confirm(card_t *c, const card_host_t *host) {
    for (int i = c->len - 1; i >= 0; i--) {
        if (c->rows[i].checked) {
            host->log_done(c->rows[i].action, host->ctx);
            remove_row(c, i);
        }
    }
    if (c->len == 0) { clear(c); host->show_grid(host->ctx); }
}

void card_skip_row(card_t *c, int row, const card_host_t *host) {
    if (row < 0 || row >= c->len) return;
    host->log_skip(c->rows[row].action, host->ctx);
    remove_row(c, row);
    if (c->len == 0) { clear(c); host->show_grid(host->ctx); }
}

void card_delay_all(card_t *c, day_log_t *log, time_t now, const card_host_t *host) {
    // Absolute, not additive: mashing the button never pushes anything past
    // now + 15, it just keeps re-stating the same deadline.
    for (int i = 0; i < c->len; i++) log->snoozed_until[c->rows[i].action] = now + SNOOZE_SECONDS;
    clear(c);
    host->show_grid(host->ctx);
}

void card_dismiss(card_t *c, const card_host_t *host) {
    // Not a skip and not a miss, for checked rows as much as unchecked ones.
    // Nothing is written; card_sync rebuilds from derive on the next tick.
    clear(c);
    host->show_grid(host->ctx);
}

void card_key_press(card_t *c, const card_host_t *host) {
    if (c->len == 0) return;
    host->log_done(c->rows[0].action, host->ctx);   // due is priority-sorted
    remove_row(c, 0);
    if (c->len == 0) { clear(c); host->show_grid(host->ctx); }
}
