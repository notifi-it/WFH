# WFH Health Tracker — Implementation Plan

Status: UX prototype built and reviewed. This document is the spec for the real build.

---

## 1. Scope

A square-format habit tracker that prompts seven actions on independent timers during working hours, chimes and vibrates, and logs one tap per completion. Today-at-a-glance only, no historical views in v1.

**One build, one source of truth: the board.**

Hardware: Waveshare ESP32-S3-Touch-AMOLED-1.8 (368x448, ESP32-S3R8, 8MB PSRAM, 16MB flash, QMI8658 IMU, PCF85063 RTC, AXP2101 PMIC, speaker, battery).

- The **board** owns the schedule, the clock, the event log, the prompting and the sound. It is the product.
- The **web app** is a viewer and a remote. It renders the same day from the same log and can log a completion, but it never schedules and never prompts.

This was previously split into a browser-first phase and a hardware phase. That split meant building a browser scheduler with known-unfixable reliability problems — tab suspension chief among them — and then deleting it. The single-phase version skips the throwaway.

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
      "cadence": { "kind": "interval", "everyMin": 40 } },

    { "id": "water", "name": "Water", "blurb": "Glass of water. Refill while you are up.",
      "icon": "drop", "tint": "#6ec3e0", "target": 8, "flow": "tap", "priority": 1,
      "cadence": { "kind": "interval", "everyMin": 45 } },

    { "id": "roll", "name": "Shoulder roll", "blurb": "Ten slow rolls back, then drop the shoulders.",
      "icon": "refresh", "tint": "#b6a3e8", "target": 8, "flow": "tap", "priority": 1,
      "cadence": { "kind": "interval", "everyMin": 60 } },

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

**No jitter.** An earlier draft staggered start offsets (stand at 09:12, water at 09:07, …) specifically to keep the three interval actions from lining up. That was solving the wrong problem: collisions are fine now that the card shows everything due at once as a checklist (§6) rather than answering one at a time. All three actions anchor to `workStart` and simply collide when they collide.

---

## 3. Data model

Shared verbatim between the board and the web app. This is what makes the two interchangeable rather than merely connected.

### 3.1 What is actually true

Two things are facts. Everything else is a view over them.

1. **The schedule** — config, from §2.1. Not per-day data.
2. **The event log** — what the user did. Append-only. `done` and `skip`, nothing else.

**A miss is derived, not stored:** a slot is missed if it is in the past and no `done` or `skip` answers it. No third fact needed, no "was the board even on" tracking.

