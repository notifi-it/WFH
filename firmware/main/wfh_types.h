// Core types shared by the firmware and the host test build (§3, §5).
// Pure C99: no ESP-IDF headers here, so firmware/test can compile it with cc.
#pragma once

#include <stdint.h>
#include <time.h>

#define ACTIONS_MAX     16
#define EVENTS_MAX      64
#define TIMES_MAX        8
#define SLOT_WALK_MAX  256      // safety stop; a real day is ~30 slots

typedef enum { CADENCE_INTERVAL, CADENCE_FIXED } cadence_kind_t;
typedef enum { FLOW_TAP, FLOW_STRETCH } flow_t;
/** An undo is an event too: the log stays append-only and the latest event
 *  for an (action, slot) is the truth. A slot whose latest is KIND_UNDO
 *  reads exactly as if it had never been answered. */
typedef enum { KIND_DONE, KIND_SKIP, KIND_UNDO } event_kind_t;

typedef struct {
    cadence_kind_t kind;
    int            every_min;              // CADENCE_INTERVAL
    const char    *times[TIMES_MAX];       // CADENCE_FIXED, "HH:MM" local
    int            n_times;
} cadence_t;

/** One step of a guided flow (§7.6). */
typedef struct { const char *name; int seconds; const char *cue; } stretch_def_t;

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
    const char         *wifi_ssid;         // empty = offline, clock stays seeded
    const char         *wifi_pass;
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

/** What happened to one slot. The tile's dot row is this, in slot order —
 *  a timeline, not a tally: the dot that fills is the slot you answered,
 *  and the ones you let pass stay hollow in front of it. */
typedef enum { SLOT_PEND, SLOT_DONE, SLOT_SKIP, SLOT_MISS, SLOT_DUE } slot_state_t;
#define SLOTS_MAX 16            // per action per day; the tile's dot row shows at most MAX_DOTS (ui.c)

/** Everything rendered. Derived on demand, never stored. */
typedef struct {
    int    counts [ACTIONS_MAX];
    int    skipped[ACTIONS_MAX];
    int    missed [ACTIONS_MAX];
    uint8_t slots [ACTIONS_MAX][SLOTS_MAX]; // slot_state_t, in slot order, up to now
    time_t slot_time[ACTIONS_MAX][SLOTS_MAX];
    time_t slot_ts  [ACTIONS_MAX][SLOTS_MAX]; // when the answer was tapped; 0 if unanswered
    int    n_slots[ACTIONS_MAX];           // slots walked so far today (past + open)
    time_t next   [ACTIONS_MAX];           // 0 = no further slot today
    time_t open_slot[ACTIONS_MAX];         // past slot whose window is open, snoozed or not; 0 = none
    int    due    [ACTIONS_MAX];           // action indices, priority desc
    time_t due_slot[ACTIONS_MAX];          // slot each due entry answers
    int    n_due;
} day_view_t;
