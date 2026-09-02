// §4's choke point. Everything that logs an event comes through here, and
// nothing else writes the log.
#include "day.h"

#include <string.h>

#include "esp_log.h"

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
             wfh_kind_name(ev->kind), (long long)ev->slot);
    return true;
}

time_t day_due_slot(int action) {
    for (int i = 0; i < g_view.n_due; i++) {
        if (g_view.due[i] == action) return g_view.due_slot[i];
    }
    return 0;
}

time_t day_current_or_next_slot(int action) {
    const time_t due = day_due_slot(action);
    if (due) return due;
    // Snoozed is off the due list but still the slot being asked about:
    // Done on a snoozed lunch answers lunch, not "the next lunch" (there is
    // none — that path wrote a slot-0 event nothing could display).
    if (g_view.open_slot[action]) return g_view.open_slot[action];
    return g_view.next[action];        // "I just drank one" — answer the next slot; 0 = day over
}