**This was simplified from an earlier draft that also tracked awake windows** — periods the board was demonstrably running — so a slot that passed while the board was off, asleep, or flat could be told apart from one the board genuinely failed to prompt for. That bought a real property (a board that ran flat for 90 minutes didn't get blamed for 90 minutes of misses) at the cost of real complexity: a second table, a gap-detection heuristic with a tunable constant, and a join in `derive` that had to reason about two kinds of interval simultaneously.

**Removed.** The rule now is the plain one: **unanswered and in the past means missed, full stop.** The tradeoff this accepts, stated plainly rather than buried: a board that was off — asleep, unplugged, dead battery — for any part of the working day will show every slot it missed during that gap as a genuine miss when it comes back, the same as if it had been on and simply not prompted. If that turns out to matter in practice, the fix is to reintroduce presence tracking in one narrow form (§10 already notes the board should mostly stay on USB power, which makes the gap rare); it is not worth the model complexity up front.

```json
{
  "version": 3,
  "date": "2026-08-14",
  "events": [
    { "id": "…", "action": "water", "kind": "done", "ts": 1723645210, "slot": 1723645020, "source": "board" },
    { "id": "…", "action": "stand", "kind": "skip", "ts": 1723646400, "slot": 1723646400, "source": "web" }
  ],
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

/** Everything persisted for one day. Facts only. */
export interface DayLog {
  version: 3;
  date: string;                                   // YYYY-MM-DD, local
  events: LogEvent[];
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

### 3.1a Timestamps: epoch everywhere except display

Stated as a rule rather than left implicit: **every timestamp that is stored, transmitted, or compared is an epoch integer.** `ts`, `slot`, `snoozedUntil` values, the `now` field in the API snapshot — all epoch seconds, UTC-equivalent, no timezone attached because none is needed.

The one exception is `cadence.times` in `actions.json` (`"11:30"`, `"16:30"`, …) — a wall-clock string, because that's the natural way for a human to author a schedule. It is converted to an epoch value immediately by `at_time()` (§5.1) and never travels as a string past that point; nothing downstream of config ever parses a time-of-day string again.

Local wall-clock time reappears exactly once more: at render, when a timestamp becomes "4m" on a tile or "14:32" in the header. That conversion happens in the UI layer only, on both the board and the web app, and is never fed back into the model.

**Why this matters for timezones.** All slot arithmetic happens in the board's own local time (§5.1, via `mktime`/`localtime_r` against a `TZ` set at boot). A browser open from a different timezone doesn't need to agree, because it never computes a slot time itself — it only ever receives an epoch timestamp from the board and formats it for display. The board's clock is authoritative; there is nothing for a client's timezone to get wrong. The one real gap is the board itself: nothing auto-detects a new timezone if the board physically moves (no GPS), so a relocated board needs its `TZ` setting updated by hand.

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
        // Unanswered and in the past. Missed, full stop — see §3.1.
        view.missed[def.id] = (view.missed[def.id] ?? 0) + 1;
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

### 3.4 Persistence — SQLite

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
}
```

**Retention** is one statement on boot instead of a key-scanning prune:

```sql
DELETE FROM events WHERE day < date('now', 'localtime', '-400 days');
```

400 days rather than 14, because the cost is negligible: roughly 30 events/day × ~80 bytes/row (SQLite row overhead plus the text columns) is ~2.4KB/day, so 400 days is under 1MB against 16MB of flash. The number is "basically free," chosen to set up the historical queries in §13 without a later retention change, not because 400 is meaningful in itself.

### 3.5 The web app is a stateless remote

An earlier draft had the web app hold an outbox of taps logged while the board was unreachable, replaying them on reconnect — reconciliation machinery for a client that isn't really a second writer, just a thin remote.

**Removed.** `Board.log()` (§8.3) calls `POST /event` directly and reports success or failure. If the board is unreachable, the tap fails and the UI shows that plainly — no local queue, no retry-on-reconnect logic, nothing held in the browser that outlives the page. This is simpler to reason about and loses little: `UNIQUE (action, slot)` on the board already makes a stray retry harmless, so there was never much for client-side cleverness to protect against.

---

## 4. Firmware architecture

Four FreeRTOS tasks. Everything that mutates the log funnels through one of them.

```
┌─────────────┐  due actions  ┌─────────────┐
│  scheduler  │ ────────────► │     ui      │  LVGL: grid, prompts, stretches
│   1 Hz tick │               │ touch + key │
└──────┬──────┘               └──────┬──────┘
       │                             │ input_done() / input_skip()   §7.4
       │                             ▼
       │                    ┌─────────────────┐      ┌──────────┐
       └───────────────────►│ day_apply_event │─────►│ feedback │ sound
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
    bool is_new = store_add_event(ev);      // INSERT OR IGNORE, §3.4

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

- Each tick asks `derive` what is due.
- **Doing an action early resets its timer from now**, so getting ahead pushes the next slot out rather than being penalised.
- **Missed slots do not stack.** This is not enforced; it is a consequence of `derive` visiting each slot exactly once.
- No jitter. Collisions are shown, not avoided — see §6.

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
            day_init(&g_day, now);
        }

        if (in_working_hours(now, &g_settings)) {
            day_view_t v;
            derive(&g_day, now, &g_settings, &v);
            card_sync(v.due, v.n_due, now);      // §6 — reconciles the checklist card
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```

The tick writes no tallies and rolls nothing forward — `derive` owns all of that. All it does is raise prompts.

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

## 6. The prompt card — a checklist, not a queue

Collisions are common: stand (40m), water (45m) and roll (60m) align exactly every 6 hours and near-align constantly — and with jitter removed (§2.1) they collide even more freely than before, on purpose.

**An earlier draft answered one prompt at a time from a queue** — a card per action, `1 OF n`, Done advancing to the next. That treated a collision as a sequence of interruptions to get through. The revision treats it as what it actually is: several things are true at once, so show them at once.

- **One card, one row per currently-due action** — icon, name, a checkbox. Nothing is hidden behind a "next" tap.
- **Tapping a row toggles its checkbox.** Nothing is logged yet.
- **Confirm** logs `done` for every checked row in one commit, and removes those rows from the card. Unchecked rows stay on the card — they are still due and unanswered, not skipped.
- **Skip**, on each row individually, logs a `skip` for just that action and removes the row. It is deliberately not part of the checkbox/Confirm flow: ticking a box means "I did this," and skip is a separate, smaller decision that should not be batchable by accident.
- **Delay all 15 min** snoozes every action currently on the card — checked or not — and clears it. Absolute, not additive, so mashing the button never pushes anything past now + 15.
- **X** dismisses the card. Nothing is logged. The dot states in §7.2 distinguish *decided against* from *never answered*; closing the card is neither, so it must not consume any slot — including ones that were checked but never confirmed.

Stretches never join the checklist. If a stretch is due alongside anything else, the stretch takes the screen on its own (§7.6) and everything else stays on the card, waiting.

```c
// firmware/main/card.c
// Reconciles the card against `derive`'s due set every tick. Rows are
// additive and sticky: a row that's still due keeps whatever checked
// state the user gave it; a row no longer due (answered elsewhere, e.g.
// a grid-tile tap) simply disappears.
void card_sync(const action_id_t *due, int n_due, time_t now) {
    // Drop rows no longer due.
    for (int i = g_card_len - 1; i >= 0; i--) {
        if (!in_list(due, n_due, g_card[i].action)) card_remove(i);
    }
    // Add newly-due rows, unchecked.
    for (int i = 0; i < n_due; i++) {
        if (!card_contains(due[i])) card_push(due[i], /* checked */ false);
    }

    bool has_stretch = false;
    for (int i = 0; i < g_card_len; i++) has_stretch |= ACTIONS[g_card[i].action].flow == FLOW_STRETCH;
    if (has_stretch) { card_take_stretch(); return; }   // stretch pre-empts, §7.6

    if (g_card_len > 0) ui_show_card();
}

void card_toggle(int row) { g_card[row].checked = !g_card[row].checked; ui_refresh_card(); }

void card_confirm(void) {
    for (int i = g_card_len - 1; i >= 0; i--) {
        if (g_card[i].checked) { input_done(g_card[i].action); card_remove(i); }
    }
    if (g_card_len == 0) ui_show_grid();
}

void card_skip_row(int row) {
    input_skip(g_card[row].action);
    card_remove(row);
    if (g_card_len == 0) ui_show_grid();
}

void card_delay_all(time_t now) {
    time_t until = now + 15 * 60;
    for (int i = 0; i < g_card_len; i++) g_day.snoozed_until[g_card[i].action] = until;
    card_clear();
    ui_show_grid();
}

void card_dismiss(void) {
    // Not a skip and not a miss for anything on the card, checked or not.
    // Nothing is written; card_sync rebuilds it from derive() next tick.
    card_clear();
    ui_show_grid();
}
```

`card_sync` running every tick — rather than the card being built once when it opens — is what makes it correct to leave the board mid-decision: check two boxes, get pulled away, come back an hour later, and the card still reflects exactly what's actually due, with your two checks intact and nothing double-logged.

**The physical key stays single-item.** Checkboxes are a touch affordance; the key's job (§7.4) is answering *something* without looking at the screen, which doesn't compose with "which boxes did I check." A key tap logs `done` for the single highest-priority due action directly, through the same `input_done` as everything else, regardless of what is or isn't checked on the card.

**One edge to be aware of.** A snoozed slot that passes unanswered still derives as a miss, because `derive` only sees the *current* `snoozed_until`, not that the slot was snoozed at the time. With the shortest cadence at 40 minutes a 15-minute delay cannot span a slot boundary, so this is unreachable as configured. It becomes reachable if §11's settings screen lets a cadence go below ~15 minutes — at which point the fix is to log the snooze as an event carrying the slot it covered, not to special-case `derive`. Floor the configurable cadence at 20 minutes.

---

## 7. Board UI

368x448 AMOLED, LVGL. A 368x368 square carries the grid; the remaining 80px is a header with the clock, the date, a battery pip and the sound icon.

```
┌──────────────────────────────────────────┐
│  14:32   Thu 14 Aug            🔊    ▮82% │  ← 80px header
├──────────────────────────────────────────┤
│  ┌────────────┐  ┌────────────┐          │
│  │ Stand      │  │ Water      │          │
│  │ ●●●●○○○○○○ │  │ ●●●◌○○○○   │          │
│  │       12m  │  │        4m  │          │  ← 368×368 grid
│  └────────────┘  └────────────┘          │
│         … five more …                    │
└──────────────────────────────────────────┘
```

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

### 7.3 The prompt card

One screen, N rows (§6). Each row is tinted to its own action rather than the whole screen taking one tint, since a card can hold up to seven different actions at once.

```c
// firmware/main/ui_card.c
static lv_obj_t *g_scr, *g_rows[MAX_CARD_ROWS];

void ui_show_card(void) {
    if (!g_scr) g_scr = lv_obj_create(NULL);
    lv_obj_clean(g_scr);

    for (int i = 0; i < g_card_len; i++) {
        const action_def_t *def = &ACTIONS[g_card[i].action];

        lv_obj_t *row = lv_obj_create(g_scr);
        lv_obj_set_size(row, 320, 64);
        lv_obj_set_style_bg_color(row, lv_color_hex(def->tint_dim), 0);

        lv_obj_t *check = lv_checkbox_create(row);
        lv_checkbox_set_text(check, def->name);
        lv_obj_add_state(check, g_card[i].checked ? LV_STATE_CHECKED : 0);
        lv_obj_add_event_cb(check, on_row_toggle_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);

        lv_obj_t *skip = lv_btn_create(row);
        lv_obj_align(skip, LV_ALIGN_RIGHT_MID, -8, 0);
        lv_obj_add_event_cb(skip, on_row_skip_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        g_rows[i] = row;
    }

    ui_x_button_attach(g_scr, on_dismiss_cb);          // §7.3a — one shared X
    build_confirm_bar(g_scr);                          // Confirm + Delay-all-15

    lv_scr_load_anim(g_scr, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
}

static void on_row_toggle_cb(lv_event_t *e) { card_toggle((int)(intptr_t)lv_event_get_user_data(e)); }
static void on_row_skip_cb(lv_event_t *e)   { card_skip_row((int)(intptr_t)lv_event_get_user_data(e)); }
static void on_confirm_cb(lv_event_t *e)    { card_confirm(); }
static void on_delay_cb(lv_event_t *e)      { card_delay_all(time(NULL)); }
static void on_dismiss_cb(lv_event_t *e)    { card_dismiss(); }
```

A single due action still renders as this same one-row card rather than reverting to a dedicated single-prompt screen — one component, always, so there is nothing separate to keep in sync.

### 7.3a One X, one component

The X — close, no-op, top right — appears on the grid's overlays, the card, and mid-stretch, and it must look and behave identically everywhere it appears rather than being three implementations that happen to agree today and drift tomorrow.

```c
// firmware/main/ui_common.c — the only place an X is built
void ui_x_button_attach(lv_obj_t *parent, lv_event_cb_t on_close) {
    lv_obj_t *x = lv_btn_create(parent);
    lv_obj_set_size(x, 44, 44);                          // fixed, everywhere
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, -8, 8);           // fixed, everywhere
    lv_obj_set_style_bg_opa(x, LV_OPA_TRANSP, 0);
    lv_label_set_text(lv_label_create(x), LV_SYMBOL_CLOSE);
    lv_obj_add_event_cb(x, on_close, LV_EVENT_CLICKED, NULL);
}
```

`ui_show_grid()`, called from every `on_close` above, is the one line that switches the active LVGL screen back to the seven-tile grid — the default view everything else returns to.

### 7.4 Inputs

Four ways to answer, and **all four end at the same two functions**. Nothing else in the firmware writes an event.

```
touch: card row + Confirm ─┐
touch: grid tile          ─┤
physical key               ─┼──► input_done(action)  ──► day_apply_event()
IMU tap (optional)        ─┘                              ▲
                                                          │
web app: POST /event ─────────────────────────────────────┘
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
        feedback_play(CUE_SUCCESS);            // sound only — §9
    }
    idle_timer_reset();                        // screen goes dark 20s from now
}

void input_skip(action_id_t id) {
    log_event_t ev = { .action = id, .kind = KIND_SKIP, .ts = time(NULL),
                       .slot = current_or_next_slot(id), .source = SOURCE_BOARD };
    uuid_v4(ev.id);
    day_apply_event(&ev);
    feedback_play(CUE_READY);
    idle_timer_reset();
}
```

Gating the cue on `day_apply_event`'s return is a small thing that matters in use: confirming a card that includes an already-answered row should chime once for what actually landed, not once per row. The duplicate is caught by the `UNIQUE` constraint, and the silence for that row tells nothing new happened.

Note `input_done`/`input_skip` no longer advance a queue — §6's `card_confirm`/`card_skip_row`/`card_dismiss` own screen transitions, since a card can hold several rows and confirming doesn't necessarily mean the card is empty.

**Touch — grid tiles.** Tapping a tile logs that action with no prompt, which is the "I just drank a glass, credit me" path. Same function, so it re-anchors the timer exactly as answering a prompt would.

```c
static void on_tile_cb(lv_event_t *e) {
    action_id_t id = (action_id_t)(intptr_t)lv_event_get_user_data(e);
    if (screen_is_dark()) { wake_screen(); return; }    // first touch only wakes
    input_done(id);
    tile_flash(id);                                      // brief tint pulse
}
```

**Physical key — the board has two buttons, and only one of them is usable.**

| Button | Read via | Notes |
|---|---|---|
| **BOOT** | `GPIO0`, active low | Free for us at runtime. Strapping pin: held at power-on it enters download mode. |
| **PWR** | `EXIO4` on the I/O expander | 6s hold is a hardware power-off in the PMIC. Not a native GPIO. |

**Use BOOT as the user key.** PWR is disqualified on two counts: it is behind an I²C expander rather than a native pin, so it cannot serve as a deep-sleep wake source and cannot be polled without an I²C transaction; and its long-press is owned by the PMIC as a hardware power-off, so the 6s gesture is not ours to redefine.

BOOT being a strapping pin is worth knowing but not disqualifying: it only matters at power-on, and at runtime `GPIO0` is an ordinary readable input. The practical consequence is one to remember at the bench — resting a finger on the key while plugging in USB drops the board into download mode instead of booting the app.

`GPIO0` is RTC-capable on the ESP32-S3, which is what makes the `ext1` wake in §10 work.

One press logs Done for the single highest-priority due action, which is what you want when the prompt is "stand up" and you are already standing — you can answer it without looking, independent of whatever is or isn't checked on the card (§6).

```c
// firmware/main/key.c
#define KEY_GPIO        GPIO_NUM_0     // BOOT. The only usable key — see above.
#define DEBOUNCE_US     250000         // 250ms
#define LONG_PRESS_US   800000         // 800ms

static void key_task(void *arg) {
    int64_t pressed_at = 0;
    bool    was_down   = false;

    for (;;) {
        bool down = gpio_get_level(KEY_GPIO) == 0;      // BOOT is active low
        int64_t now = esp_timer_get_time();

        if (down && !was_down) {                        // edge: press
            pressed_at = now;
        } else if (!down && was_down) {                 // edge: release
            int64_t held = now - pressed_at;

            if (held < DEBOUNCE_US) {
                /* noise */
            } else if (held > LONG_PRESS_US) {
                // Hold = toggle sound. Deliberate by definition, so it acts
                // straight from a dark screen — no wake-first step. This is
                // the gesture you reach for *because* something just chimed.
                input_toggle_sound();
                wake_screen();                          // show the state briefly
            } else if (screen_is_dark()) {
                wake_screen();                          // first tap only wakes
            } else if (g_card_len > 0) {
                input_done(g_card[0].action);           // tap = done, top priority
            }
        }
        was_down = down;
        vTaskDelay(pdMS_TO_TICKS(20));                  // 50Hz poll is plenty
    }
}
```

```c
// firmware/main/input.c
void input_toggle_sound(void) {
    g_settings.sound = !g_settings.sound;
    store_set_setting("sound", g_settings.sound ? "1" : "0");   // survives reboot

    // Order matters. feedback_play() gates audio on g_settings.sound, so
    // flipping the setting first means this call is naturally silent when
    // switching sound off — no special case, and no chirp from a board you
    // just muted. There is no off-cue at all; the icon (§7.5) carries that half.
    if (g_settings.sound) feedback_play(CUE_SOUND_ON);

    ui_sound_icon_update();                  // the icon is the state, §7.5
    ws_broadcast_state();                    // the web app's icon follows
}
```

**The confirmation is visual, not haptic.** An earlier draft added a vibration motor specifically to confirm the mute-off case, on the reasoning that muting can't confirm itself with sound. **Removed** — this board does not ship with a motor, and adding one is a real hardware change (BOM, wiring, enclosure) that shouldn't be pulled in for one gesture. The icon's visual pulse (§7.5), already built for exactly this purpose, carries it instead: switching sound off dims and swaps the icon and pulses it once; switching on does the same plus a chirp, since by then you can hear it again. The one honest tradeoff: unlike a haptic, the visual confirmation only lands if you're looking at the board when you release the key — acceptable, since the icon's steady state (§7.5) tells you the same thing at a glance a moment later regardless.

**What this costs.** The key previously carried `delay all 15`; sound now owns the hold. Delay-all is touch-only, which is the right trade — muting is the gesture you want blind and in a hurry, and delay-all is one you make while already looking at a card. If you want it back on the key later, a double-tap is free and unambiguous next to a hold.

The wake-then-answer split still applies to the short press: a single tap that both lights the panel and logs a completion means every accidental brush marks water as drunk. First tap wakes, second commits. Acting on *release* rather than press is what lets one button carry both gestures.

Confirm the touch controller part against the schematic for your board revision — the AMOLED boards have changed touch parts between revisions — but the two-button arrangement above is per Waveshare's documentation for this model.

**IMU tap (optional, default off).** The QMI8658 has tap detection, so a knock on the desk beside the board could answer the current prompt without reaching for it — genuinely nice for "stand break", since you are already moving.

```c
static void imu_tap_isr(void *arg) {
    // Only ever answers something already on the card. A tap must never
    // be able to log something the user was not being asked about — the
    // false-positive rate on desk knocks is far too high for that.
    if (g_card_len == 0 || !g_settings.imu_tap) return;
    xQueueSendFromISR(g_input_q, &(input_msg_t){ .kind = INPUT_TAP }, NULL);
}
```

Prototype it behind the setting and keep the guard: an IMU tap can only answer, never originate.

### 7.5 The sound icon

The icon *is* the state. It sits in the header, always visible, and it is also the third way to toggle — tap it. There is no toast and no settings dive: you can see whether the board will make a noise without touching anything.

```c
// firmware/main/ui_header.c
static lv_obj_t *g_sound_icon;

void ui_header_build(lv_obj_t *parent) {
    g_sound_icon = lv_label_create(parent);
    lv_obj_set_style_text_font(g_sound_icon, &lv_font_montserrat_24, 0);
    lv_obj_align(g_sound_icon, LV_ALIGN_RIGHT_MID, -70, 0);

    // Generous hit area: the glyph is 24px, the target is 56px square.
    lv_obj_add_flag(g_sound_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(g_sound_icon, 16);
    lv_obj_add_event_cb(g_sound_icon, on_sound_icon_cb, LV_EVENT_CLICKED, NULL);

    ui_sound_icon_update();
}

void ui_sound_icon_update(void) {
    lv_label_set_text(g_sound_icon,
        g_settings.sound ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE);

    // Muted is dimmed as well as different: the shape carries the meaning at
    // a glance, the weight confirms it. Colour alone would not survive the
    // amber and yellow tints washing the tiles underneath.
    lv_obj_set_style_text_color(g_sound_icon,
        lv_color_hex(g_settings.sound ? 0xE6EDF3 : 0x5A636D), 0);

    pulse(g_sound_icon);        // brief scale bump, so a change catches the eye
}

static void on_sound_icon_cb(lv_event_t *e) {
    if (screen_is_dark()) { wake_screen(); return; }
    input_toggle_sound();       // same function the key hold calls, §7.4
}
```

Three ways in, one function out — the key hold (§7.4), this tap, and `POST /settings` from the web app (§8.2) all call `input_toggle_sound`, exactly as the four completion paths all call `input_done`.

The pulse is what replaces the toast. A hold on the key can happen while you are looking at the panel or not, so the icon animates on change rather than sitting there statically — enough to catch the eye if you are watching, invisible if you are not, and no text to read either way.

**Icon, not switch.** A toggle switch would need a label to say what it toggles and would take three times the header width. A speaker glyph reads instantly at 24px and is the same idiom every phone uses.

### 7.6 Stretch flow

Replaces the prompt screen. One stretch per screen with a countdown ring, auto-advancing on completion, Next to advance early, Skip to abandon the set.

Derive the remaining time from an absolute end timestamp rather than decrementing a counter, so a delayed render resumes at the correct point instead of stretching a 40-second hold into a minute.

Abandoning the set part-way logs a `skip`, not a partial `done` — a half-finished stretch set is a skipped stretch set, and the dot should say so.

---

## 8. Web app

A viewer and a remote. It renders the `DayView` the board already derived (§3.3) and can log a completion. **It has no scheduler and raises no prompts.**

### 8.0 One frontend, a small backend contract

Stated as an explicit architectural rule rather than left as an accident of how §8.2/8.3 happen to be written: **the React app is the one frontend**, and everything it talks to — the board today, potentially something else later — implements the same small contract: `GET /state`, `POST /event`, `POST /settings`, `GET /ws`. The frontend doesn't know or care what's behind that contract; it only knows the shapes in §8.1.

This is what "I might want to host this on an app or a website in the future" (the reason WiFi is worth keeping at all — §1, §12) actually costs to support, which is close to nothing extra:

- **A website** is the same static build, deployed anywhere, still pointed at `wfh.local` on the same LAN — or, if remote access is ever wanted, at a relay that speaks the same three-endpoint-plus-socket contract. Nothing in the frontend changes; only where it's served from does.
- **A native app** is the same build wrapped in a WebView shell (Capacitor or similar). It's still a client of the same HTTP/WS API, so the board's firmware doesn't change either.
- **A different backend entirely** — say, a pure-web version with no board, storing to `localStorage` or a small server — is a new implementation of the same four endpoints, not a new frontend.

**The board's own screen is not this frontend.** §7 established that literal React can't run on the ESP32-S3 at the touch latency this needs, so the board's LVGL/C screens are a second, necessarily separate renderer of the same underlying data (`DayView` plus the action config) — not a second frontend, and not something that should grow its own opinions about layout or copy that the web app doesn't share. Keep the two in sync by sharing the source of meaning (§2.1's generated config, §3's `DayView` shape), not by trying to share code that can't actually run in both places.

### 8.1 The protocol

A handful of endpoints and one socket. Everything is JSON; nothing is versioned beyond the `version` field in the payload, because both ends ship together.

| | | |
|---|---|---|
| `GET /state` | → `{ log, view, power }` | full snapshot, used once on load |
| `POST /event` | `{ action, kind, ts, slot }` → `{ ok, applied }` | log a completion |
| `POST /settings` | `{ sound?, volume? }` → `{ ok }` | flip a toggle from the browser |
| `GET /ws` | ← `{ type: 'state', … }` | push on every change |
| `GET /logs` | → last 24h of device log lines | debugging, §14 |
| `GET /` | → the web app itself | static files from LittleFS |

**The snapshot.** One shape, sent by both `GET /state` and every WebSocket push, so the client has exactly one code path for "here is the world".

```jsonc
{
  "type": "state",
  "log":  { "version": 3, "date": "2026-08-14", "events": [ … ] },
  "view": {                                  // derived on the board, §3.3
    "counts":  { "water": 3, "stand": 4 },
    "skipped": { "stand": 1 },
    "missed":  { "water": 2 },
    "next":    { "water": 1723648800 },
    "due":     ["water"]
  },
  "power":    { "battPct": 82, "charging": true, "onBattery": false },
  "settings": { "sound": true, "volume": 70 },   // so the web toggle/slider
                                                 // follows a hold on the board, §7.4
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
// ── POST /settings ───────────────────────────────────────────────────────
// The web app's sound icon is a remote for the board's. Partial updates:
// only the keys present are changed.
static esp_err_t settings_post(httpd_req_t *req) {
    char buf[128];
    int n = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf) - 1));
    if (n <= 0) return ESP_FAIL;
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    cJSON *sound = cJSON_GetObjectItem(root, "sound");
    cJSON *volume = cJSON_GetObjectItem(root, "volume");

    if (cJSON_IsBool(sound) && cJSON_IsTrue(sound) != g_settings.sound) {
        input_toggle_sound();     // same path as the key hold and the icon tap:
                                  // persists, updates the icon, rebroadcasts
    }
    if (cJSON_IsNumber(volume)) {
        g_settings.volume = volume->valueint;
        store_set_setting("volume", int_to_str(g_settings.volume));
        ws_broadcast_state();
    }
    cJSON_Delete(root);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// ── GET /ws ──────────────────────────────────────────────────────────────
// 4 is neither tight nor generous — realistic concurrent viewers are 1, maybe
// 2 (phone + laptop open at once), and each open socket costs a small fixed
// buffer against 8MB of PSRAM, so this has headroom without being large
// enough to matter for memory. Exceeding it logs a warning and drops the new
// connection (below) rather than failing anything, so the cost of guessing
// wrong is "can't open a 5th tab," not a crash.
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
        { .uri = "/state",    .method = HTTP_GET,  .handler = state_get    },
        { .uri = "/event",    .method = HTTP_POST, .handler = event_post   },
        { .uri = "/settings", .method = HTTP_POST, .handler = settings_post },
        { .uri = "/logs",     .method = HTTP_GET,  .handler = logs_get    },  // §14
        { .uri = "/ws",       .method = HTTP_GET,  .handler = ws_handler, .is_websocket = true },
        { .uri = "/*",        .method = HTTP_GET,  .handler = static_get   },  // the web app
    };
    for (int i = 0; i < 6; i++) httpd_register_uri_handler(g_server, &routes[i]);

    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("wfh"));            // → wfh.local
    ESP_ERROR_CHECK(mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0));
}
```

**Serve the web app from the board.** `http://wfh.local` talking to `http://wfh.local` sidesteps the mixed-content block that would stop an HTTPS-hosted page from reaching the board over plain HTTP. The built bundle goes on the LittleFS partition and is served by `static_get`; at a few hundred KB it fits comfortably in 16MB alongside the firmware and the database.

### 8.3 Web app side — a stateless remote

One class, and it holds no local write state at all: no outbox, no optimistic update. It owns the socket, the reconnect, and the clock skew, and it hands the rest of the app a plain snapshot.

```ts
// web/src/board.ts
type Snapshot = { log: DayLog; view: DayView; power: Power; now: number };

export class Board {
  private ws?: WebSocket;
  private backoff = 1000;                       // grows to 30s, resets on connect
  skew = 0;                                     // board clock − browser clock
  online = false;

  constructor(private host = location.host, private onSnapshot: (s: Snapshot) => void) {}

  // ── connect ────────────────────────────────────────────────────────────
  start() {
    this.ws = new WebSocket(`ws://${this.host}/ws`);

    this.ws.onopen = () => { this.online = true; this.backoff = 1000; };

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
  // No optimistic update, no outbox. Success or failure is reported to the
  // caller directly; a failed tap is the caller's problem to surface (§6's
  // checklist card leaves the row checked and shows an inline error rather
  // than silently queueing the tap for later).
  async log(action: ActionId, kind: EventKind, slot: number): Promise<boolean> {
    const ev: LogEvent = {
      id: crypto.randomUUID(), action, kind, slot,
      ts: this.boardNow(), source: 'web',
    };
    try {
      const r = await fetch(`http://${this.host}/event`, {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify(ev),
      });
      return r.ok;                              // the WS push updates the view
    } catch {
      return false;
    }
  }

  /** Flip the board's sound from the browser. The board is authoritative:
   *  we send the intent and let the push tell us what actually happened,
   *  so the icon can never show a state the board is not in. */
  async setSound(on: boolean) {
    await fetch(`http://${this.host}/settings`, {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ sound: on }),
    });
    // No optimistic update here — unlike a tap, there is nothing to feel
    // impatient about, and a toggle that flickers back is worse than one
    // that waits 20ms.
  }

  /** The board's clock, not the browser's. Countdowns must agree with the tile. */
  boardNow(): number { return Math.floor(Date.now() / 1000) + this.skew; }
}
```

Without an optimistic update, a tap on the laptop lands a beat later than a tap on the board — the round trip to `POST /event` plus the WS push back. That's an accepted, honest cost of statelessness: the alternative was a client that pretends to know things it doesn't yet, and unwinding that pretense on failure was exactly the machinery §3.5 removed.

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

Asleep, off the LAN, or flat — all indistinguishable from a closed socket, and the client does not need to tell them apart. `Board` keeps the last snapshot and reconnects with backoff. The UI shows a plain marker, disables logging, and stops the countdowns, which would otherwise tick down to zero and lie.

```tsx
{!board.online && <div className="banner">Board offline — nothing can be logged until it returns</div>}
```

The header carries the same sound icon as the board (§7.5), driven by the same state and reaching the same function:

```tsx
<button className="icon-btn" onClick={() => board.setSound(!s.settings.sound)}
        aria-label={s.settings.sound ? 'Mute board' : 'Unmute board'}
        aria-pressed={s.settings.sound} disabled={!board.online}>
  <Icon name={s.settings.sound ? 'volume-up' : 'volume-off'} />
