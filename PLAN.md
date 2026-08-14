# WFH Health Tracker — Implementation Plan

Status: UX prototype built and reviewed. This document is the spec for the real build.

---

## 1. Scope

A square-format habit tracker that prompts seven actions on independent timers during working hours, chimes and vibrates, and logs one tap per completion. Today-at-a-glance only, no historical views in v1.

**One build, one source of truth: the board.**

Hardware: Waveshare ESP32-S3-Touch-AMOLED-1.8 (368x448, ESP32-S3R8, 8MB PSRAM, 16MB flash, QMI8658 IMU, PCF85063 RTC, AXP2101 PMIC, speaker, battery).

- The **board** owns the schedule, the clock, the event log, the prompting, the sound and the haptics. It is the product.
- The **web app** is a viewer and a remote. It renders the same day from the same log and can log a completion, but it never schedules and never prompts.

This was previously split into a browser-first phase and a hardware phase. That split meant building a browser scheduler with known-unfixable reliability problems — tab suspension, no iOS haptics, no away-from-desk reach — and then deleting it. The single-phase version skips the throwaway.

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

### 3.3 Two implementations, one behaviour

`derive` exists twice: in C for the board's own UI (and on boot, to rebuild state from the NVS log) and in TypeScript for the web app. That duplication is the single largest correctness risk in this design — two implementations of a fiddly walk, in two languages, that must agree exactly or the board and the laptop will show different numbers for the same day.

**Mitigation: shared fixtures.** One directory of JSON cases, both test suites load it.

```json
// fixtures/derive/interval-done-early.json
{
  "name": "interval action done early re-anchors the next slot",
  "settings": { "workStart": "09:00", "workEnd": "18:00" },
  "now": 1723640400,
  "log": { "version": 2, "date": "2026-08-14",
           "events": [ { "action": "water", "kind": "done", "ts": 1723637400, "slot": 1723638120 } ],
           "awake": [[1723633200, 1723640400]], "snoozedUntil": {} },
  "expect": { "counts": { "water": 1 }, "missed": {}, "next": { "water": 1723640100 } }
}
```

Adding a case means adding a file. Neither implementation may be changed without both suites passing. If a third client ever appears, it inherits the fixtures for free.

**The board's copy is authoritative** if they ever disagree in the field: the board is the writer and the thing making noise. The web app showing a different count is a web app bug.

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

### 3.5 Persistence (NVS)

The board's store. One blob per day, rewritten on each event — at ~4KB and ~30 events a day this is far below anything flash wear makes interesting.

```c
static esp_err_t day_persist(const day_log_t *log) {
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open("wfh", NVS_READWRITE, &h));
    char key[16];
    snprintf(key, sizeof(key), "d%s", log->date);        // "d2026-08-14"
    esp_err_t err = nvs_set_blob(h, key, log, day_log_size(log));
    if (err == ESP_OK) err = nvs_commit(h);              // commit before returning
    nvs_close(h);
    return err;
}
```

Persist synchronously on every event, before the UI advances. A completion that is acknowledged on screen but lost to a power cut is the one failure this design must not have. Prune to the last 14 days on boot.

### 3.6 Merge

The web app can log while the board is unreachable (§8.3), so the two logs must reconcile. Because every event carries a unique `(action, slot)`, union *is* the merge — commutative, idempotent, order-independent.

```ts
export function merge(a: DayLog, b: DayLog): DayLog {
  const byKey = new Map<string, LogEvent>();
  for (const e of [...a.events, ...b.events]) byKey.set(`${e.action}@${e.slot}`, e);
  return {
    ...a,
    events: [...byKey.values()].sort((x, y) => x.ts - y.ts),
    awake: mergeWindows([...a.awake, ...b.awake]),
    snoozedUntil: maxPerKey(a.snoozedUntil, b.snoozedUntil),
  };
}
```

Note that only the *board* contributes awake windows in practice. The web app being open says nothing about whether the user was prompted, so it must never write one — a laptop open at 03:00 must not turn the night into missed slots.

---

## 4. Firmware architecture

Four FreeRTOS tasks. Everything that mutates the log funnels through one of them.

