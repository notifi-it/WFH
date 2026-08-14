# WFH Health Tracker — Implementation Plan

Status: UX prototype built and reviewed. This document is the spec for the real build.

---

## 1. Scope

A square-format habit tracker that prompts seven actions on independent timers during working hours, chimes and vibrates, and logs one tap per completion. Today-at-a-glance only, no historical views in v1.

**One build, one source of truth: the board.**

Hardware: Waveshare ESP32-S3-Touch-AMOLED-1.8 (368x448, ESP32-S3R8, 8MB PSRAM, 16MB flash, QMI8658 IMU, PCF85063 RTC, AXP2101 PMIC, speaker, battery).

- The **board** owns the schedule, the clock, the event log, the prompting, the sound and the haptics. It is the product.
- The **web app** is a viewer and a remote. It renders the same day from the same log and can log a completion, but it never schedules and never prompts.

This was previously split into a browser-first phase and a hardware phase. That split meant building a browser scheduler with known-unfixable reliability problems — tab suspension, no haptics on iOS — and then deleting it. The single-phase version skips the throwaway.

The board sits on the desk and prompts the person sitting at it. It does not chase them elsewhere: no phone push, no notifications away from the desk. If you are not there, you are not there.

**The one rule that keeps this coherent:** there is exactly one scheduler, and it runs on the board. Two schedulers writing the same log is survivable, because §3 dedupes. Two schedulers *chiming* is not. When the board is unreachable the web app degrades to a read-and-log client — it does not start prompting to fill the gap.

**Explicitly out of scope:** eye breaks (20-20-20), the lunchtime walk, and any "silence for the day" control. All three were considered and cut. No historical views in v1 — see §13.

---

## 2. Actions

| Action | Cadence | Daily target | Tint | Flow |
|---|---|---|---|---|
| Stand break | every 40 min | 10 | green `#7fd4a8` | single tap |
| Water | every 45 min | 8 | blue `#6ec3e0` | single tap |
| Shoulder roll | every 60 min | 8 | violet `#b6a3e8` | single tap |
| Snack | 10:45, 15:30 | 2 | amber `#e8b06a` | single tap |
| Lunch | 13:00 | 1 | orange `#e8926a` | single tap |
| Stretches | 11:30, 16:30 | 2 | yellow `#e0d16a` | guided, 4 steps |
| Shut down | 18:00 | 1 | grey `#8f9aa8` | single tap |

Working hours 09:00–18:00. No prompts outside the window. Timers reset at 09:00 the next day.

**Stretch sequence:** chin tucks (30s), doorway pec stretch (40s), cat-cow (40s), hip flexor stretch (50s).

### 2.1 One definition, two languages

The board and the web app both need this table, and two hand-maintained copies will drift the first time a cadence changes. Define it once as JSON and generate both:

```json
// config/actions.json — the only place this table is edited
{
  "workStart": "09:00",
  "workEnd": "18:00",
  "actions": [
    { "id": "stand", "name": "Stand break", "blurb": "Up on your feet for a minute.",
      "icon": "person-check", "tint": "#7fd4a8", "target": 10, "flow": "tap", "priority": 1,
      "cadence": { "kind": "interval", "everyMin": 40, "startOffsetMin": 12 } },

    { "id": "water", "name": "Water", "blurb": "Glass of water. Refill while you are up.",
      "icon": "drop", "tint": "#6ec3e0", "target": 8, "flow": "tap", "priority": 1,
      "cadence": { "kind": "interval", "everyMin": 45, "startOffsetMin": 7 } },

    { "id": "roll", "name": "Shoulder roll", "blurb": "Ten slow rolls back, then drop the shoulders.",
      "icon": "refresh", "tint": "#b6a3e8", "target": 8, "flow": "tap", "priority": 1,
      "cadence": { "kind": "interval", "everyMin": 60, "startOffsetMin": 3 } },

    { "id": "snack", "name": "Snack", "blurb": "Something small before the slump.",
      "icon": "apple", "tint": "#e8b06a", "target": 2, "flow": "tap", "priority": 2,
      "cadence": { "kind": "fixed", "times": ["10:45", "15:30"] } },

    { "id": "lunch", "name": "Lunch", "blurb": "Away from the desk.",
      "icon": "bowl", "tint": "#e8926a", "target": 1, "flow": "tap", "priority": 2,
      "cadence": { "kind": "fixed", "times": ["13:00"] } },

    { "id": "stretch", "name": "Stretches", "blurb": "Four stretches, about three minutes.",
      "icon": "sun", "tint": "#e0d16a", "target": 2, "flow": "stretch", "priority": 3,
      "cadence": { "kind": "fixed", "times": ["11:30", "16:30"] } },

    { "id": "shutdown", "name": "Shut down", "blurb": "Close the laptop. That is the day.",
      "icon": "moon", "tint": "#8f9aa8", "target": 1, "flow": "tap", "priority": 2,
      "cadence": { "kind": "fixed", "times": ["18:00"] } }
  ],
  "stretches": [
    { "name": "Chin tucks",          "seconds": 30, "cue": "Draw the chin straight back. Hold, release, repeat." },
    { "name": "Doorway pec stretch", "seconds": 40, "cue": "Forearms on the frame, step through, chest open." },
    { "name": "Cat-cow",             "seconds": 40, "cue": "On all fours, arch and round with the breath." },
    { "name": "Hip flexor stretch",  "seconds": 50, "cue": "Half kneel, tuck the pelvis, 25s each side." }
  ]
}
```

```
tools/gen-config.mjs  →  firmware/main/actions.g.h      (C: static const action_def_t[])
                      →  web/src/config/actions.g.ts    (TS: ACTIONS, BY_ID, STRETCH_SET)
```

Both generated files are committed and both are checked in CI (`gen-config && git diff --exit-code`), so a hand-edit of a generated file fails the build rather than silently surviving.

`startOffsetMin` is jitter expressed as data: stand fires at 09:12, water at 09:07, roll at 09:03. The three interval actions collide far less than they would from a common 09:00 anchor — see §5.

---

## 3. Data model

Shared verbatim between the board and the web app. This is what makes the two interchangeable rather than merely connected.

### 3.1 What is actually true

Three things are facts. Everything else is a view over them.

1. **The schedule** — config, from §2.1. Not per-day data.
2. **The event log** — what the user did. Append-only. `done` and `skip`, nothing else.
3. **The awake windows** — the periods the board was actually running and able to prompt.

The load-bearing idea: **a miss is not an event, it is an absence.** Nothing happens when a slot is missed — that is what missing it means. So there is nothing to write. A slot is missed if it is in the past, it fell inside an awake window, and no `done` or `skip` answers it.

Deriving rather than writing buys three things:

- It cannot drift. There is no second copy to disagree with the log.
- **There is no backfill problem.** Slots that passed while nothing was running aren't in an awake window, so they aren't missed. No repair pass on boot, no setting, no decision.
- **A board that slept, ran flat, or was unplugged is just a gap in `awake`.** No special case. This is the entire answer to §10.

Storing misses would mean a board that ran flat overnight wakes up and writes forty misses for slots nobody was ever prompted about. Deriving them means it wakes up and writes nothing, because nothing happened.

> The app can only mark you as having missed something it actually asked you about.

```json
{
  "version": 2,
  "date": "2026-08-14",
  "events": [
    { "id": "…", "action": "water", "kind": "done", "ts": 1723645210, "slot": 1723645020, "source": "board" },
    { "id": "…", "action": "stand", "kind": "skip", "ts": 1723646400, "slot": 1723646400, "source": "web" }
  ],
  "awake": [ [1723640400, 1723645210], [1723649000, 1723652600] ],
  "snoozedUntil": { "stand": 1723647300 }
}
```

```ts
// web/src/state/types.ts — mirrored by log_event_t / day_log_t in firmware
export type EventKind = 'done' | 'skip';        // no 'miss' — see above

export interface LogEvent {
  id: string;          // uuid v4
  action: ActionId;
  kind: EventKind;
  ts: number;          // epoch seconds — when the user tapped
  slot: number;        // epoch seconds — which slot this answers; dedupe key
  source: 'board' | 'web';
}

export type Awake = [start: number, end: number][];

/** Everything persisted for one day. Facts only. */
export interface DayLog {
  version: 2;
  date: string;                                   // YYYY-MM-DD, local
  events: LogEvent[];
  awake: Awake;
  snoozedUntil: Partial<Record<ActionId, number>>;
}

/** Everything rendered. Derived on demand, never stored, never transmitted. */
export interface DayView {
  counts:  Partial<Record<ActionId, number>>;
  skipped: Partial<Record<ActionId, number>>;
  missed:  Partial<Record<ActionId, number>>;
  next:    Partial<Record<ActionId, number>>;
  due:     ActionId[];
}
```