</button>
```

**The web app does not prompt in this state.** No sound, no cards, no timers. Filling the gap with browser-side prompting would resurrect exactly the two-scheduler problem §1 exists to avoid, and would do it at the worst possible moment: when the board comes back and both start chiming.

---

## 9. Feedback

Four cues, matching the reviewed prototype's vocabulary, all sound — no motor is fitted on this board, so there is one channel, not two (§7.4).

| Event | Sound |
|---|---|
| Prompt appears | `bloom` — rising two-note |
| Action logged | `success` — quick up-tick |
| Stretch step complete | `ready` — single soft note |
| Sound switched **on** | `success` — proof you can hear it |
| Sound switched **off** | *(none — visual only, §7.5)* |

```c
// firmware/main/feedback.h
typedef enum { CUE_BLOOM, CUE_SUCCESS, CUE_READY, CUE_SOUND_ON, CUE_COUNT } cue_t;

void feedback_play(cue_t cue);      // non-blocking
```

```c
// firmware/main/feedback.c — the one table
static const tone_t BLOOM[]   = { {523, 90}, {659, 140} };        // C5 → E5
static const tone_t SUCCESS[] = { {659, 70}, {880, 110} };        // E5 → A5
static const tone_t READY[]   = { {440, 120} };                   // A4

