// The event log on LittleFS (§3.4): one append-only file per local day,
// one TSV line per event. Everything above storage sees only these
// functions; day.c is the sole caller that writes.
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "wfh_types.h"

esp_err_t store_open(void);

/** Returns true if this changed the slot's answer (latest event wins,
 *  §3.4); false for a repeat of what is already latest. */
bool store_add_event(const log_event_t *ev, const settings_t *s);

/** Load one 'YYYY-MM-DD' back into the struct derive walks. */
void store_load_day(const char *day, day_log_t *out, const settings_t *s);

/** Delete day files older than keep_days. */
void store_prune(int keep_days);

/** 'YYYY-MM-DD' in local time for an epoch. Static buffer, one call deep. */
const char *store_day_key(time_t ts);