`slot` is the field that makes the whole thing idempotent. A retry, a second browser tab, the board and the web app both logging the same prompt — all collapse to one entry because `(action, slot)` is unique. It is also the join key between the schedule and the log, which is what lets a miss be derived at all.

`source` is diagnostic only. Nothing branches on it; it exists so that "did I tap this on the board or on my laptop?" is answerable when something looks wrong.

### 3.2 Derivation

One pass per action, walking that action's slots forward and folding in the events that answer them. Seven actions, at most ~30 slots each — a few hundred comparisons, cheap enough to run on every tick on an ESP32.

```ts
export function derive(log: DayLog, now: number, s: Settings, day = new Date()): DayView {
  const view: DayView = { counts: {}, skipped: {}, missed: {}, next: {}, due: [] };

  const answered = new Map<string, LogEvent>();
  for (const e of log.events) answered.set(`${e.action}@${e.slot}`, e);

  for (const def of ACTIONS) {
    let slot = firstSlot(def, s, day);
    let anchor: number | null = null;      // set when an interval action is done early

    while (slot != null) {
      const ev = answered.get(`${def.id}@${slot}`);

      if (ev?.kind === 'done') {
        view.counts[def.id] = (view.counts[def.id] ?? 0) + 1;
        if (def.cadence.kind === 'interval') anchor = ev.ts;   // re-anchor. §5.3
      } else if (ev?.kind === 'skip') {
        view.skipped[def.id] = (view.skipped[def.id] ?? 0) + 1;
      } else if (slot <= now) {
        // Unanswered and in the past. Missed only if we were awake to ask.
        if (wasAwake(log.awake, slot)) {
          view.missed[def.id] = (view.missed[def.id] ?? 0) + 1;
        }
        if (isDueNow(slot, now, log.snoozedUntil[def.id])) view.due.push(def.id);
      } else {
        view.next[def.id] = slot;          // first future slot; stop here
        break;
      }

      slot = slotAfter(def, anchor ?? slot, s, day);
      anchor = null;
    }
  }

  view.due.sort((a, b) => BY_ID[b].priority - BY_ID[a].priority);
  return view;
}

export const wasAwake = (awake: Awake, t: number): boolean =>
  awake.some(([from, to]) => t >= from && t <= to);
```

The subtlety is `anchor`. §5.3's rule — an interval action done early resets its timer from the tap — means the slot grid for `stand` is not a fixed lattice; it bends every time you get ahead. Folding the anchor forward during the walk reproduces exactly the sequence the live scheduler produced, which is what makes the derived view agree with what the user actually saw on the board.

### 3.3 `derive` runs on the board only

An earlier draft had `derive` implemented twice — C for the board, TypeScript for the web app — with shared test fixtures to keep them honest. That is two implementations of a fiddly walk in two languages that must agree exactly, and it was the largest correctness risk in the design.

**Simpler: only the board derives.** It sends the web app the finished `DayView` alongside the log, and the web app renders what it is given.

```
board:    SQLite ──► derive() ──► DayView ──┐
                                            ├──► GET /state  {log, view, power}
          SQLite ──────── DayLog ───────────┘
web app:  render(view)          // no derive, no slot maths, no cadence logic
```

The rule this replaces was "ship facts, not conclusions, so the two can never disagree about what a miss is". Removing the second implementation achieves the same goal more directly — there is nothing left to disagree. The web app drops the slot walk, the cadence branching and the anchor logic entirely; it is a renderer.

The log is still sent, for two reasons: it is small (~4KB), and it lets the web app show something sensible while the board is unreachable (§8.4). But it is never the thing that produces the numbers on screen.

Fixtures still earn their place for the one remaining implementation — see §11.1.

### 3.4 Awake windows

Written by the same tick that drives everything else. Extend the current window if the last tick was recent; otherwise open a new one, because a gap means the board was not running.

```c
#define AWAKE_GAP_SEC 120   // > one slow tick, << the shortest cadence

void day_mark_awake(day_log_t *log, time_t now) {
    awake_win_t *last = log->awake_len ? &log->awake[log->awake_len - 1] : NULL;
    if (last && now - last->fin <= AWAKE_GAP_SEC) {
        last->fin = now;                                   // extend
    } else if (log->awake_len < AWAKE_MAX) {
        log->awake[log->awake_len++] = (awake_win_t){ .start = now, .fin = now };
    }
}
```

`AWAKE_GAP_SEC` at 120 is the judgement call. Too tight and ordinary scheduling jitter fragments the day into hundreds of windows that start reporting phantom misses for slots the board *was* awake for. Too loose and a genuinely-off board looks awake. Two minutes clears any plausible tick delay while staying well under the 40-minute shortest cadence.

The array stays small — a normal day is one to five windows — because extension mutates the last entry rather than appending. `AWAKE_MAX` of 32 is generous; if it ever fills, stop extending rather than wrapping, since a lost window costs at most a few phantom misses while a wrapped one corrupts the day.

### 3.5 Persistence — SQLite

The board's store is a SQLite database on a LittleFS partition. The full rationale is in §13; the short version is that the model from §3.1 *is* a table, and SQL expresses it more plainly than hand-rolled blob packing does.

**The schema is the data model.** Reading it should tell you everything §3.1 says.

```sql
-- firmware/main/schema.sql  (embedded in the binary, run at every boot)

CREATE TABLE IF NOT EXISTS events (
  id      TEXT PRIMARY KEY,        -- uuid v4
  day     TEXT NOT NULL,           -- 'YYYY-MM-DD', local time
  action  TEXT NOT NULL,           -- 'water', 'stand', …
  kind    TEXT NOT NULL CHECK (kind IN ('done', 'skip')),
  ts      INTEGER NOT NULL,        -- epoch seconds: when the user tapped
  slot    INTEGER NOT NULL,        -- epoch seconds: which slot this answers
  source  TEXT NOT NULL,           -- 'board' | 'web'  (diagnostic only)

  -- One answer per slot. This single line replaces every dedupe check in
  -- the codebase: retries, double-taps, and the web app and the board
  -- logging the same prompt all collapse here.
  UNIQUE (action, slot)
);

CREATE INDEX IF NOT EXISTS events_day ON events (day);

-- When the board was awake and able to prompt. A slot outside every window
-- was never asked about, so it can never be a miss. See §3.1.
CREATE TABLE IF NOT EXISTS awake (
  day     TEXT NOT NULL,
  start   INTEGER NOT NULL,
  fin     INTEGER NOT NULL,
  PRIMARY KEY (day, start)
);

CREATE TABLE IF NOT EXISTS settings (
  key     TEXT PRIMARY KEY,
  value   TEXT NOT NULL
);
```

**Opening it.** The ESP32 port needs a VFS backed by a filesystem partition; LittleFS is the better choice over FATFS here for its wear levelling and its tolerance of unclean shutdowns, which a battery-powered device will produce.

```c
// firmware/main/store.c
static sqlite3 *g_db;

esp_err_t store_open(void) {
    esp_vfs_littlefs_conf_t fs = {
        .base_path = "/fs", .partition_label = "storage", .format_if_mount_failed = true,
    };
    ESP_ERROR_CHECK(esp_vfs_littlefs_register(&fs));

    if (sqlite3_open("/fs/wfh.db", &g_db) != SQLITE_OK) {
        ESP_LOGE(TAG, "open failed: %s", sqlite3_errmsg(g_db));
        return ESP_FAIL;
    }

    // WAL keeps a power cut mid-write from corrupting the database: the
    // last committed transaction survives and the partial one is discarded.
    sqlite3_exec(g_db, "PRAGMA journal_mode=WAL;",   NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA synchronous=FULL;",   NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA foreign_keys=ON;",    NULL, NULL, NULL);
    sqlite3_exec(g_db, SCHEMA_SQL, NULL, NULL, NULL);   // CREATE IF NOT EXISTS
    return ESP_OK;
}
```

`synchronous=FULL` costs a flush per write. At ~30 writes a day that is free, and it is what makes "the screen said Done, so it is recorded" true across a battery cut.

**Writing an event** is one statement. Note there is no dedupe code — `INSERT OR IGNORE` against the `UNIQUE` constraint does it, and `sqlite3_changes` tells us whether the row was new.