static const cue_def_t CUES[CUE_COUNT] = {
    [CUE_BLOOM]     = { BLOOM,   2 },
    [CUE_SUCCESS]   = { SUCCESS, 2 },
    [CUE_READY]     = { READY,   1 },
    [CUE_SOUND_ON]  = { SUCCESS, 2 },
    // no CUE_SOUND_OFF: muting is confirmed by the icon's pulse, not a sound
};

void feedback_play(cue_t cue) {
    if (g_settings.sound) audio_play(CUES[cue].tones, CUES[cue].n_tones);
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
if (g_card_len > 0 && now - g_card_raised_at == 120) {
    audio_set_volume(g_settings.volume + 15);
    feedback_play(CUE_BLOOM);
    audio_set_volume(g_settings.volume);
}
```

### 9.2 In the web app

Off by default. The board is making the noise, and a laptop echoing every cue a second time is worse than silence.

If enabled, `cuelume` (MIT, ESM, ~5kB, synthesised live, no audio files) driven from the WebSocket push — the same three cues, so the vocabulary stays consistent wherever you hear it. Resume the `AudioContext` on the first gesture or the first cue of the session is silent:

```ts
const unlock = () => { ctx?.resume(); document.removeEventListener('pointerdown', unlock); };
document.addEventListener('pointerdown', unlock, { once: true });
```

---

## 10. Power

The board is **not** always on. On USB-C it effectively is; on battery it is not, and the design has to say what happens instead.

**Rough numbers.** The 1.8" AMOLED dominates — call it 80–150mA lit depending on how much of the panel is emitting, against maybe 40–50mA for the ESP32-S3 with WiFi associated and the screen off. A 500mAh-class cell is therefore a handful of hours screen-on and most of a day screen-off. Measure at the PMIC rather than trusting this paragraph.

So **screen-on time is the budget, not uptime** — which suits an app that is idle 99% of the time: seven prompts an hour at most, each needing a few seconds of attention.

| State | Trigger | Wakes on | Screen | WiFi | Serves HTTP |
|---|---|---|---|---|---|
| Active | prompt firing, or touch within 20s | — | on | on | yes |
| Idle | no touch for 20s, inside working hours | any touch or key press | off | modem-sleep (DTIM) | yes |
| Dormant | outside working hours | RTC timer (next `workStart`) or key press | off | off | no |

**Idle is the important one.** Screen off with WiFi in modem-sleep keeps the TCP stack alive, so `/state` and `/ws` keep working and the web app never notices, while the panel — the actual cost — is dark. A touch or key press takes it straight back to Active, same path as the wake sources listed for Dormant below.

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
    // BOOT/GPIO0 is RTC-capable, so it can wake us. PWR cannot — it is behind
    // the I/O expander, which is unpowered in deep sleep. §7.4
    esp_sleep_enable_ext1_wakeup(BIT64(KEY_GPIO), ESP_EXT1_WAKEUP_ANY_LOW);
    sqlite3_close(g_db);                          // checkpoint WAL before sleep
    esp_deep_sleep_start();
}
```

**Running flat mid-day writes nothing corrupt, but it does cost misses.** The board dies at 14:00, gets plugged in at 15:30, and boots with the database intact on flash and the correct wall-clock time from the PCF85063 — no data loss, no crash. But per §3.1's simplified rule, every slot that fell between 14:00 and 15:30 is now unanswered and in the past, so it derives as missed, the same as if the board had been on the whole time and simply failed to prompt. That is the accepted cost of removing awake-window tracking: correctness of the stored data is unaffected, but the day's dot row will show a real gap. §10's own recommendation — run it on USB-C — is what keeps this rare in practice.

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
        ui_show_battery_warning();                 // on the board's own screen
        break;
    case AXP2101_BATT_CRITICAL:                   // ~5%
        sqlite3_close(g_db);                       // checkpoint WAL before sleep
        enter_dormant(0);                         // wake on USB only
        break;
    }
}
```

Closing the database connection on the critical interrupt checkpoints the WAL, so the next boot opens a clean database rather than replaying a journal.

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
3. **Storage** — LittleFS partition, SQLite, schema, `store_add_event` / `store_load_day`. Verifiable on its own with a serial console before any UI exists.
4. **`derive` and `day_apply_event`** — the walk, the anchor rule, plus the fixtures in §11.1. **This is the step to get right**; everything else is presentation.
5. **Scheduler tick and the checklist card** (§6) — due detection, `card_sync`, Confirm/Skip/Delay-all/X. Testable on-desk by moving the RTC forward.
6. **Grid UI** — tiles, dots, wash, header, the shared X component (§7.3a). First point at which the thing looks like itself.
7. **Inputs** — card rows, tile-tap logging, physical key (§7.4).
8. **Sound** — the cue table, the I2S tone task, escalation (§9).
9. **Guided stretch flow.**
10. **HTTP server** — `/state`, `/event`, `/settings`, `/ws`, `/logs` (§8, §14), mDNS. The board is complete and usable on its own at this point.
11. **Web app** — the reviewed grid UI, the `Board` client, served from LittleFS.
12. **Power states** — light sleep, brightness, PMIC events, Dormant.
13. **Settings screen** — working hours, per-action cadence, sound, volume, IMU tap.

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
  unanswered-past-slot.json         unanswered + in the past = missed, always
  duplicate-event.json              second apply is a no-op
  replay-idempotent.json            applying the same event twice changes nothing
  card-confirm-partial.json         confirming checked rows leaves unchecked ones due
  stretch-priority.json             a due stretch pre-empts the checklist
  dst-forward.json                  no slots lost or doubled on the changeover
```

