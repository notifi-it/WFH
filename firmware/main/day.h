#pragma once

#include <stdbool.h>

#include "wfh_types.h"

extern day_log_t  g_day;
extern day_view_t g_view;
extern settings_t g_settings;

/** Re-read today from storage and re-derive. Preserves RAM-only snoozes. */
void day_reload(time_t now);

/** The only function that writes the log. True if the event was new. */
bool day_apply_event(const log_event_t *ev);

/** Which slot a tap answers: the due one, else the next upcoming one. */
time_t day_current_or_next_slot(int action);