```c
// Returns true if this was a new event, false if we already had it.
bool store_add_event(const log_event_t *ev) {
    static const char *SQL =
        "INSERT OR IGNORE INTO events (id, day, action, kind, ts, slot, source)"
        " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7);";

    sqlite3_stmt *st;
    sqlite3_prepare_v2(g_db, SQL, -1, &st, NULL);
    sqlite3_bind_text(st, 1, ev->id,                  -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, day_key(ev->ts),         -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, ACTIONS[ev->action].id,  -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 4, ev->kind == KIND_DONE ? "done" : "skip", -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 5, ev->ts);
    sqlite3_bind_int64(st, 6, ev->slot);
    sqlite3_bind_text(st, 7, ev->source == SOURCE_BOARD ? "board" : "web", -1, SQLITE_STATIC);

    int rc = sqlite3_step(st);
    sqlite3_finalize(st);

    if (rc != SQLITE_DONE) { ESP_LOGE(TAG, "insert: %s", sqlite3_errmsg(g_db)); return false; }
    return sqlite3_changes(g_db) > 0;
}
```

**Reading a day back** into the struct `derive` walks:

```c
void store_load_day(const char *day, day_log_t *out) {
    out->events_len = 0;
    out->awake_len  = 0;

    sqlite3_stmt *st;
    sqlite3_prepare_v2(g_db,
        "SELECT action, kind, ts, slot FROM events WHERE day = ?1 ORDER BY ts;",
        -1, &st, NULL);
    sqlite3_bind_text(st, 1, day, -1, SQLITE_STATIC);

    while (sqlite3_step(st) == SQLITE_ROW && out->events_len < EVENTS_MAX) {
        log_event_t *e = &out->events[out->events_len++];
        e->action = action_from_id((const char *)sqlite3_column_text(st, 0));
        e->kind   = strcmp((const char *)sqlite3_column_text(st, 1), "done") == 0
                    ? KIND_DONE : KIND_SKIP;
        e->ts     = sqlite3_column_int64(st, 2);
        e->slot   = sqlite3_column_int64(st, 3);
    }
    sqlite3_finalize(st);

    sqlite3_prepare_v2(g_db,
        "SELECT start, fin FROM awake WHERE day = ?1 ORDER BY start;", -1, &st, NULL);
    sqlite3_bind_text(st, 1, day, -1, SQLITE_STATIC);
    while (sqlite3_step(st) == SQLITE_ROW && out->awake_len < AWAKE_MAX) {
        out->awake[out->awake_len++] = (awake_win_t){
            .start = sqlite3_column_int64(st, 0),
            .fin   = sqlite3_column_int64(st, 1),
        };
    }
    sqlite3_finalize(st);
}
```

**Awake windows** get an upsert, because the tick extends the current window every second and we do not want a row per second:

```c
void store_mark_awake(time_t now) {
    static const char *SQL =
        "INSERT INTO awake (day, start, fin) VALUES (?1, ?2, ?2)"
        // Extend the open window if the last tick was recent; a longer gap
        // means the board was off, so we let a new row be inserted instead.
        " ON CONFLICT (day, start) DO UPDATE SET fin = ?2;";
    ...
}
```

The board holds today's `start` in RAM so it knows which row to update; on boot, or after a gap longer than `AWAKE_GAP_SEC`, it starts a new one. That gap rule is the whole of §3.4 and is unchanged by the move to SQL.

**Retention** is one statement on boot instead of a key-scanning prune:

```sql
DELETE FROM events WHERE day < date('now', 'localtime', '-400 days');
DELETE FROM awake  WHERE day < date('now', 'localtime', '-400 days');
```

400 days rather than 14: SQLite makes keeping history nearly free, and a year plus a margin is what makes §13's queries possible later without a migration.

### 3.6 Reconciling the web app

There is one writer of record — the board — so there is no merge algorithm to write. When the web app is offline it holds unsent taps in an outbox and replays them on reconnect. The `UNIQUE (action, slot)` constraint absorbs whatever arrives twice.

```ts
// The entire offline story. Replay is safe at any multiplicity.
async function flushOutbox(board: Backend) {
  for (const ev of [...outbox]) {
    await board.log(ev);        // INSERT OR IGNORE on the board
    outbox.remove(ev);
  }
}
```

Only the board writes `awake` rows. A browser tab being open says nothing about whether the user was prompted, and a laptop left open at 03:00 must not turn the night into missed slots.

---

## 4. Firmware architecture

Four FreeRTOS tasks. Everything that mutates the log funnels through one of them.

```
┌─────────────┐  due actions  ┌─────────────┐
│  scheduler  │ ────────────► │     ui      │  LVGL: grid, prompts, stretches
│   1 Hz tick │               │ touch + key │
└──────┬──────┘               └──────┬──────┘
       │                             │ input_done() / input_skip()   §7.4
       │ store_mark_awake            ▼
       │                    ┌─────────────────┐      ┌──────────┐
       └───────────────────►│ day_apply_event │─────►│ feedback │ sound + haptic
                            │  ← ONLY writer  │      └──────────┘
                            └────────┬────────┘
                                     │
                    ┌────────────────┼────────────────┐
                    ▼                ▼                ▼
              ┌──────────┐    ┌────────────┐   ┌────────────┐
              │  SQLite  │    │ derive()   │   │  ws_broad- │
              │ /fs/wfh  │    │ → DayView  │   │  cast_state│──► browsers
              └──────────┘    └────────────┘   └────────────┘
                    ▲
                    │  POST /event
              ┌─────┴──────┐
              │   httpd    │  /state  /event  /ws  /  (web app)
              └────────────┘
```

`day_apply_event` is the only function that writes the log. The touch handler calls it, `POST /event` calls it, and nothing else does. That single choke point is what makes a board tap and a browser tap genuinely the same operation rather than two code paths that happen to look alike.

```c
// firmware/main/day.c
// The choke point. Everything that logs an event comes through here.
// Returns true if this was new, false if we already had it.
bool day_apply_event(const log_event_t *ev) {
    bool is_new = store_add_event(ev);      // INSERT OR IGNORE, §3.5

    if (!is_new) return false;              // duplicate: nothing changed, stay quiet

    store_load_day(day_key(ev->ts), &g_day); // refresh the in-RAM copy
    derive(&g_day, time(NULL), &g_settings, &g_view);
    ui_refresh();                            // redraw tiles from the new view
    ws_broadcast_state();                    // push to every connected browser
    return true;
}
```

Two properties this gives everything upstream of it:

- **A duplicate is not an error.** A web client retrying a request whose response it never saw, or a user tapping Done twice, gets a quiet no-op rather than a failure or a double count.
- **The screen and the socket update from the same place.** There is no path where the board's tiles and the browser's tiles come from different computations, because both are driven by the single `derive` that runs here.

Reloading the whole day from SQLite after each insert rather than patching the in-RAM struct is deliberate: it is a few hundred microseconds, it happens ~30 times a day, and it guarantees the RAM copy can never drift from what is actually stored.

---

## 5. Scheduler

Runs on the board, in C, at 1 Hz. One timer, not one per action.

- Each tick marks the board awake and asks `derive` what is due.
- **Doing an action early resets its timer from now**, so getting ahead pushes the next slot out rather than being penalised.
- **Missed slots do not stack.** This is not enforced; it is a consequence of `derive` visiting each slot exactly once.
- **Jittered start offsets** (§2.1) keep the three interval actions from aligning.

### 5.1 Slot arithmetic

```c
// Local time throughout. setenv("TZ", ...) + tzset() at boot, so DST is handled
// by the C library rather than by hand.
static time_t at_time(const struct tm *day, const char *hhmm) {
    struct tm t = *day;
    t.tm_hour = (hhmm[0]-'0')*10 + (hhmm[1]-'0');
    t.tm_min  = (hhmm[3]-'0')*10 + (hhmm[4]-'0');
    t.tm_sec  = 0;
    t.tm_isdst = -1;              // let mktime resolve the DST flag
    return mktime(&t);
}

time_t slot_after(const action_def_t *def, time_t from, const settings_t *s) {
    struct tm day; localtime_r(&from, &day);
    time_t end = at_time(&day, s->work_end);

    if (def->cadence.kind == CADENCE_INTERVAL) {
        time_t next = from + def->cadence.every_min * 60;
        return next < end ? next : 0;
    }
    for (int i = 0; i < def->cadence.n_times; i++) {
        time_t t = at_time(&day, def->cadence.times[i]);
        if (t > from) return t;
    }
    return 0;
}
```

Do the date maths in local time via `mktime`/`localtime_r`, never in UTC by hand. A user in a DST-shifting zone would otherwise see the whole day slide by an hour on the changeover. `tm_isdst = -1` lets `mktime` resolve the ambiguous hour rather than guessing.

