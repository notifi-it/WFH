// §4's choke point. Everything that logs an event comes through here, and
// nothing else writes the log.
#include "day.h"

#include <string.h>

#include "esp_log.h"

#include "config.h"
#include "derive.h"
#include "store.h"
#include "ui.h"

static const char *TAG = "day";

day_log_t  g_day;
day_view_t g_view;
settings_t g_settings;

void day_reload(time_t now) {
    // store_load_day clears the whole struct, and snoozes live in RAM only
    // (§3.1) — reloading would silently forget a Delay-all the user just
    // pressed. Carry them across.
    time_t snoozed[ACTIONS_MAX];
    memcpy(snoozed, g_day.snoozed_until, sizeof snoozed);

    store_load_day(store_day_key(now), &g_day, &g_settings);
    memcpy(g_day.snoozed_until, snoozed, sizeof snoozed);

    wfh_derive(&g_day, now, &g_settings, &g_view);
}

bool day_apply_event(const log_event_t *ev) {
    if (!store_add_event(ev, &g_settings)) return false;   // duplicate: stay quiet

    // Redraw from a derive over the freshly-reloaded log, never by patching
    // the view in place, so the tiles cannot show something storage does not
    // contain.
    day_reload(time(NULL));
    ui_refresh();

    ESP_LOGI(TAG, "%s %s (slot %lld)", g_settings.actions[ev->action].id,
             ev->kind == KIND_DONE ? "done" : "skip", (long long)ev->slot);
    return true;
}

time_t day_current_or_next_slot(int action) {
    for (int i = 0; i < g_view.n_due; i++) {
        if (g_view.due[i] == action) return g_view.due_slot[i];
    }
    return g_view.next[action];        // "I just drank one" — answer the next slot
}