The last three are the ones worth having. Card-confirm-partial is the case that would silently regress if `card_confirm` and `derive` ever disagree about which rows are still open, idempotence under replay is what makes a browser retry or a double-tap on Confirm harmless even without an outbox (§3.5), and DST is the bug that will otherwise appear twice a year and be impossible to reproduce.

### 11.2 Day rollover

Handled in the tick (§5.2), not on boot: the board runs for weeks at a time, so midnight is the common case rather than an edge case. There is no "reset the timers" step — `next` derives from an empty log against today's date, so a fresh day is literally an empty struct.

---

## 12. Known constraints

- **Single point of failure.** The board is the product. If it is off the LAN, the web app is a read-only cache; if it is dead, there is no tracker. Accepted deliberately — the alternative is two schedulers.
- **Firmware dev loop.** Flash-and-test is slower than a browser reload. §11.1's fixtures and a host-compiled unit test target for `derive` and the slot maths take most of the sting out; build those early.
- **LAN-only, unauthenticated.** Anyone on the same network can read `/state` and post to `/event`. Fine for a home LAN; not fine on a shared or office network. If that changes, put a shared secret in a header before exposing it further. WiFi is required for this to work at all — HTTP and WebSocket both need the browser and the board on the same network, and WiFi is the board's only radio (no Ethernet). It never needs internet access; this is entirely LAN-local, no account, no cloud dependency.
- **No cross-device sync beyond the board.** The board is the only writer of record; two browsers reach it independently, not each other.
- **No reach away from the desk.** Deliberate. If you are not at the board, it does not prompt you, and it does not tell your phone.
- **A board that was off accumulates real misses.** Simplified from an earlier draft that tracked presence explicitly (§3.1) — a board that slept, ran flat, or was unplugged for part of the day will show every slot it missed during that gap as missed when it returns, the same as if it had simply failed to prompt. Accepted for the simplicity; §10 recommends USB-C precisely to keep this rare.
- **Clock.** NTP at boot when WiFi is available, PCF85063 otherwise. A board that has never seen NTP and has a flat backup cell will have a wrong date, and the day key will be wrong with it. Show the date in the header so this is visible rather than silent.