### 5.2 The tick

```c
static void scheduler_task(void *arg) {
    for (;;) {
        time_t now = time(NULL);

        if (!day_is_today(&g_day, now)) {        // rollover, §11.2
            store_close_awake(now);
            day_init(&g_day, now);
        }

        store_mark_awake(now);                   // §3.4 — the same write that
                                                 // makes misses correct

        if (in_working_hours(now, &g_settings)) {
            day_view_t v;
            derive(&g_day, now, &g_settings, &v);
            for (int i = 0; i < v.n_due; i++) {
                if (!queue_contains(v.due[i]) && !is_snoozed(v.due[i], now)) {
                    queue_push(v.due[i]);
                    ui_raise_prompt();           // wakes screen, sound, haptics
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```

The tick writes no tallies and rolls nothing forward — `derive` owns all of that. All it does is mark presence and raise prompts.

### 5.3 Completing early

Logging is just an append (§7.4's `input_done`). The interval/fixed asymmetry lives entirely in `derive`'s `anchor` and nowhere else: drinking water at 11:20 pushes the next glass to 12:05, while eating lunch at 12:40 leaves the 13:00 slot exactly where it was. Having that branch in one place rather than at every call site is the point.

```c
/** Which slot is this tap answering? The due one if there is one, else the
 *  next upcoming one — which is what makes a grid tile tap ("I just drank
 *  one") work through the same path as answering a prompt. */
time_t current_or_next_slot(action_id_t id) {
    for (int i = 0; i < g_view.n_due; i++) {
        if (g_view.due[i] == id) return g_view.due_slot[i];
    }
    return g_view.next[id];
}
```

---

## 6. Prompt queue

Collisions are common: stand (40m), water (45m) and roll (60m) align exactly every 6 hours and near-align constantly.

- Prompts **queue**; only one card is visible at a time.
- The card shows `1 OF n` when more are waiting.
- **Done** logs the action and advances to the next in the queue.
- **Skip this one** logs an explicit skip for that action only and advances.
- **Delay all 15 min** clears the entire queue. A meeting blocks everything, so one tap covers all of it. Repeatable, never stacks.
- **X (top right)** dismisses the whole queue and returns to the grid. Present on every overlay including mid-stretch.

Stretches never merge into a collision group. If stretches collide with anything, stretches take priority and the rest are dropped from the queue — they need no bookkeeping now that misses are derived, and they re-raise on the next tick if still due after the set.

```c
void queue_delay_all(time_t now) {
    // One tap covers the whole meeting. Snoozing is absolute, not additive,
    // so mashing the button never pushes anything past now + 15.
    time_t until = now + 15 * 60;
    for (int i = 0; i < g_queue_len; i++) g_day.snoozed_until[g_queue[i]] = until;
    g_queue_len = 0;
    ui_show_grid();
}

void queue_dismiss(void) {
    // Not a skip and not a miss. Nothing is written; the slots re-raise on
    // the next tick if still due.
    g_queue_len = 0;
    ui_show_grid();
}
```

`dismiss` deliberately logs nothing. The dot states in §7 distinguish *decided against* from *never answered*; closing a card is neither, so it must not consume the slot.

**One edge to be aware of.** A snoozed slot that passes unanswered still derives as a miss, because `derive` only sees the *current* `snoozed_until`, not that the slot was snoozed at the time. With the shortest cadence at 40 minutes a 15-minute delay cannot span a slot boundary, so this is unreachable as configured. It becomes reachable if §11's settings screen lets a cadence go below ~15 minutes — at which point the fix is to log the snooze as an event carrying the slot it covered, not to special-case `derive`. Floor the configurable cadence at 20 minutes.

---

## 7. Board UI

368x448 AMOLED, LVGL. A 368x368 square carries the grid; the remaining 80px is a header with the clock, the date and a battery pip.

### 7.1 Grid (default)

- 7 tiles, 2 columns, `shutdown` spanning the full final row — it is the one action that ends the day, so the layout reads as a full stop
- Each tile: icon, name, progress dots, countdown in minutes
- **Countdown** renders as a slow tinted wash rising from the bottom of the tile as the slot approaches
- **Grain** overlay, drifting in 6 discrete steps so it reads as film grain rather than a sliding gradient

The wash is the countdown, and on LVGL it is a gradient-filled object whose height tracks the fraction of the interval elapsed — updated once per second from the scheduler tick, which is well within budget at seven tiles.

```c
static void tile_set_fill(tile_t *t, float frac) {          // frac 0 → 1
    lv_obj_set_height(t->wash, (lv_coord_t)(t->h * frac));
    lv_obj_align(t->wash, LV_ALIGN_BOTTOM_MID, 0, 0);
}
```

Tapping any tile logs that action immediately with no prompt — the "I just drank a glass" path, through the same `on_done` as §5.3.

### 7.2 Dot states

| State | Appearance | Meaning |
|---|---|---|
| Done | filled, action tint | logged |
| Skipped | mid-grey solid | deliberately skipped |
| Missed | hollow ring | slot passed with no response |
| Pending | dark solid | still to come |

Skipped and missed are visually distinct because deciding not to eat lunch and forgetting to log lunch are different facts about the day.

Dots render in a fixed order — done, skipped, missed, then pending padding — so a tile's dots never reshuffle as the day fills in. An overshoot (more done than the target) grows the row rather than truncating.

### 7.3 Prompt screen

Tinted to the action. Large icon, name, one line of context, three buttons plus the X. Buttons sized for a thumb on a 368px panel, not a stylus.

```c
static void build_prompt(action_id_t id) {
    const action_def_t *def = &ACTIONS[id];

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(def->tint_dim), 0);

    lv_obj_t *done = lv_btn_create(scr);
    lv_obj_set_size(done, 300, 96);
    lv_obj_align(done, LV_ALIGN_CENTER, 0, 40);
    lv_obj_set_style_bg_color(done, lv_color_hex(def->tint), 0);
    lv_obj_add_event_cb(done, on_done_cb, LV_EVENT_CLICKED, (void *)(intptr_t)id);
    /* … skip, delay-all, X … */

    lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
}

static void on_done_cb(lv_event_t *e) {
    action_id_t id = (action_id_t)(intptr_t)lv_event_get_user_data(e);
    on_done(id);                    // → day_apply_event → persist → ws_broadcast
    haptic_pulse(25);
    audio_cue(CUE_SUCCESS);
    queue_advance();                // next card, or back to the grid
    idle_timer_reset();             // screen goes dark 20s from now
}
```

### 7.4 Inputs

Four ways to answer a prompt, and **all four end at the same two functions**. Nothing else in the firmware writes an event.

```
touch: Done button   ─┐
touch: grid tile     ─┤
physical key         ─┼──► input_done(action)  ──► day_apply_event()
IMU tap (optional)   ─┘                              ▲
                                                     │
web app: POST /event ────────────────────────────────┘
```

```c
// firmware/main/input.c — every input path lands here

void input_done(action_id_t id) {
    log_event_t ev = {
        .action = id,
        .kind   = KIND_DONE,
        .ts     = time(NULL),                  // RTC-backed; right after a reboot
        .slot   = current_or_next_slot(id),    // which slot this answers
        .source = SOURCE_BOARD,
    };
    uuid_v4(ev.id);

    if (day_apply_event(&ev)) {                // false = we already had it
        feedback_play(CUE_SUCCESS);            // sound + haptic, §9
    }
    queue_advance();                           // next card, or back to the grid
    idle_timer_reset();                        // screen goes dark 20s from now
}

void input_skip(action_id_t id) {
    log_event_t ev = { .action = id, .kind = KIND_SKIP, .ts = time(NULL),
                       .slot = current_or_next_slot(id), .source = SOURCE_BOARD };
    uuid_v4(ev.id);
    day_apply_event(&ev);
    feedback_play(CUE_READY);
    queue_advance();
    idle_timer_reset();
}
```

Gating the cue on `day_apply_event`'s return is a small thing that matters in use: a double-tap on Done should chime once, not twice. The second tap is deduplicated by the `UNIQUE` constraint, and the silence tells the user it did not count twice.

**Touch — prompt buttons.** LVGL callbacks are one line each.

```c
static void on_done_cb(lv_event_t *e)  { input_done((action_id_t)(intptr_t)lv_event_get_user_data(e)); }
static void on_skip_cb(lv_event_t *e)  { input_skip((action_id_t)(intptr_t)lv_event_get_user_data(e)); }
static void on_delay_cb(lv_event_t *e) { queue_delay_all(time(NULL)); feedback_play(CUE_READY); }
static void on_close_cb(lv_event_t *e) { queue_dismiss(); }
```

**Touch — grid tiles.** Tapping a tile logs that action with no prompt, which is the "I just drank a glass, credit me" path. Same function, so it re-anchors the timer exactly as answering a prompt would.

```c
static void on_tile_cb(lv_event_t *e) {
    action_id_t id = (action_id_t)(intptr_t)lv_event_get_user_data(e);
    if (screen_is_dark()) { wake_screen(); return; }    // first touch only wakes
    input_done(id);
    tile_flash(id);                                      // brief tint pulse
}
```

**Physical key.** One press = Done on the current prompt, which is what you want when the prompt is "stand up" and you are already standing — you can answer it without looking.

```c
// firmware/main/key.c
#define KEY_GPIO        GPIO_NUM_0     // verify against your board revision
#define DEBOUNCE_US     250000         // 250ms
#define LONG_PRESS_US   800000         // 800ms

static void key_task(void *arg) {
    int64_t pressed_at = 0;
    bool    was_down   = false;

    for (;;) {
        bool down = gpio_get_level(KEY_GPIO) == 0;      // active low — verify
        int64_t now = esp_timer_get_time();

        if (down && !was_down) {                        // edge: press
            pressed_at = now;
        } else if (!down && was_down) {                 // edge: release
            int64_t held = now - pressed_at;
            if (held > DEBOUNCE_US) {
                if (screen_is_dark()) {
                    wake_screen();                      // first press only wakes
                } else if (held > LONG_PRESS_US) {
                    queue_delay_all(time(NULL));        // hold = delay all 15
                    feedback_play(CUE_READY);
                } else if (g_queue_len > 0) {
                    input_done(g_queue[0]);             // tap = done
                }
            }
        }
        was_down = down;
        vTaskDelay(pdMS_TO_TICKS(20));                  // 50Hz poll is plenty
    }
}
```

The wake-then-answer split matters: a single press that both lights the panel and logs a completion means every accidental brush marks water as drunk. First press wakes, second press commits. Acting on *release* rather than press is what makes the long-press variant possible without a second button.

**Check the touch controller part and the key GPIO against the Waveshare schematic for your board revision** rather than trusting the numbers here — the AMOLED boards have changed touch parts between revisions, and the BOOT key is shared with strapping on some layouts, which will bite you at flash time.

**IMU tap (optional, default off).** The QMI8658 has tap detection, so a knock on the desk beside the board could answer the current prompt without reaching for it — genuinely nice for "stand break", since you are already moving.

```c
static void imu_tap_isr(void *arg) {
    // Only ever answers a prompt that is already on screen. A tap must never
    // be able to log something the user was not being asked about — the
    // false-positive rate on desk knocks is far too high for that.
    if (g_queue_len == 0 || !g_settings.imu_tap) return;
    xQueueSendFromISR(g_input_q, &(input_msg_t){ .kind = INPUT_TAP }, NULL);
}
```

Prototype it behind the setting and keep the guard: an IMU tap can only answer, never originate.

### 7.5 Stretch flow

Replaces the prompt screen. One stretch per screen with a countdown ring, auto-advancing on completion, Next to advance early, Skip to abandon the set.

Derive the remaining time from an absolute end timestamp rather than decrementing a counter, so a delayed render resumes at the correct point instead of stretching a 40-second hold into a minute.

Abandoning the set part-way logs a `skip`, not a partial `done` — a half-finished stretch set is a skipped stretch set, and the dot should say so.

---

## 8. Web app

A viewer and a remote. It renders `DayView` from the board's `DayLog` using the same `derive`, and it can log a completion. **It has no scheduler and raises no prompts.**

### 8.1 The protocol

Three endpoints and one socket. Everything is JSON; nothing is versioned beyond the `version` field in the payload, because both ends ship together.

| | | |
|---|---|---|
| `GET /state` | → `{ log, view, power }` | full snapshot, used once on load |
| `POST /event` | `{ action, kind, ts, slot }` → `{ ok, applied }` | log a completion |
| `GET /ws` | ← `{ type: 'state', … }` | push on every change |
| `GET /` | → the web app itself | static files from LittleFS |

**The snapshot.** One shape, sent by both `GET /state` and every WebSocket push, so the client has exactly one code path for "here is the world".

```jsonc
{
  "type": "state",
  "log":  { "version": 2, "date": "2026-08-14", "events": [ … ], "awake": [ … ] },
  "view": {                                  // derived on the board, §3.3
    "counts":  { "water": 3, "stand": 4 },
    "skipped": { "stand": 1 },
    "missed":  { "water": 2 },
    "next":    { "water": 1723648800 },
    "due":     ["water"]
  },
  "power": { "battPct": 82, "charging": true, "onBattery": false },
  "now": 1723645200                          // board clock, for countdown skew
}
```

`now` matters more than it looks: the browser renders countdowns from `view.next`, and if the laptop's clock is two minutes off the board's, every tile shows the wrong number. The client stores `skew = now_board - now_browser` once per snapshot and applies it to every countdown.

### 8.2 Board side

```c
// firmware/main/api.c
static httpd_handle_t g_server;

// ── GET /state ───────────────────────────────────────────────────────────
static esp_err_t state_get(httpd_req_t *req) {
    char *json = snapshot_json();          // log + view + power + now
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

// ── POST /event ──────────────────────────────────────────────────────────
static esp_err_t event_post(httpd_req_t *req) {
    char buf[256];
    int n = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf) - 1));
    if (n <= 0) return ESP_FAIL;
    buf[n] = '\0';

    log_event_t ev;
    if (parse_event(buf, &ev) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad event");
        return ESP_OK;
    }
    ev.source = SOURCE_WEB;

    bool applied = day_apply_event(&ev);   // same function the touch handler calls

    // `applied: false` means we already had this event — a retry, or the user
    // tapped both the board and their laptop. Still a 200: the client asked
    // for the event to exist, and it does.
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"applied\":%s}", applied ? "true" : "false");
    return httpd_resp_sendstr(req, resp);
}
```

**The WebSocket.** ESP-IDF's `esp_http_server` handles the upgrade; we keep a small table of connected sockets and write the snapshot to each on change.

```c
// ── GET /ws ──────────────────────────────────────────────────────────────
#define WS_MAX_CLIENTS 4
static int g_ws_fds[WS_MAX_CLIENTS];

static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {         // the handshake — no payload yet
        int fd = httpd_req_to_sockfd(req);
        ws_client_add(fd);
        ESP_LOGI(TAG, "ws client %d connected", fd);
        ws_send_snapshot(fd);              // send current state immediately
        return ESP_OK;
    }
    return ESP_OK;                          // we never expect inbound frames
}

static void ws_client_add(int fd) {
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (g_ws_fds[i] == fd || g_ws_fds[i] == 0) { g_ws_fds[i] = fd; return; }
    }
    ESP_LOGW(TAG, "ws client table full, dropping %d", fd);
}

static void ws_send_snapshot(int fd) {
    char *json = snapshot_json();
    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json,
        .len = strlen(json),
    };
    // Queue rather than send inline: this is called from the scheduler and
    // the touch handler, and neither should block on a slow client.
    if (httpd_ws_send_frame_async(g_server, fd, &frame) != ESP_OK) {
        ws_client_remove(fd);              // client went away
    }
    free(json);
}

// Called by day_apply_event (§4) and by the power-event handler (§10).
void ws_broadcast_state(void) {
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (g_ws_fds[i]) ws_send_snapshot(g_ws_fds[i]);
    }
}
```

`httpd_ws_send_frame_async` rather than the blocking form is the important detail. `ws_broadcast_state` is called from inside `day_apply_event`, which runs on the LVGL task in response to a touch — a blocking write to a stalled client would freeze the UI mid-tap.

**Registration and discovery:**

```c
void api_start(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = WS_MAX_CLIENTS + 3;   // sockets + room for plain GETs
    ESP_ERROR_CHECK(httpd_start(&g_server, &cfg));

    httpd_uri_t routes[] = {
        { .uri = "/state", .method = HTTP_GET,  .handler = state_get  },
        { .uri = "/event", .method = HTTP_POST, .handler = event_post },
        { .uri = "/ws",    .method = HTTP_GET,  .handler = ws_handler, .is_websocket = true },
        { .uri = "/*",     .method = HTTP_GET,  .handler = static_get  },  // the web app
    };
    for (int i = 0; i < 4; i++) httpd_register_uri_handler(g_server, &routes[i]);

    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("wfh"));            // → wfh.local
    ESP_ERROR_CHECK(mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0));
}
```

**Serve the web app from the board.** `http://wfh.local` talking to `http://wfh.local` sidesteps the mixed-content block that would stop an HTTPS-hosted page from reaching the board over plain HTTP. The built bundle goes on the LittleFS partition and is served by `static_get`; at a few hundred KB it fits comfortably in 16MB alongside the firmware and the database.

### 8.3 Web app side

One class. It owns the socket, the reconnect, the clock skew and the outbox, and it hands the rest of the app a plain snapshot.

```ts
// web/src/board.ts
type Snapshot = { log: DayLog; view: DayView; power: Power; now: number };

export class Board {
  private ws?: WebSocket;
  private backoff = 1000;                       // grows to 30s, resets on connect
  private outbox: LogEvent[] = [];
  skew = 0;                                     // board clock − browser clock
  online = false;

  constructor(private host = location.host, private onSnapshot: (s: Snapshot) => void) {}

  // ── connect ────────────────────────────────────────────────────────────
  start() {
    this.ws = new WebSocket(`ws://${this.host}/ws`);

    this.ws.onopen = () => {
      this.online = true;
      this.backoff = 1000;
      this.flushOutbox();                       // replay anything logged offline
    };

    this.ws.onmessage = e => this.receive(JSON.parse(e.data));

    this.ws.onclose = () => {
      this.online = false;
      this.onSnapshot(this.last!);              // re-render with the offline flag
      setTimeout(() => this.start(), this.backoff);
      this.backoff = Math.min(this.backoff * 2, 30_000);
    };

    // onerror always precedes onclose; let onclose own the reconnect so we
    // never schedule two retries for one failure.
    this.ws.onerror = () => this.ws?.close();
  }

  private receive(s: Snapshot) {
    this.skew = s.now - Math.floor(Date.now() / 1000);
    this.last = s;
    this.onSnapshot(s);
  }

  // ── log a completion ───────────────────────────────────────────────────
  async log(action: ActionId, kind: EventKind, slot: number) {
    const ev: LogEvent = {
      id: crypto.randomUUID(), action, kind, slot,
      ts: this.boardNow(), source: 'web',
    };

    // Optimistic: bump the tile now, so the tap feels instant. The board's
    // push will overwrite this within a few milliseconds if it lands.
    this.onSnapshot(applyOptimistic(this.last!, ev));

    try {
      const r = await fetch(`http://${this.host}/event`, {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify(ev),
      });
      if (!r.ok) throw new Error(String(r.status));
    } catch {
      this.outbox.push(ev);                     // retried on next connect
    }
  }

  private async flushOutbox() {
    for (const ev of [...this.outbox]) {
      try {
        await fetch(`http://${this.host}/event`, {
          method: 'POST',
          headers: { 'content-type': 'application/json' },
          body: JSON.stringify(ev),
        });
        this.outbox.splice(this.outbox.indexOf(ev), 1);
      } catch { return; }                       // still down; try again later
    }
  }

  /** The board's clock, not the browser's. Countdowns must agree with the tile. */
  boardNow(): number { return Math.floor(Date.now() / 1000) + this.skew; }
}
```

The optimistic update is what makes a tap on the laptop feel like a tap on the board. It is safe to be wrong: the authoritative snapshot arrives moments later and replaces it wholesale, and if the request failed the outbox retries it. Nothing needs to be rolled back by hand.

The UI layer — grid, tiles, wash, grain, dots — is the reviewed prototype, unchanged. It reads `DayView` and does not care where the log came from.

```css
.frame {
  aspect-ratio: 1 / 1;
  max-width: 540px;
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 10px; padding: 10px;
  background: #0d1014;
  border-radius: 20px;
  isolation: isolate;          /* keeps the frame grain off any overlay */
}

