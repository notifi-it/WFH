// SQLite on LittleFS (§3.4). The entire storage surface is these functions —
// which is what makes the §15.1 R1 kill switch cheap: swap the bodies for
// per-day append files and nothing above storage knows.
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "wfh_types.h"

esp_err_t store_open(void);
void      store_close(void);

/** The journal mode actually in force. A journal_mode pragma that cannot
 *  switch does not error, it silently leaves you on DELETE — so callers
 *  check rather than assume. */
const char *store_journal_mode(void);

/** Returns true if this was a new event, false if we already had it.
 *  Dedupe is the UNIQUE (action, slot) constraint, not a scan. */
bool store_add_event(const log_event_t *ev, const settings_t *s);

/** Load one 'YYYY-MM-DD' back into the struct derive walks. */
void store_load_day(const char *day, day_log_t *out, const settings_t *s);

int  store_count_events(void);
void store_prune(int keep_days);

/** 'YYYY-MM-DD' in local time for an epoch. Static buffer, one call deep. */
const char *store_day_key(time_t ts);