```
┌────────────┐   due actions   ┌────────────┐
│ scheduler  │ ──────────────► │    ui      │  LVGL, touch, screens
│  1 Hz tick │                 │  (LVGL tz) │
└─────┬──────┘                 └─────┬──────┘
      │                              │ on_done / on_skip
      │  day_mark_awake              ▼
      │                     ┌─────────────────┐
      └────────────────────►│ day_apply_event │  ← the ONLY mutation path
                            └────────┬────────┘
        ┌────────────┐               │            ┌──────────┐
        │  httpd     │───────────────┘            │  notify  │
        │ /state /ws │◄── ws_broadcast ───────────│ notifi.it│
        └────────────┘                            └──────────┘
```

`day_apply_event` is the only function that writes the log. The touch handler calls it, `POST /event` calls it, and nothing else does. That single choke point is what makes a board tap and a browser tap genuinely the same operation rather than two code paths that happen to look alike.

```c
// The choke point. Idempotent on (action, slot).
esp_err_t day_apply_event(day_log_t *log, const log_event_t *ev) {
    for (size_t i = 0; i < log->events_len; i++) {
        if (log->events[i].action == ev->action && log->events[i].slot == ev->slot) {
            return ESP_OK;                     // already have it; not an error
        }
    }
    if (log->events_len >= EVENTS_MAX) return ESP_ERR_NO_MEM;
    log->events[log->events_len++] = *ev;

    esp_err_t err = day_persist(log);           // durable before we acknowledge
    if (err != ESP_OK) return err;

    view_invalidate();                          // recompute on next render
    ws_broadcast_state();                       // every connected browser updates
    return ESP_OK;
}
```

Returning `ESP_OK` for a duplicate rather than an error matters: a web client retrying a request it never saw the response to must not get a failure for an event that landed.

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
            day_persist(&g_day);
            day_init(&g_day, now);
        }

        day_mark_awake(&g_day, now);             // §3.4 — the same write that
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

```c
void on_done(action_id_t id) {
    time_t now = time(NULL);
    log_event_t ev = {
        .action = id,
        .kind   = KIND_DONE,
        .ts     = now,                       // PCF85063-backed; correct after reboot
        .slot   = current_or_next_slot(id),  // the slot being answered
        .source = SOURCE_BOARD,
    };
    uuid_v4(ev.id);
    day_apply_event(&g_day, &ev);
}
```

The interval/fixed asymmetry lives entirely in `derive`'s `anchor`, not here: drinking water at 11:20 pushes the next glass to 12:05, while eating lunch at 12:40 leaves the 13:00 slot exactly where it was. Having that branch in one place rather than at every call site is the point.

`current_or_next_slot` resolves to the due slot if one is due, and to the next upcoming slot otherwise — which is what makes tapping a tile on the grid ("I just drank one, credit me") work through the same path as answering a prompt.

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

### 7.4 Physical key

One press = Done on the current prompt, which is what you want when the prompt is "stand up" and you are already standing.

```c
static void key_task(void *arg) {
    int64_t last = 0;
    for (;;) {
        if (gpio_get_level(PIN_BTN) == 0) {               // active low — verify
            int64_t now = esp_timer_get_time();
            if (now - last > 250000) {                     // 250ms debounce
                last = now;
                if (screen_is_dark())      wake_screen();  // first press wakes only
                else if (g_queue_len > 0)  on_done_current();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
```

The wake-then-answer split matters: a single press that both lights the panel and logs a completion means every accidental brush marks water as drunk. First press wakes, second press commits.

**Check the touch controller part and the key GPIOs against the Waveshare schematic for your board revision** rather than trusting the numbers here — the AMOLED boards have changed touch parts between revisions, and the BOOT key is shared with strapping on some layouts.

**Tap-to-answer via the IMU** is worth prototyping as a third input: the QMI8658 has tap detection, so a knock on the desk beside the board could answer the current prompt without reaching for it — genuinely nice for "stand break". It needs a real false-positive threshold before it goes anywhere near `day_apply_event`. Prototype behind a setting, default off.

### 7.5 Stretch flow

Replaces the prompt screen. One stretch per screen with a countdown ring, auto-advancing on completion, Next to advance early, Skip to abandon the set.

Derive the remaining time from an absolute end timestamp rather than decrementing a counter, so a delayed render resumes at the correct point instead of stretching a 40-second hold into a minute.

Abandoning the set part-way logs a `skip`, not a partial `done` — a half-finished stretch set is a skipped stretch set, and the dot should say so.

---

## 8. Web app

A viewer and a remote. It renders `DayView` from the board's `DayLog` using the same `derive`, and it can log a completion. **It has no scheduler and raises no prompts.**