.tile { position: relative; overflow: hidden; border-radius: 14px;
        background: #151a21; --tint: #7fd4a8; --fill: 0; }

.tile::before {                /* the rising wash = the countdown */
  content: ''; position: absolute; inset: 0;
  background: linear-gradient(to top,
              color-mix(in oklab, var(--tint) 26%, transparent), transparent);
  transform: scaleY(var(--fill)); transform-origin: bottom;
  transition: transform 1.2s linear; pointer-events: none;
}

@keyframes grain-drift {       /* 6 discrete steps — grain, not a sliding sheet */
  0%,15%{transform:translate(0,0)}      16%,31%{transform:translate(-2%,1%)}
  32%,47%{transform:translate(1%,-2%)}  48%,63%{transform:translate(-1%,-1%)}
  64%,79%{transform:translate(2%,1%)}   80%,100%{transform:translate(0,2%)}
}
.grain { position:absolute; inset:-6%; background-image: var(--grain-svg);
         opacity:.06; mix-blend-mode:overlay; pointer-events:none;
         animation: grain-drift 1.6s steps(1) infinite; }

@media (prefers-reduced-motion: reduce) {
  .grain { animation: none; }
  .tile::before { transition: none; }
}
```

`color-mix` in oklab keeps the seven tints at even perceptual weight; mixing in sRGB would make the yellow and amber tiles read considerably hotter than the violet at the same percentage. The grain texture is one inline `feTurbulence` SVG data URI defined on `:root` and shared by every tile — one decoded bitmap in the compositor rather than seven.

### 8.4 When the board is away

Asleep, off the LAN, or flat — all indistinguishable from a closed socket, and the client does not need to tell them apart. `Board` keeps the last snapshot, keeps accepting taps into the outbox, and reconnects with backoff. The UI shows a plain marker and stops the countdowns, which would otherwise tick down to zero and lie.

```tsx
{!board.online && <div className="banner">Board offline — taps will sync when it returns</div>}
```

**The web app does not prompt in this state.** No sound, no cards, no timers. Filling the gap with browser-side prompting would resurrect exactly the two-scheduler problem §1 exists to avoid, and would do it at the worst possible moment: when the board comes back and both start chiming.

---

## 9. Feedback

Three cues, matching the reviewed prototype's vocabulary. Both channels fire together and both are driven from one table, so a cue is defined in exactly one place.

| Event | Sound | Haptic |
|---|---|---|
| Prompt appears | `bloom` — rising two-note | triple pulse |
| Action logged | `success` — quick up-tick | short tick |
| Stretch step complete | `ready` — single soft note | light tick |

```c
// firmware/main/feedback.h
typedef enum { CUE_BLOOM, CUE_SUCCESS, CUE_READY, CUE_COUNT } cue_t;

void feedback_play(cue_t cue);      // sound + haptic together, non-blocking
```

```c
// firmware/main/feedback.c — the one table
typedef struct {
    const tone_t   *tones;   int n_tones;
    const uint16_t *buzz;    int n_buzz;    // alternating on/off ms
} cue_def_t;

static const tone_t BLOOM[]   = { {523, 90}, {659, 140} };        // C5 → E5
static const tone_t SUCCESS[] = { {659, 70}, {880, 110} };        // E5 → A5
static const tone_t READY[]   = { {440, 120} };                   // A4

static const uint16_t BUZZ_PROMPT[]  = { 40, 60, 40, 60, 40 };
static const uint16_t BUZZ_DONE[]    = { 25 };
static const uint16_t BUZZ_LIGHT[]   = { 12 };

static const cue_def_t CUES[CUE_COUNT] = {
    [CUE_BLOOM]   = { BLOOM,   2, BUZZ_PROMPT, 5 },
    [CUE_SUCCESS] = { SUCCESS, 2, BUZZ_DONE,   1 },
    [CUE_READY]   = { READY,   1, BUZZ_LIGHT,  1 },
};

void feedback_play(cue_t cue) {
    if (g_settings.sound)   audio_play(CUES[cue].tones, CUES[cue].n_tones);
    if (g_settings.haptics) haptic_play(CUES[cue].buzz, CUES[cue].n_buzz);
}
```

### 9.1 Sound

The speaker is driven over I2S. Rather than shipping audio files, synthesise the tones — a sine with a short attack and an exponential decay, which is all these cues are.

**Check which amplifier and pin mapping your board revision uses** before wiring this up; the ESP32-S3 AMOLED boards have shipped with more than one audio arrangement.

```c
// firmware/main/audio.c
#define SAMPLE_RATE 16000

typedef struct { uint16_t freq_hz; uint16_t ms; } tone_t;

/** Render one tone into `buf` and write it to I2S. Blocking, runs on the
 *  audio task only — never call this from the LVGL or scheduler tasks. */
static void render_tone(const tone_t *t) {
    const int n = (SAMPLE_RATE * t->ms) / 1000;
    static int16_t buf[SAMPLE_RATE / 4];              // 250ms max per tone
    const float step = 2.0f * M_PI * t->freq_hz / SAMPLE_RATE;

    for (int i = 0; i < n; i++) {
        // Envelope: 5ms attack so it doesn't click, then exponential decay.
        // Without the attack ramp a square-edged start pops audibly on a
        // small speaker, which is exactly the wrong texture for a health nudge.
        float attack = fminf(1.0f, i / (0.005f * SAMPLE_RATE));
        float decay  = expf(-3.0f * i / n);
        float env    = attack * decay * (g_settings.volume / 100.0f);

        buf[i] = (int16_t)(sinf(step * i) * env * 12000);
    }

    size_t written;
    i2s_channel_write(g_i2s, buf, n * sizeof(int16_t), &written, portMAX_DELAY);
}

/** Queue a cue. Returns immediately — the audio task does the work. */
void audio_play(const tone_t *tones, int n) {
    audio_msg_t msg = { .n = n };
    memcpy(msg.tones, tones, n * sizeof(tone_t));
    xQueueSend(g_audio_q, &msg, 0);      // drop if the queue is full, don't block
}

static void audio_task(void *arg) {
    audio_msg_t msg;
    for (;;) {
        if (xQueueReceive(g_audio_q, &msg, portMAX_DELAY)) {
            for (int i = 0; i < msg.n; i++) render_tone(&msg.tones[i]);
        }
    }
}
```

Two things worth keeping: the queue means `feedback_play` never blocks a touch handler, and `xQueueSend` with a zero timeout means a burst of cues drops rather than backing up — three prompts arriving at once should not produce eight seconds of queued beeping.

*Note:* `bloom` is soft and may not cut through when heads-down. Test against a brighter `chime` before locking it in, **and test it on the board's actual speaker**, which is small and will not reproduce the low end the way a laptop does. The cue that works in a browser prototype is not necessarily the cue that works here.

**Escalate rather than repeat.** If a prompt goes unanswered for two minutes, re-cue once at higher volume. If it is still unanswered when the next slot arrives it becomes a derived miss and the board goes quiet. It must never nag in a loop.

```c
// In the scheduler tick.
if (g_queue_len > 0 && now - g_prompt_raised_at == 120) {
    audio_set_volume(g_settings.volume + 15);
    feedback_play(CUE_BLOOM);
    audio_set_volume(g_settings.volume);
}
```

### 9.2 Haptics

An ERM or LRA motor on a PWM (LEDC) pin through a small driver transistor. **The board does not have one fitted** — this is an added component on a spare GPIO, so budget for it in the enclosure.

```c
// firmware/main/haptic.c
#define HAPTIC_GPIO      GPIO_NUM_14        // verify against your wiring
#define HAPTIC_DUTY      180                // of 255; full duty is loud and buzzy

void haptic_init(void) {
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = 200,                     // ERM motors like a few hundred Hz
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t ch = {
        .gpio_num = HAPTIC_GPIO, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1, .timer_sel = LEDC_TIMER_1, .duty = 0,
    };
    ledc_channel_config(&ch);

    g_haptic_q = xQueueCreate(4, sizeof(haptic_msg_t));
    xTaskCreate(haptic_task, "haptic", 2048, NULL, 5, NULL);
}

static void motor(bool on) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, on ? HAPTIC_DUTY : 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

/** Pattern is alternating on/off durations in ms, starting with on. */
static void haptic_task(void *arg) {
    haptic_msg_t msg;
    for (;;) {
        if (xQueueReceive(g_haptic_q, &msg, portMAX_DELAY)) {
            for (int i = 0; i < msg.n; i++) {
                motor(i % 2 == 0);                        // even = on, odd = off
                vTaskDelay(pdMS_TO_TICKS(msg.pattern[i]));
            }
            motor(false);      // always leave it off, even if we were interrupted
        }
    }
}

void haptic_play(const uint16_t *pattern, int n) {
    haptic_msg_t msg = { .n = n };
    memcpy(msg.pattern, pattern, n * sizeof(uint16_t));
    xQueueSend(g_haptic_q, &msg, 0);
}
```

Same shape as audio — its own task, its own queue, non-blocking send. The unconditional `motor(false)` at the end of each pattern is the line that stops a crash or a reset mid-pattern from leaving the motor running.

### 9.3 In the web app

Off by default. The board is making the noise, and a laptop echoing every cue a second time is worse than silence.

If enabled, `cuelume` (MIT, ESM, ~5kB, synthesised live, no audio files) driven from the WebSocket push — the same three cues, so the vocabulary stays consistent wherever you hear it. Resume the `AudioContext` on the first gesture or the first cue of the session is silent:

```ts
const unlock = () => { ctx?.resume(); document.removeEventListener('pointerdown', unlock); };
document.addEventListener('pointerdown', unlock, { once: true });
```

No browser haptics: `navigator.vibrate` is unsupported in iOS Safari. Feature-detect and hide the toggle rather than showing a switch that does nothing — and it costs nothing now that the board carries the haptics.

---

## 10. Power

The board is **not** always on. On USB-C it effectively is; on battery it is not, and the design has to say what happens instead.

**Rough numbers.** The 1.8" AMOLED dominates — call it 80–150mA lit depending on how much of the panel is emitting, against maybe 40–50mA for the ESP32-S3 with WiFi associated and the screen off. A 500mAh-class cell is therefore a handful of hours screen-on and most of a day screen-off. Measure at the PMIC rather than trusting this paragraph.

So **screen-on time is the budget, not uptime** — which suits an app that is idle 99% of the time: seven prompts an hour at most, each needing a few seconds of attention.

| State | Trigger | Screen | WiFi | Serves HTTP |
|---|---|---|---|---|
| Active | prompt firing, or touch within 20s | on | on | yes |
| Idle | no touch for 20s, inside working hours | off | modem-sleep (DTIM) | yes |
| Dormant | outside working hours | off | off | no |

**Idle is the important one.** Screen off with WiFi in modem-sleep keeps the TCP stack alive, so `/state` and `/ws` keep working and the web app never notices, while the panel — the actual cost — is dark.

```c
esp_pm_config_t pm = {
    .max_freq_mhz = 240,
    .min_freq_mhz = 40,
    .light_sleep_enable = true,       // wakes on WiFi, timer, or GPIO
};
ESP_ERROR_CHECK(esp_pm_configure(&pm));
esp_wifi_set_ps(WIFI_PS_MAX_MODEM);   // wake only on DTIM beacons
```

**Deep sleep is the wrong tool for Idle**, and it is worth stating so nobody reaches for it: it drops the network stack, so the board vanishes from the LAN and the web app's polls fail. Use it only for Dormant, where nothing is scheduled and being unreachable costs nothing.

```c
static void enter_dormant(time_t work_start_tomorrow) {
    uint64_t us = (uint64_t)(work_start_tomorrow - time(NULL)) * 1000000ULL;
    esp_sleep_enable_timer_wakeup(us);
    esp_sleep_enable_ext1_wakeup(BIT64(KEY_GPIO), ESP_EXT1_WAKEUP_ANY_LOW);
    store_close_awake(time(NULL));                // seal the window, §3.4
    sqlite3_close(g_db);                          // checkpoint WAL before sleep
    esp_deep_sleep_start();
}
```

**Running flat mid-day is a non-event.** This is the payoff from §3. The board dies at 14:00, gets plugged in at 15:30, and boots with the database intact on flash and the correct wall-clock time from the PCF85063. It opens a new awake window at 15:30, and the slots between 14:00 and 15:30 are outside every window, so they are *not* misses. The user is not blamed for a dead battery.

```c
static void on_power_event(axp2101_event_t ev) {
    switch (ev) {
    case AXP2101_VBUS_REMOVED:
        g_on_battery = true;
        ui_set_brightness(BRIGHTNESS_BATTERY);    // ~40% — biggest single saving
        idle_timeout_set(20 * 1000);
        break;
    case AXP2101_VBUS_INSERTED:
        g_on_battery = false;
        ui_set_brightness(BRIGHTNESS_MAINS);
        idle_timeout_set(60 * 1000);
        break;
    case AXP2101_BATT_LOW:                        // ~15%
        store_close_awake(time(NULL));             // seal while we still can
        ui_show_battery_warning();                 // on the board, not the phone
        break;
    case AXP2101_BATT_CRITICAL:                   // ~5%
        store_close_awake(time(NULL));
        sqlite3_close(g_db);
        enter_dormant(0);                         // wake on USB only
        break;
    }
}
```

`store_close_awake` on the low-battery interrupt is the line that matters for correctness: it seals the window while there is still power to commit, so the database honestly records when the board stopped being able to prompt. Closing the connection on the critical interrupt checkpoints the WAL, so the next boot opens a clean database rather than replaying a journal.

Expose battery in `/state` so the web app can show it and the user is never guessing:

```c
cJSON_AddNumberToObject(root, "battPct",   axp2101_get_batt_percent());
cJSON_AddBoolToObject(root,   "charging",  axp2101_is_charging());
cJSON_AddBoolToObject(root,   "onBattery", g_on_battery);
```

**Recommendation: run it on USB-C.** It sits on a desk next to a laptop; a permanently-connected cable is not a hardship and it removes the question entirely. The battery then covers a power cut and the occasional unplugging rather than being the operating mode — and the design above means neither corrupts the day's record.

---

## 11. Build order

Firmware first, because it is the product and it is the long pole. Each step should end somewhere you can leave it.

1. **Board bring-up** — ESP-IDF project, display, touch, LVGL hello-world, WiFi, NTP-set RTC. Confirms the hardware and the toolchain before any product logic exists.
2. **Config generation** — `actions.json` → `actions.g.h` / `actions.g.ts`, wired into the build with the CI diff check.
3. **Storage** — LittleFS partition, SQLite, schema, `store_add_event` / `store_load_day` / `store_mark_awake`. Verifiable on its own with a serial console before any UI exists.
4. **`derive` and `day_apply_event`** — the walk, the anchor rule, the awake test, plus the fixtures in §11.1. **This is the step to get right**; everything else is presentation.
5. **Scheduler tick** — awake windows, jittered offsets, due detection. Testable on-desk by moving the RTC forward.
6. **Grid UI** — tiles, dots, wash, header. First point at which the thing looks like itself.
7. **Inputs** — prompt screens, Done / Skip / Delay-all / X, tile-tap logging, physical key (§7.4).
8. **Sound and haptics** — the cue table, the I2S tone task, the motor task, escalation (§9).
9. **Guided stretch flow.**
10. **HTTP server** — `/state`, `/event`, `/ws`, mDNS (§8). The board is complete and usable on its own at this point.
11. **Web app** — the reviewed grid UI, the `Board` client, served from LittleFS.
12. **Power states** — light sleep, brightness, PMIC events, Dormant.
13. **Settings screen** — working hours, per-action cadence, sound, haptics, IMU tap.

Steps 1–10 are a finished product. Everything after is reach.

### 11.1 Test targets

Step 4 produces the bugs nobody notices for a week, and `derive` is a pure function of `(log, now, settings)` — so it compiles and runs on a laptop with no board attached. Build a host target for it early; the flash-and-test loop is far too slow to iterate a slot-walk bug on hardware.

```
firmware/test/           # host build: gcc, no ESP-IDF, runs in milliseconds
  test_derive.c          # loads every fixture below and asserts the view
fixtures/derive/*.json   # the cases
```

```
fixtures/derive/
  interval-done-early.json          re-anchors the next slot from the tap
  fixed-done-early.json             13:00 lunch stays 13:00
  missed-while-awake.json           unanswered + inside a window = missed
  missed-while-asleep.json          unanswered + gap = NOT missed
  awake-gap-boundary.json           119s extends, 121s opens a new window
  duplicate-event.json              second apply is a no-op
  replay-idempotent.json            applying the same event twice changes nothing
  stretch-priority.json             stretches take the queue head
  dst-forward.json                  no slots lost or doubled on the changeover
```

The last three are the ones worth having. The awake-gap boundary is what separates "you missed this" from "the board was off", idempotence under replay is what makes the web app's retries safe, and DST is the bug that will otherwise appear twice a year and be impossible to reproduce.

### 11.2 Day rollover

Handled in the tick (§5.2), not on boot: the board runs for weeks at a time, so midnight is the common case rather than an edge case. There is no "reset the timers" step — `next` derives from an empty log against today's date, so a fresh day is literally an empty struct.

---

## 12. Known constraints

- **Single point of failure.** The board is the product. If it is off the LAN, the web app is a read-only cache; if it is dead, there is no tracker. Accepted deliberately — the alternative is two schedulers.
- **Firmware dev loop.** Flash-and-test is slower than a browser reload. §3.3's fixtures and a host-compiled unit test target for `derive` and the slot maths take most of the sting out; build those early.
- **LAN-only, unauthenticated.** Anyone on the same network can read `/state` and post to `/event`. Fine for a home LAN; not fine on a shared or office network. If that changes, put a shared secret in a header before exposing it further.
- **No cross-device sync beyond the board.** The board is the only writer of record; two browsers reconcile through it, not with each other.
- **No reach away from the desk.** Deliberate. If you are not at the board, it does not prompt you, and it does not tell your phone.
- **Clock.** NTP at boot when WiFi is available, PCF85063 otherwise. A board that has never seen NTP and has a flat backup cell will have a wrong date, and the day key will be wrong with it. Show the date in the header so this is visible rather than silent.

What is explicitly *not* a constraint, thanks to §3: a board that slept, ran flat, or was unplugged does not accumulate misses. It can be switched off freely without the day's record becoming a wall of hollow rings.

---

## 13. What SQLite buys

The schema and the storage code are in §3.5. This section is why, and where the line is.

### 13.1 It removes code rather than adding it

The model in §3.1 — an append-only log of uniquely-keyed rows plus a table of intervals — *is* relational. Storing it as packed blobs meant hand-writing the things a database already does. Three examples from earlier drafts of this plan, all now deleted:

| Was | Is |
|---|---|
| A linear scan over the events array to reject duplicates | `UNIQUE (action, slot)` + `INSERT OR IGNORE` |
| `EVENTS_MAX` / `AWAKE_MAX` caps and overflow handling | rows |
| A key-scanning prune of old day blobs | one `DELETE ... WHERE day < date(...)` |

The dedupe case is the one that matters most. It was the load-bearing invariant of the whole design — the thing that makes retries, double-taps and two clients safe — and it was enforced by a loop that a future edit could quietly break. As a constraint, the database refuses the duplicate no matter which code path reached it, including paths nobody has written yet.

WAL mode is the other quiet win: a power cut mid-write on a battery-powered device leaves the last committed transaction intact rather than a half-written blob.

### 13.2 Where the line is

**`derive` stays in C. Do not move it into SQL.**

Counts and skips would translate fine — one `GROUP BY`. But `missed` and `next` need the slot walk, and §5.3's early-completion rule means slot *n+1* depends on the event that answered slot *n*. In SQL that is a recursive CTE joining each generated slot back against `events`, parameterised per action because the cadence lives in config:

```sql
-- What NOT to write. Correct, and far harder to read than the C it replaces.
WITH RECURSIVE slots(slot) AS (
  SELECT :first_slot
  UNION ALL
  SELECT COALESCE((SELECT e.ts FROM events e
                    WHERE e.action = :action AND e.slot = slots.slot AND e.kind = 'done'),
                  slots.slot) + :every_sec
  FROM slots WHERE slot < :work_end
)
SELECT COUNT(*) FROM slots WHERE ...
```

The rule: **the database holds facts, `derive` holds meaning.** Anything involving cadences, slots or working hours is scheduling semantics and belongs in the C. Anything that is counting or filtering rows belongs in SQL.

### 13.3 What it unlocks later

Retention is 400 days (§3.5), so the data for history is accumulating from day one even though v1 renders only today. When historical views arrive they are queries, not a migration:

```sql
-- Water over the last 30 days
SELECT day, COUNT(*) AS n
FROM events
WHERE action = 'water' AND kind = 'done' AND day >= date('now','localtime','-30 days')
GROUP BY day ORDER BY day;

-- Current streak of days where every action hit its target
SELECT COUNT(*) FROM (
  SELECT day FROM events WHERE kind = 'done'
  GROUP BY day HAVING COUNT(DISTINCT action) = 7
  ORDER BY day DESC
);

-- Which action gets skipped most, and at what time of day
SELECT action, strftime('%H', ts, 'unixepoch', 'localtime') AS hour, COUNT(*) AS n
FROM events WHERE kind = 'skip'
GROUP BY action, hour ORDER BY n DESC;
```

That last one is the interesting one, and it is the argument for keeping the data: "you skip your 15:30 snack four days in five" is a fact about the schedule being wrong, not about the person. A tracker that can notice that is worth more than one that only counts.

Serve it as `GET /history?from=&to=` when the time comes — a query on the board beats shipping a year of rows to the browser to reduce client-side.
