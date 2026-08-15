// The slot walk (§3.2) and the arithmetic under it (§5.1).
// Pure function of (log, now, settings) — no I/O, no globals, host-testable.
#pragma once

#include "wfh_types.h"

/** "HH:MM" on the calendar day of `day`, resolved in local time.
 *  tm_isdst = -1 so mktime picks the DST flag rather than us guessing. */
time_t wfh_at_time(const struct tm *day, const char *hhmm);

/** The action's first slot on the day containing `ref`.
 *  Interval actions anchor to work_start and land one interval later;
 *  fixed actions take their first authored time at or after work_start. */
time_t wfh_first_slot(const action_def_t *def, time_t ref, const settings_t *s);

/** The next slot strictly after `from`, or 0 if the day has no more.
 *  Interval slots stop before work_end; fixed times are honoured as
 *  authored (18:00 shutdown is meant to fire — see the §5.2 grace). */
time_t wfh_slot_after(const action_def_t *def, time_t from, const settings_t *s);

/** When an unanswered slot stops being due and becomes a miss: the action's
 *  next slot, or work_end + grace for the last slot of the day. */
time_t wfh_window_close(const action_def_t *def, time_t slot, const settings_t *s);

void wfh_derive(const day_log_t *log, time_t now, const settings_t *s, day_view_t *out);