### 8.1 API

```c
// GET /state → the DayLog verbatim. Facts, not conclusions.
static esp_err_t state_get(httpd_req_t *req) {
    char *json = day_log_to_json(&g_day);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

// POST /event {"action":"water","kind":"done","ts":…,"slot":…,"source":"web"}
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
    day_apply_event(&g_day, &ev);            // same choke point as the touch handler
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}
```

Shipping the log rather than the derived view means the two can never disagree about what a miss is. It also keeps the payload small — a full day is ~30 events, well under 4KB.

`GET /ws` pushes the log on every change, so the browser updates the instant someone taps the board.

Discovery via mDNS at `wfh.local`, so no hardcoded IP.

**Serve the web app from the board.** `http://wfh.local` to `http://wfh.local` avoids the mixed-content block that would otherwise stop an HTTPS-hosted page from talking to the board over plain HTTP. The built bundle goes in a SPIFFS partition and is served by the same `esp_http_server`; at a few hundred KB it fits comfortably in 16MB of flash alongside the firmware.

### 8.2 Client

```ts
export interface Backend {
  load(): Promise<DayLog>;
  log(ev: LogEvent): Promise<void>;
  subscribe(cb: (l: DayLog) => void): () => void;
}

export const boardBackend = (host = 'wfh.local'): Backend => ({
  load: () => fetch(`http://${host}/state`).then(r => r.json()),
  log: ev => fetch(`http://${host}/event`, {
    method: 'POST', headers: { 'content-type': 'application/json' },
    body: JSON.stringify(ev),
  }).then(() => void 0),
  subscribe(cb) {
    const ws = new WebSocket(`ws://${host}/ws`);
    ws.onmessage = e => cb(JSON.parse(e.data));
    return () => ws.close();
  },
});
```

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

### 8.3 When the board is away

Asleep, off the LAN, or flat — all indistinguishable from a failed fetch, and the client does not need to know which. It shows the last log it saw, still accepts taps, and reconciles on reconnect via §3.6.

```ts
export const resilient = (board: Backend, cache: Backend): Backend => ({
  async load() {
    try {
      const fresh = await board.load();
      await cache.save(fresh);
      return outbox.length ? merge(fresh, outboxAsLog()) : fresh;
    } catch {
      return cache.load();                    // board away: last known state
    }
  },
  async log(ev) {
    await cache.append(ev);                   // never lose the tap
    try { await board.log(ev); } catch { outbox.push(ev); }
  },
  subscribe: board.subscribe,
});
```

Local write first, board write second, best-effort. The retry is safe at any multiplicity because `(action, slot)` dedupes.

**The web app does not prompt in this state.** It shows a plain "board offline" marker in the header and nothing else changes. Filling the gap with browser-side prompting would resurrect exactly the two-scheduler problem §1 exists to avoid, and would do it at the worst possible moment — when the board comes back and both start chiming.

---

## 9. Feedback

### 9.1 On the board

The speaker and a vibration motor on a PWM GPIO. Three cues, matching the reviewed prototype's vocabulary:

| Event | Cue | Haptic |
|---|---|---|
| Prompt appears | `bloom` | triple pulse `[40,60,40,60,40]` |
| Action logged | `success` | short tick `[25]` |
| Stretch step complete | `ready` | light tick `[12]` |

*Note:* `bloom` is soft and may not cut through when heads-down. Test against `chime` and `ready` before locking it in — and test it on the board's actual speaker, which is small and will not reproduce the low end the way a laptop does. The cue that works in a browser prototype is not necessarily the cue that works here.

Escalate rather than repeat: if a prompt goes unanswered for two minutes, re-cue once at higher volume. If it is still unanswered at the next slot it becomes a derived miss and goes quiet — the board should never nag in a loop.

### 9.2 Away from the desk

The board `POST`s to `notifi.it/send` for a native push on iPhone or Mac. One HTTP request, no SDK, no account.

```c
static void notify(const char *title, const char *body) {
    esp_http_client_config_t cfg = {
        .url = "https://notifi.it/send",
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"channel\":\"%s\",\"title\":\"%s\",\"body\":\"%s\"}",
             CONFIG_WFH_NOTIFI_CHANNEL, title, body);
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_post_field(c, payload, strlen(payload));
    esp_http_client_perform(c);          // fire and forget; never block the scheduler
    esp_http_client_cleanup(c);
}
```

Gate it on absence: send only when the IMU has reported no movement and the touch panel no interaction for several minutes. Pushing every prompt to the phone while the user is sitting in front of the board is how the phone becomes the thing they mute.

### 9.3 In the web app

Optional and secondary, since the board is making the noise. If enabled, `cuelume` (MIT, ESM, ~5kB, synthesized live) with a fallback to raw oscillators. Resume the `AudioContext` on first gesture or the first cue of the session is silent:

```ts
const unlock = () => { ctx?.resume(); document.removeEventListener('pointerdown', unlock); };
document.addEventListener('pointerdown', unlock, { once: true });
```

Haptics in the browser are a non-starter on iPhone — `navigator.vibrate` is unsupported in iOS Safari. Feature-detect and hide the toggle rather than showing a switch that does nothing. This costs nothing now that the board carries the haptics.

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
    esp_sleep_enable_ext1_wakeup(BIT64(PIN_BTN), ESP_EXT1_WAKEUP_ANY_LOW);
    day_awake_close(&g_day, time(NULL));
    day_persist(&g_day);                          // flush before we lose RAM
    esp_deep_sleep_start();
}
```

