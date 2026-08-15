// Core types shared by the firmware and the host test build (§3, §5).
// Pure C99: no ESP-IDF headers here, so firmware/test can compile it with cc.
#pragma once

#include <time.h>

#define ACTIONS_MAX     16
#define EVENTS_MAX      64
#define TIMES_MAX        8
#define SLOT_WALK_MAX  256      // safety stop; a real day is ~30 slots

typedef enum { CADENCE_INTERVAL, CADENCE_FIXED } cadence_kind_t;
typedef enum { FLOW_TAP, FLOW_STRETCH } flow_t;
typedef enum { KIND_DONE, KIND_SKIP } event_kind_t;

typedef struct {
    cadence_kind_t kind;
    int            every_min;              // CADENCE_INTERVAL
    const char    *times[TIMES_MAX];       // CADENCE_FIXED, "HH:MM" local
    int            n_times;
} cadence_t;

typedef struct {
    const char *id;
    const char *name;
    const char *blurb;
    int         target;
    int         priority;                  // higher sorts first in `due`
    flow_t      flow;
    cadence_t   cadence;
} action_def_t;

typedef struct {
    const char         *work_start;        // "09:00"
    const char         *work_end;          // "18:00"
    const char         *tz;                // POSIX rule string — §3.1a
    int                 grace_min;         // prompt window past work_end, §5.2
    const action_def_t *actions;
    int                 n_actions;
} settings_t;

typedef struct {
    int          action;                   // index into settings->actions
    event_kind_t kind;
    time_t       ts;                       // when the user tapped
    time_t       slot;                     // which slot this answers
} log_event_t;

/** One day as the board tracks it. `events` is the persisted part (§3.4);
 *  `snoozed_until` is RAM-only by design — a reboot mid-snooze forgets it
 *  and the card simply comes back. */
typedef struct {
    log_event_t events[EVENTS_MAX];
    int         events_len;
    time_t      snoozed_until[ACTIONS_MAX];   // 0 = not snoozed
} day_log_t;

/** Everything rendered. Derived on demand, never stored. */
typedef struct {
    int    counts [ACTIONS_MAX];
    int    skipped[ACTIONS_MAX];
    int    missed [ACTIONS_MAX];
    time_t next   [ACTIONS_MAX];           // 0 = no further slot today
    int    due    [ACTIONS_MAX];           // action indices, priority desc
    time_t due_slot[ACTIONS_MAX];          // slot each due entry answers
    int    n_due;
} day_view_t;