---

## 13. What SQLite buys

The schema and the storage code are in §3.4. This section is why, and where the line is.

### 13.1 It removes code rather than adding it

The model in §3.1 — an append-only log of uniquely-keyed rows — *is* relational. Storing it as packed blobs meant hand-writing the things a database already does. Two examples from earlier drafts of this plan, both now deleted:

| Was | Is |
|---|---|
| A linear scan over the events array to reject duplicates | `UNIQUE (action, slot)` + `INSERT OR IGNORE` |
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

Retention is 400 days (§3.4), so the data for history is accumulating from day one even though v1 renders only today. When historical views arrive they are queries, not a migration:

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

---

## 14. Device logs

There is currently no answer to "why did it crash at 3am" beyond a serial cable plugged into a board that's sitting on someone's desk, which in practice means no answer at all.

**A rolling 24-hour buffer, verbose, written as things happen.** State transitions (Active/Idle/Dormant, power events), WiFi association and drops, watchdog resets, SQLite errors — anything worth an `ESP_LOG` call is worth keeping past the point the serial console scrolled past it.

```c
// firmware/main/devlog.c
// A fixed-size ring in LittleFS. Old lines are overwritten, not deleted —
// there is no unbounded growth to prune, and no SQLite involvement: this is
// diagnostic scratch, not data the product depends on being correct.
#define DEVLOG_PATH      "/fs/devlog.txt"
#define DEVLOG_MAX_BYTES (256 * 1024)          // ~24h of verbose logging, comfortably

void devlog_write(const char *tag, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char line[256];
    int n = snprintf(line, sizeof(line), "%lld [%s] ", (long long)time(NULL), tag);
    n += vsnprintf(line + n, sizeof(line) - n, fmt, ap);
    va_end(ap);

    ring_append(DEVLOG_PATH, DEVLOG_MAX_BYTES, line, n);   // wraps at the size cap
    ESP_LOGI(tag, "%s", line);                             // still visible over serial
}
```

```c
// GET /logs — pullable from the web app with no cable attached.
static esp_err_t logs_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/plain");
    return ring_send(DEVLOG_PATH, req);        // streams the file as-is
}
```

Every call site that currently does `ESP_LOGx(...)` on something worth remembering becomes `devlog_write(...)`, which does both — visible on a cable if one happens to be attached at the time, and pullable afterward if it wasn't. The ring is capped and self-overwriting specifically so this can be sprinkled liberally without a slow flash-fill-up turning into its own bug.