**Running flat mid-day is a non-event.** This is the payoff from §3. The board dies at 14:00, gets plugged in at 15:30, and boots with the log intact in NVS and the correct wall-clock time from the PCF85063. It opens a new awake window at 15:30, and the slots between 14:00 and 15:30 are outside every window, so they are *not* misses. The user is not blamed for a dead battery.

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
        day_awake_close(&g_day, time(NULL));
        day_persist(&g_day);
        notify("WFH tracker low", "Battery at 15% — plug it in");
        break;
    case AXP2101_BATT_CRITICAL:                   // ~5%
        day_awake_close(&g_day, time(NULL));
        day_persist(&g_day);
        enter_dormant(0);                         // wake on USB only
        break;
    }
}
```

`day_awake_close` on the low-battery interrupt is the line that matters for correctness: it seals the window while there is still power to write NVS, so the log honestly records when the board stopped being able to prompt.

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
3. **Data model in C** — `day_log_t`, `day_apply_event`, NVS persist and load, prune. Plus `derive` and the shared fixtures from §3.3. **This is the step to get right**; everything else is presentation.
4. **Scheduler tick** — awake windows, jittered offsets, due detection. Testable on-desk by moving the RTC forward.
5. **Grid UI** — tiles, dots, wash, header. First point at which the thing looks like itself.
6. **Prompt queue and screens** — Done / Skip / Delay-all / X, tile-tap logging, physical key.
7. **Sound and haptics** — cue vocabulary, escalation, motor PWM.
8. **Guided stretch flow.**
9. **HTTP server** — `/state`, `/event`, `/ws`, mDNS. The board is now complete and usable on its own.
10. **Web app** — `derive` in TS against the shared fixtures, the reviewed grid UI, `resilient` backend, served from SPIFFS.
11. **Power states** — light sleep, brightness, PMIC events, Dormant.
12. **notifi.it push** with absence gating.
13. **Settings screen** — working hours, per-action cadence, sound and haptics, on-device.

Steps 1–9 are a finished product. Everything after is reach.

### 11.1 Test targets

Steps 3 and 4 produce the bugs nobody notices for a week, and both are pure functions of `(log, now)`. Cover them via the §3.3 fixtures, which the C and TS suites share:

```
fixtures/derive/
  interval-done-early.json          re-anchors the next slot from the tap
  fixed-done-early.json             13:00 lunch stays 13:00
  missed-while-awake.json           unanswered + inside a window = missed
  missed-while-asleep.json          unanswered + gap = NOT missed
  awake-gap-boundary.json           119s extends, 121s opens a new window
  duplicate-event.json              second apply is a no-op
  merge-commutes.json               merge(a,b) and merge(b,a) derive identically
  stretch-priority.json             stretches take the queue head
  dst-forward.json                  no slots lost or doubled on the changeover
```

The last four are the ones worth having. Idempotence under replay and commutativity of merge are what the whole board-plus-browser story rests on, and DST is the bug that will otherwise appear twice a year and be impossible to reproduce.

### 11.2 Day rollover

Handled in the tick (§5.2), not on boot: the board runs for weeks at a time, so midnight is the common case rather than an edge case. There is no "reset the timers" step — `next` derives from an empty log against today's date, so a fresh day is literally an empty struct.

---

## 12. Known constraints

- **Single point of failure.** The board is the product. If it is off the LAN, the web app is a read-only cache; if it is dead, there is no tracker. Accepted deliberately — the alternative is two schedulers.
- **Firmware dev loop.** Flash-and-test is slower than a browser reload. §3.3's fixtures and a host-compiled unit test target for `derive` and the slot maths take most of the sting out; build those early.
- **LAN-only.** `/state` and `/event` are unauthenticated on the local network. Fine for a home LAN, not fine on a shared or office network — if that changes, put a shared secret in a header before exposing it further. Away-from-home reach is push-only, via §9.2.
- **Two `derive` implementations** must stay in agreement. Mitigated by shared fixtures, not eliminated.
- **No cross-device sync beyond the board.** The board is the only writer of record; two browsers merge through it, not with each other.
- **Clock.** NTP at boot when WiFi is available, PCF85063 otherwise. A board that has never seen NTP and has a flat backup cell will have a wrong date, and the day key will be wrong with it. Show the date in the header so this is visible rather than silent.

What is explicitly *not* a constraint, thanks to §3: a board that slept, ran flat, or was unplugged does not accumulate misses. It can be switched off freely without the day's record becoming a wall of hollow rings.

---

## 13. Storage: SQLite?

Considered and deferred. It is a good fit for the shape the data took in §3, and a poor fit for the volume — so the answer depends entirely on whether historical views arrive.

### 13.1 Why it fits the model well

§3 made the store an append-only log of uniquely-keyed rows. That is a table, and the idempotence that `day_apply_event` enforces with a linear scan becomes a schema constraint — strictly better, since the database refuses the duplicate whatever its source:

```sql
CREATE TABLE events (
  id     TEXT PRIMARY KEY,
  day    TEXT NOT NULL,
  action TEXT NOT NULL,
  kind   TEXT NOT NULL CHECK (kind IN ('done', 'skip')),
  ts     INTEGER NOT NULL,
  slot   INTEGER NOT NULL,
  source TEXT NOT NULL DEFAULT 'board',
  UNIQUE (action, slot)                  -- the dedupe key, enforced
);
CREATE INDEX events_day ON events (day);

CREATE TABLE awake (
  day   TEXT NOT NULL,
  start INTEGER NOT NULL,
  fin   INTEGER NOT NULL,
  PRIMARY KEY (day, start)
);
```

`INSERT OR IGNORE` becomes the whole of `day_apply_event`, and counts become one `GROUP BY` instead of a walk.

**But `missed` and `next` do not translate cleanly.** Both need the slot walk, and §5.3's early-completion rule means slot *n+1* depends on the event that answered slot *n*. In SQL that is a recursive CTE joining each generated slot back against `events` — correct, and worse than the code it replaces: harder to read, harder to test, and parameterised per action because the cadence lives in config. **Keep `derive` in code regardless of store.** Database for facts and aggregates, not for scheduling semantics.

### 13.2 Not yet

On the board it is viable now: the SQLite3 port runs on ESP32 over a FATFS or LittleFS partition, and 8MB of PSRAM comfortably covers the page cache. At ~30 writes a day flash wear is not a consideration. It costs a few hundred KB of flash and a filesystem partition, against NVS blobs at ~4KB a day with no dependency at all.

For a v1 scoped to "today at a glance", that is a dependency bought for nothing.

### 13.3 When it becomes the right call

Two triggers, either of which flips it:

1. **Historical views.** "Water over the last 30 days", streaks, weekday-vs-Friday. Hand-rolled aggregation over per-day NVS blobs stops being pleasant immediately and SQL starts paying for itself.
2. **The board as archive.** 16MB of flash holds years of this. `GET /history?from=&to=` backed by a real query is a far better API than shipping a year of blobs to the browser to reduce client-side.

Two rules keep the door open, and §3 already did most of the work by making the store an append-only log behind one function:

- **Never read the derived view out of the database.** Facts in, `derive` on top. The moment a query returns `missed` directly, the board and the browser can disagree about what a miss is and §3's guarantee is gone.
- **Migration is a replay, not a conversion.** Read the NVS blobs, `INSERT OR IGNORE` every event, done. Idempotent, re-runnable, safe to abandon halfway.

**Decision: NVS blobs for v1. Revisit at the first historical view.**
