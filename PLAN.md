# WFH Health Tracker — Implementation Plan

Status: UX prototype built and reviewed. This document is the spec for the real build.

---

## 1. Scope

A square-format habit tracker that prompts seven actions on independent timers during working hours, chimes and vibrates, and logs one tap per completion. Today-at-a-glance only, no historical views in v1.

**A standalone desk object with a debug port. Nothing else.**

Hardware: Waveshare ESP32-S3-Touch-AMOLED-1.8 (368x448, ESP32-S3R8, 8MB PSRAM, 16MB flash, QMI8658 IMU, PCF85063 RTC, AXP2101 PMIC, ES8311 codec + NS4150B amp into an onboard 8Ω/1W speaker, TCA9554 I/O expander, battery connector — no cell included).

**The board ships in two revisions and the difference is not cosmetic:** V1 pairs the SH8601 display controller with FT3168 touch; V2 (shipping from May 30, 2026) pairs CO5300 with CST820. Display bring-up and the touch driver both change with it. Check the label on the back before writing a line of driver code; everything schematic-derived in this plan (§7.4, §9.1, §10) was verified against V1.

The board owns everything: the schedule, the clock, the event log, the prompting, the sound, and the only screen. There is no companion app, no browser client, no second way to log anything.

**This was cut down twice, and both cuts matter.** The plan began as a browser app with the board as a later phase; collapsing that into one board-authoritative build removed a throwaway browser scheduler. But the web app *survived* that collapse unexamined, demoted to "a viewer and a remote," and grew an HTTP server, a WebSocket, a client class and a frontend architecture before anyone re-asked whether it should exist. It shouldn't. The justifications didn't survive contact:

- **Settings** — changed once at setup, maybe a few times after. `actions.json` plus a reflash covers it, and §2.1 already generates the config.
- **Logs** — §10 recommends permanent USB-C power, so the cable is already attached. Serial already gives you this.
- **History** — real, but §13 defers it to v2 regardless.

What makes a future web version cheap is the event log in §3 and the snapshot shape in §8 being clean, not a React app existing now. Build it when you know what you want it to do.

The board sits on the desk and prompts the person sitting at it. It does not chase them elsewhere: no phone push, no notifications away from the desk. If you are not there, you are not there.

**Explicitly out of scope:** eye breaks (20-20-20), the lunchtime walk, any "silence for the day" control, any companion client, and the end-of-day shut-down prompt (cut after seeing it on the board — it was the one tile that never told you anything you did not already know). No historical views in v1 — see §13.

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

Working hours 09:00–18:00. No prompts outside the window, save a 30-minute grace past `workEnd` that exists so the 18:00 shut-down card can be raised and answered (§5.2). Timers reset at 09:00 the next day.

**Stretch sequence:** chin tucks (30s), doorway pec stretch (40s), cat-cow (40s), hip flexor stretch (50s).

### 2.1 Config as data, not literals

The action table is authored once as JSON and generated into the firmware, so changing a cadence is a config edit and a reflash rather than hunting literals through C:

```json
// config/actions.json — the only place this table is edited
{
  "workStart": "09:00",
  "workEnd": "18:00",
  "tz": "GMT0BST,M3.5.0/1,M10.5.0",
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
```

The generated header is committed and checked in CI (`gen-config && git diff --exit-code`), so a hand-edit of a generated file fails the build rather than silently surviving.

This is also the settings mechanism. There is no settings screen and no settings client: working hours and cadences change here, then you reflash. For something touched a handful of times ever, that beats designing a config UI for a 368px panel.

**No jitter.** An earlier draft staggered start offsets (stand at 09:12, water at 09:07, …) specifically to keep the three interval actions from lining up. That was solving the wrong problem: collisions are fine now that the card shows everything due at once as a checklist (§6) rather than answering one at a time. All three actions anchor to `workStart` and simply collide when they collide.

---

## 3. Data model

What the board stores and what it derives. Facts in the database, meaning computed on top — never the other way round.

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
    { "id": "…", "action": "water", "kind": "done", "ts": 1723645210, "slot": 1723645020 },
    { "id": "…", "action": "stand", "kind": "skip", "ts": 1723646400, "slot": 1723646400 }
  ],
  "snoozedUntil": { "stand": 1723647300 }
}
```

```ts
// Shapes in TypeScript for readability only — the one implementation is C
// (log_event_t / day_log_t / derive, §3.3). A future client codes against
// these through GET /state (§8.2); no web/ directory exists now.
export type EventKind = 'done' | 'skip';        // no 'miss' — see above

export interface LogEvent {
  id: string;          // uuid v4
  action: ActionId;
  kind: EventKind;
  ts: number;          // epoch seconds — when the user tapped
  slot: number;        // epoch seconds — which slot this answers; dedupe key
}

/** One day as the board tracks it. `events` is the persisted part (§3.4);
 *  `snoozedUntil` is RAM-only — the schema has no row for it, deliberately.
 *  A reboot mid-snooze forgets the snooze and the card just comes back,
 *  which is the right failure for a 15-minute deferral. */
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

`slot` is the field that makes the whole thing idempotent. A double-tap on Done, a Confirm that includes an already-answered row, a replayed event — all collapse to one entry because `(action, slot)` is unique. It is also the join key between the schedule and the log, which is what lets a miss be derived at all.

There is no `source` field. With the board as the only writer there is nothing to attribute.

### 3.1a Timestamps: epoch everywhere except display

Stated as a rule rather than left implicit: **every timestamp that is stored, transmitted, or compared is an epoch integer.** `ts`, `slot`, `snoozedUntil` values, the `now` field in the API snapshot — all epoch seconds, UTC-equivalent, no timezone attached because none is needed.

The one exception is `cadence.times` in `actions.json` (`"11:30"`, `"16:30"`, …) — a wall-clock string, because that's the natural way for a human to author a schedule. It is converted to an epoch value immediately by `at_time()` (§5.1) and never travels as a string past that point; nothing downstream of config ever parses a time-of-day string again.

Local wall-clock time reappears exactly once more: at render, when a timestamp becomes "4m" on a tile or "14:32" in the header. That conversion happens in the UI layer only and is never fed back into the model.

**Why this matters for timezones.** All slot arithmetic happens in the board's own local time (§5.1, via `mktime`/`localtime_r` against a `TZ` set at boot). The `TZ` value is the `tz` field of §2.1's config, and it must be a **POSIX rule string** (`"GMT0BST,M3.5.0/1,M10.5.0"`) — ESP32's newlib has no zoneinfo database, so an IANA name like `Europe/London` means nothing on the board. The board's clock is the only clock in the system — there is no second device whose timezone could disagree. The one real gap is the board itself: nothing auto-detects a new timezone if it physically moves (no GPS), so a relocated board needs its `TZ` updated and reflashed.

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
        // Unanswered and in the past: due while its window is open, missed
        // once it closes. A slot's window runs from its time until the
        // action's next slot (or workEnd + the §5.2 grace, for the last
        // one); snooze defers due-ness within the window.
        if (isDueNow(def, slot, now, log.snoozedUntil[def.id])) view.due.push(def.id);
        else view.missed[def.id] = (view.missed[def.id] ?? 0) + 1;
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

`isDueNow` is §3.1's rule made precise: *unanswered and in the past* splits into *due* (window still open) and *missed* (window closed). Without the split, a prompt would count as a miss the second it fired — the hollow ring appearing while the card is still asking — and §9's "goes quiet when the next slot arrives" would contradict the dots. The window needs no storage; it falls out of the same slot walk.

**Open question, surfaced by the fixtures.** Deriving the window from the *next slot* is right for the interval actions — a stand prompt gives up after 40 minutes — but the fixed ones have enormous gaps, so an unanswered 10:45 snack stays on the card until 15:30, and an unanswered 11:30 stretch until 16:30. `stretch-priority.json` (§11.1) asserts exactly that today, which is how it was noticed. Two ways to go: accept it (the card is the outstanding list, and a snack you never ate *is* outstanding), or cap the window at some minutes-open constant. The cap is a tunable, and this plan has spent two revisions deleting tunables (§3.1), so the default stands — but it stands as a decision, not an oversight. Change the fixture with it if it changes.

### 3.3 `derive` runs once, on the board

Two earlier drafts got this wrong in opposite directions: one implemented `derive` twice (C for the board, TypeScript for a browser client) with shared fixtures to keep them honest; the next kept one implementation but shipped its output to that client.

With no client, this is simply the board computing its own view for its own screen. `GET /state` (§8) serves the derived view too, but nothing consumes it yet — it is there so the endpoint is useful for debugging and so a future client has a contract to build against.

The fixtures in §11.1 still earn their place for the one implementation that exists.

### 3.4 Persistence — one append-only file per day

**This section used to specify SQLite. Spike S2 (§15.3) measured it on the board and it failed, so R1's kill switch fired.** The numbers, 15,000 inserts on the real hardware:

| | SQLite on LittleFS | one file per day |
|---|---|---|
| typical insert (p50) | ~3,060ms at 688 rows | **18.9ms** |
| mean | — | 51.2ms |
| p99 | — | 558ms |
| worst | — | 1,234ms |
| trend with row count | degrading, never reached 1k | **flat from 3k to 15k** |
| WAL available | **no** | n/a |
| flash cost | +265KB | 0 |

Two things killed it. The insert cost was 60× the budget before the database was a tenth of its retention size, and — the part that removed the argument entirely — **`PRAGMA journal_mode=WAL` returns no row on this port**, so the crash-safety that §13 called "the other quiet win" was never going to be there. A B-tree updating pages in the middle of a file is close to the worst thing you can ask of a copy-on-write filesystem on raw flash.

**What replaces it.** One append-only file per day, `\t`-separated, one line per event:

```
/fs/d/2026-08-13
1723645210  1723645020  water  done  3f9a1c04d1e88b02
1723646400  1723646400  stand  skip  8c2b77e0a45f1993
```

`<ts>\t<slot>\t<action>\t<kind>\t<uuid>`. Plain text, greppable, and `cat`-able over the §8 debug endpoint. TSV rather than the JSON the kill switch originally sketched: identical properties, one `fprintf` to write, one `sscanf` to read, and no JSON parser in the firmware.

**Appending is the one thing LittleFS is genuinely good at**, and the design leans on exactly that. Durability comes from `fflush` + `fsync` after each line, which is what `synchronous=FULL` was buying.

**The open day stays open.** This is the difference between the flat line above and a slow crawl, and it was measured rather than assumed: opening a file costs a linear scan of the directory, so with 400 days retained an open-per-write makes every tap pay for every day ever recorded. Holding the handle and the day's events in RAM turned p50 from ~90ms-and-climbing into 24ms-and-flat.

```c
// firmware/main/store_files.c
static char      g_open_day[11];
static FILE     *g_fp;
static day_log_t g_cache;      // the same in-RAM day §4 already wanted

bool store_add_event(const log_event_t *ev, const settings_t *s) {
    const char *day = store_day_key(ev->ts);
    if (!open_day(day, s)) return false;       // reloads + reopens only on rollover

    // UNIQUE (action, slot) moves from the schema to here. It is the one
    // thing genuinely lost with SQLite, so it is the one thing the tests
    // have to keep honest.
    for (int i = 0; i < g_cache.events_len; i++) {
        if (g_cache.events[i].action == ev->action && g_cache.events[i].slot == ev->slot) return false;
    }

    fprintf(g_fp, "%lld\t%lld\t%s\t%s\t%08lx%08lx\n", …);
    fflush(g_fp);
    fsync(fileno(g_fp));
    g_cache.events[g_cache.events_len++] = *ev;
    return true;
}
```

**Retention** is `unlink` on any file whose name sorts before the cutoff — ISO dates sort lexically, so the comparison is `strcmp`. Measured at 1.9s for a 400-day sweep, which is a once-per-boot cost and invisible.

**The honest caveat: p99 is 558ms and the worst case is 1.2s.** That is flash garbage collection, it is inherent, and it does not grow with the data. At ~30 writes a day it means a sub-second hitch roughly every few days. If that ever shows up as a felt problem the answer is to move the write off the UI path — but §4's guarantee is that the screen reflects what was *stored*, so that is a real trade to make deliberately, not a tweak. **Not decided here.**

**What this costs.** No SQL, so §13's historical queries become a script over pulled files rather than a query on the device. The event log itself is unchanged — §3.1's model was never relational, it was always an append-only log, which is exactly what it is now stored as.


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
                          ┌──────────┴──────────┐
                          ▼                     ▼
                    ┌──────────┐         ┌────────────┐
                    │  SQLite  │         │ derive()   │
                    │ /fs/wfh  │         │ → DayView  │
                    └────┬─────┘         └────────────┘
                         │ read-only
                   ┌─────┴──────┐
                   │   httpd    │  GET /state, GET /logs  (debug, §8)
                   └────────────┘
```

`day_apply_event` is the only function that writes the log. Every input path in §7.4 funnels through it, and the HTTP surface (§8) is read-only, so nothing else can.

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
    return true;
}
```

Two properties this gives everything upstream of it:

- **A duplicate is not an error.** Tapping Done twice, or confirming a card whose row was already answered from a tile tap, gets a quiet no-op rather than a failure or a double count.
- **The screen always reflects what was stored.** The redraw is driven by a `derive` over the freshly-reloaded log, not by patching the view in place, so the tiles cannot show something the database does not contain.

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

        // Prompt window, not working hours: workStart ≤ now < workEnd + 30min.
        // Slots stop generating at workEnd (§5.1), but the 18:00 shutdown slot
        // *is* workEnd — gate on the hours alone and its card either never
        // syncs or lives for exactly one tick. The grace lets that last card
        // be raised and answered; it can create no new slots.
        if (in_prompt_window(now, &g_settings)) {
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

**Built and tested** — `firmware/main/card.c`, with the nine scenarios in `firmware/test/test_card.c`. Every side effect goes through a `card_host_t` of function pointers (logging, screen changes, the stretch hand-off), which is what lets the whole state machine run on a laptop; the firmware wires those to `input_done`/`input_skip` and LVGL, the test wires them to a recorder.

Writing it turned up two bugs in this section's earlier pseudocode, both now fixed in the implementation:

- **The card redrew every tick.** `card_sync` ended with `if (g_card_len > 0) ui_show_card();`, and `ui_show_card` (§7.3) cleans the screen, rebuilds every row and re-runs a load animation. At 1 Hz that wipes a half-made decision once a second — the exact thing `card_sync` exists to protect. It now returns whether the row set actually changed and only redraws on a change.
- **The stretch was pushed as a row, then handled.** §6 says stretches never join the checklist, but the code added every due action first and only afterwards checked whether one of them was a stretch. A stretch-flow action is now skipped when rows are built, so it can never be checked, confirmed, or counted as a row.

The scenarios were mutation-checked the same way as `derive`: broken row lookup, Confirm taking unchecked rows, Delay-all compounding, Delay-all ignoring unchecked rows, the stretch joining the list, redrawing every tick, and the key answering the wrong end of the list. All seven caught.

`card_sync` running every tick — rather than the card being built once when it opens — is what makes it correct to leave the board mid-decision: check two boxes, get pulled away, come back an hour later, and the card still reflects exactly what's actually due, with your two checks intact and nothing double-logged.

**The physical key stays single-item.** Checkboxes are a touch affordance; the key's job (§7.4) is answering *something* without looking at the screen, which doesn't compose with "which boxes did I check." A key tap logs `done` for the single highest-priority due action directly, through the same `input_done` as everything else, regardless of what is or isn't checked on the card.

**One edge to be aware of.** A snoozed slot that passes unanswered still derives as a miss, because `derive` only sees the *current* `snoozed_until`, not that the slot was snoozed at the time. With the shortest cadence at 40 minutes a 15-minute delay cannot span a slot boundary, so this is unreachable as configured. It becomes reachable if §11's settings screen lets a cadence go below ~15 minutes — at which point the fix is to log the snooze as an event carrying the slot it covered, not to special-case `derive`. Floor the configurable cadence at 20 minutes.

---

## 7. Board UI

368x448 AMOLED, LVGL — pin the major version in `idf_component.yml`; v9 renamed `lv_btn_*`/`lv_scr_*` to `lv_button_*`/`lv_screen_*`, and the snippets here use the v8 names. A 368x368 square carries the grid; the remaining 80px is a header with the clock, the date, a battery pip and the sound icon.

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

- 6 tiles, 2 columns, 3 even rows at 140px — no header bar, so the grid owns all 448px
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
| Missed | hollow ring | due window closed with no response (§3.2) |
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

Four ways to answer, and **all four end at the same two functions**. Nothing else in the firmware writes an event — and with no client, nothing outside the firmware can.

```
touch: card row + Confirm ─┐
touch: grid tile          ─┤
physical key              ─┼──► input_done(action)  ──► day_apply_event()
IMU tap (optional)        ─┘
```

```c
// firmware/main/input.c — every input path lands here

void input_done(action_id_t id) {
    log_event_t ev = {
        .action = id,
        .kind   = KIND_DONE,
        .ts     = time(NULL),                  // RTC-backed; right after a reboot
        .slot   = current_or_next_slot(id),    // which slot this answers
    };
    uuid_v4(ev.id);

    if (day_apply_event(&ev)) {                // false = we already had it
        feedback_play(CUE_SUCCESS);            // sound only — §9
    }
    idle_timer_reset();                        // screen goes dark 20s from now
}

void input_skip(action_id_t id) {
    log_event_t ev = { .action = id, .kind = KIND_SKIP, .ts = time(NULL),
                       .slot = current_or_next_slot(id) };
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
| **PWR** | `EXIO4` on the I/O expander | Wired to AXP2101 `PWRON`, mirrored onto `EXIO4` through a FET. Long-press power-off belongs to the PMIC (default ~6s, register-set). Not a native GPIO. |

**Use BOOT as the user key.** PWR is disqualified on two counts: it is behind an I²C expander rather than a native pin, so it cannot serve as a deep-sleep wake source and cannot be polled without an I²C transaction; and its long-press is owned by the PMIC as a hardware power-off, so the 6s gesture is not ours to redefine.

BOOT being a strapping pin is worth knowing but not disqualifying: it only matters at power-on, and at runtime `GPIO0` is an ordinary readable input. The practical consequence is one to remember at the bench — resting a finger on the key while plugging in USB drops the board into download mode instead of booting the app.

`GPIO0` is RTC-capable on the ESP32-S3, which is what makes the `ext1` wake in §10 work.

One press logs Done for the single highest-priority due action, which is what you want when the prompt is "stand up" and you are already standing — you can answer it without looking, independent of whatever is or isn't checked on the card (§6).

```c
// firmware/main/key.c
#define KEY_GPIO        GPIO_NUM_0     // BOOT. The only usable key — see above.
#define DEBOUNCE_US     30000          // 30ms: a real tap runs ~50–150ms, so a
                                       // longer floor would eat quick presses
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
}
```

**The confirmation is visual, not haptic.** An earlier draft added a vibration motor specifically to confirm the mute-off case, on the reasoning that muting can't confirm itself with sound. **Removed** — this board does not ship with a motor, and adding one is a real hardware change (BOM, wiring, enclosure) that shouldn't be pulled in for one gesture. The icon's visual pulse (§7.5), already built for exactly this purpose, carries it instead: switching sound off dims and swaps the icon and pulses it once; switching on does the same plus a chirp, since by then you can hear it again. The one honest tradeoff: unlike a haptic, the visual confirmation only lands if you're looking at the board when you release the key — acceptable, since the icon's steady state (§7.5) tells you the same thing at a glance a moment later regardless.

**What this costs.** The key previously carried `delay all 15`; sound now owns the hold. Delay-all is touch-only, which is the right trade — muting is the gesture you want blind and in a hurry, and delay-all is one you make while already looking at a card. If you want it back on the key later, a double-tap is free and unambiguous next to a hold.

The wake-then-answer split still applies to the short press: a single tap that both lights the panel and logs a completion means every accidental brush marks water as drunk. First tap wakes, second commits. Acting on *release* rather than press is what lets one button carry both gestures.

The table above is verified against the V1 schematic — Key1 → `GPIO0` with a 10K pull-up, Key3 → `PWRON` with the FET mirror onto `EXIO4`. V2 swaps the display and touch parts (CO5300, CST820 — §1); re-check its schematic before assuming the buttons carried over unchanged.

**IMU tap (optional, default off).** The QMI8658 has tap detection, so a knock on the desk beside the board could answer the current prompt without reaching for it — genuinely nice for "stand break", since you are already moving.

```c
// No ISR on this board: QMI8658 INT1 lands on the expander (EXIO6), and the
// expander's own INT line is not routed to the ESP — so tap detection is an
// I2C poll of the QMI's tap-status register, piggybacked on the key task's
// 50Hz loop.
static void imu_poll_tap(void) {
    // Only ever answers something already on the card. A tap must never
    // be able to log something the user was not being asked about — the
    // false-positive rate on desk knocks is far too high for that.
    if (g_card_len == 0 || !g_settings.imu_tap) return;
    if (qmi8658_tap_detected()) input_done(g_card[0].action);
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

Two ways in, one function out — the key hold (§7.4) and this tap both call `input_toggle_sound`, exactly as the four completion paths all call `input_done`.

The pulse is what replaces the toast. A hold on the key can happen while you are looking at the panel or not, so the icon animates on change rather than sitting there statically — enough to catch the eye if you are watching, invisible if you are not, and no text to read either way.

**Icon, not switch.** A toggle switch would need a label to say what it toggles and would take three times the header width. A speaker glyph reads instantly at 24px and is the same idiom every phone uses.

### 7.6 Stretch flow

Replaces the prompt screen. One stretch per screen with a countdown ring, auto-advancing on completion, Next to advance early, Skip to abandon the set.

Derive the remaining time from an absolute end timestamp rather than decrementing a counter, so a delayed render resumes at the correct point instead of stretching a 40-second hold into a minute.

Abandoning the set part-way logs a `skip`, not a partial `done` — a half-finished stretch set is a skipped stretch set, and the dot should say so.

---

## 8. Debug endpoints

Not an app. Two read-only endpoints you `curl` when something looks wrong, and NTP. That is the entire justification for the radio.

| | | |
|---|---|---|
| `GET /state` | → `{ log, view, power, settings, now }` | what the board thinks is true |
| `GET /logs` | → last 24h of log lines, `text/plain` | why it rebooted at 3am, §14 |

No `POST`. **The board is the only writer of events** — there is no second client, so there is no cross-writer reconciliation, no `source` attribution, and no clock-skew problem. Everything §3 says about `(action, slot)` uniqueness still holds, but now it only has to survive a double-tap on the panel rather than two devices racing.

```c
// firmware/main/api.c — the whole HTTP surface
static esp_err_t state_get(httpd_req_t *req) {
    char *json = snapshot_json();          // log + derived view + power + now
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

void api_start(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(httpd_start(&g_server, &cfg));

    httpd_uri_t routes[] = {
        { .uri = "/state", .method = HTTP_GET, .handler = state_get },
        { .uri = "/logs",  .method = HTTP_GET, .handler = logs_get  },   // §14
    };
    for (int i = 0; i < 2; i++) httpd_register_uri_handler(g_server, &routes[i]);

    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("wfh"));            // curl http://wfh.local/state
}
```

**The snapshot shape is worth keeping honest even though nothing consumes it yet.** It is the contract a future web version would implement against, and it costs nothing to serialise correctly now:

```jsonc
{
  "log":  { "version": 3, "date": "2026-08-14", "events": [ … ] },
  "view": { "counts": {…}, "skipped": {…}, "missed": {…}, "next": {…}, "due": [ … ] },
  "power":    { "battPct": 82, "charging": true, "onBattery": false },
  "settings": { "sound": true, "volume": 70 },
  "now": 1723645200
}
```

### 8.1 Why WiFi survives the cut

With no client, the radio earns its place on one job: **NTP**. §12 flags that a board which has never seen network time — and whose RTC backup cell is flat — will have a wrong date, and a wrong date silently writes events under the wrong day key. That is the one failure here that corrupts data rather than merely annoying you.

Everything else WiFi was carrying is gone: no static file serving from flash, no WebSocket, no mDNS-for-discovery (the hostname is now just a convenience for typing `curl`), no mixed-content constraint, no client socket table.

This also simplifies §10 — Idle previously kept WiFi responsive so a browser's polling never failed. With nothing polling, Idle can drop the radio between NTP syncs and wake it on a schedule.

### 8.2 If a web version ever happens

Build it then, against `GET /state` as it already exists. What makes that cheap is §3's event log and the snapshot above — both designed to be transmitted, neither dependent on a client existing. Adding a `POST /event` and a push socket later is a contained change; having built a frontend now against requirements nobody has articulated would not have made it cheaper.

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

The chain is I2S → ES8311 codec → NS4150B amp → the onboard 8Ω/1W speaker. Rather than shipping audio files, synthesise the tones — a sine with a short attack and an exponential decay, which is all these cues are.

Pins, from the V1 schematic: MCLK `GPIO16`, BCLK `GPIO9`, LRCK `GPIO45`, data out `GPIO8` (the mic comes back on `GPIO10`, unused here). The codec needs an I2C init before it makes a sound — use `esp_codec_dev` rather than hand-rolling ES8311 registers — and the amp is gated by `PA_CTRL` on `GPIO46`: high to play, low in Idle so a silent board is actually silent.

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

*Note:* `bloom` is soft and may not cut through when heads-down. Test against a brighter `chime` before locking it in, **and test it on the board's actual speaker**, which is small and will not reproduce the low end the way a laptop does. The cue that works in a desktop prototype is not necessarily the cue that works here.

**Escalate rather than repeat.** If a prompt goes unanswered for two minutes, re-cue once at higher volume. If it is still unanswered when the next slot arrives it becomes a derived miss and the board goes quiet. It must never nag in a loop.

```c
// In the scheduler tick. One-shot per card, `>=` not `==` so one skipped
// tick cannot skip the escalation. The boost rides inside the queued
// message: audio_play is asynchronous, so bumping a global volume and
// restoring it around the call would race the audio task and mostly play
// the re-cue at normal volume anyway.
if (g_card_len > 0 && !g_card_escalated && now - g_card_raised_at >= 120) {
    g_card_escalated = true;               // cleared when the card empties
    feedback_play_boost(CUE_BLOOM, 15);    // +15 volume, clamped to 100
}
```

---

## 10. Power

The board is **not** always on. On USB-C it effectively is; on battery it is not, and the design has to say what happens instead.

**Rough numbers.** The 1.8" AMOLED dominates — call it 80–150mA lit depending on how much of the panel is emitting, against maybe 40–50mA for the ESP32-S3 with WiFi associated and the screen off. A 500mAh-class cell is therefore a handful of hours screen-on and most of a day screen-off. Measure at the PMIC rather than trusting this paragraph.

So **screen-on time is the budget, not uptime** — which suits an app that is idle 99% of the time: seven prompts an hour at most, each needing a few seconds of attention.

| State | Trigger | Wakes on | Screen | WiFi | Serves HTTP |
|---|---|---|---|---|---|
| Active | prompt firing, or touch within 20s | — | on | on | yes |
| Idle | no touch for 20s, inside working hours | any touch or key press | off | modem-sleep | yes, if awake |
| Dormant | outside the prompt window (§5.2) | RTC timer (next `workStart`) or key press | off | off | no |

**Idle is the important one**, and it got cheaper when the client went away. Nothing is polling the board, so the radio does not need to stay responsive — it only has to wake for NTP occasionally. Screen off is what matters, since the panel is the actual cost. A touch or key press takes it straight back to Active, same path as the wake sources listed for Dormant below.

```c
esp_pm_config_t pm = {
    .max_freq_mhz = 240,
    .min_freq_mhz = 40,
    .light_sleep_enable = true,       // wakes on WiFi, timer, or GPIO
};
ESP_ERROR_CHECK(esp_pm_configure(&pm));
esp_wifi_set_ps(WIFI_PS_MAX_MODEM);   // wake only on DTIM beacons
```

**Deep sleep is still the wrong tool for Idle**, but for a plainer reason than before: it loses RAM state and costs a full boot, and Idle can happen dozens of times an hour. Use it only for Dormant, where the next event is hours away.

```c
static void enter_dormant(time_t wake_at) {
    if (wake_at > time(NULL)) {           // guard: a past/zero wake_at would
        uint64_t us = (uint64_t)(wake_at - time(NULL)) * 1000000ULL;
        esp_sleep_enable_timer_wakeup(us);// underflow into a garbage
    }                                     // multi-year timer
    // BOOT/GPIO0 is RTC-capable, so it can wake us (ext1; the _io variant on
    // newer IDF). PWR cannot — it reaches the ESP only through the expander,
    // which has no line to an RTC GPIO.
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
        sqlite3_close(g_db);                      // checkpoint WAL first
        pmic_power_off();                         // AXP2101 register power-off
        break;
    }
}
```

Closing the database connection on the critical event checkpoints the WAL, so the next boot opens a clean database rather than replaying a journal.

Two wiring facts (V1 schematic) shape this code. First, `on_power_event` is fed by an I2C poll of the AXP2101 status registers on the 1 Hz tick, not by a GPIO interrupt — the PMIC's IRQ pin lands on the expander (`EXIO5`) and the expander's INT line goes nowhere. Second, that same fact is why critical-battery is a **PMIC power-off, not deep sleep**: nothing can wake a deep-sleeping ESP when USB power returns, so a board that deep-slept at 5% would sit dead-looking on a charger until someone pressed the key. A register power-off cuts the rails for microamps, and the AXP2101 powers back on from exactly the right triggers — a PWR press or USB insertion (confirm the power-on-source config at bring-up).

Battery goes in `/state` (§8) so it is answerable over `curl`, and on the header pip so it is answerable at a glance:

```c
cJSON_AddNumberToObject(root, "battPct",   axp2101_get_batt_percent());
cJSON_AddBoolToObject(root,   "charging",  axp2101_is_charging());
cJSON_AddBoolToObject(root,   "onBattery", g_on_battery);
```

**Recommendation: run it on USB-C.** It sits on a desk next to a laptop; a permanently-connected cable is not a hardship and it removes the question entirely. The battery then covers a power cut and the occasional unplugging rather than being the operating mode — and the design above means neither corrupts the day's record.

---

## 11. Build order

Firmware first, because it is the product and it is the long pole. Each step should end somewhere you can leave it. The three §15.3 spikes run before step 1 — a day and a half that decides whether §3.4 and §10 survive contact with the hardware.

0. **The design loop** — `design/` renders board screens at 368×448 and screenshots them headlessly (see `design/README.md`). Needs no hardware and no ESP-IDF, so design iteration is never blocked behind a toolchain. Settle what the screens look like here; §7 transcribes the result.
1. **Board bring-up** — ESP-IDF project, display, touch, LVGL hello-world, WiFi, NTP-set RTC. Confirms the hardware and the toolchain before any product logic exists. First act: read the revision label (§1) and run the vendor demo that matches it — V1 and V2 take different display and touch drivers.
2. **Config generation** — `actions.json` → `actions.g.h`, wired into the build with the CI diff check.
3. **Storage** ✅ — LittleFS partition, `store_files.c`, `store_add_event` / `store_load_day`, benchmarked on the board (§3.4). `day_apply_event` still to come on top of it.
4. **`derive`** ✅ — the walk, the anchor rule, the due window, and the §11.1 fixtures, all host-built and mutation-checked. `day_apply_event` still to come, on top of step 3. **This was the step to get right**; everything else is presentation.
5. **Scheduler tick and the checklist card** (§6) — card logic ✅ (`card.c`, host-tested and mutation-checked); the 1 Hz tick that drives it lands with bring-up, and is testable on-desk by moving the RTC forward.
6. **Grid UI** — tiles, dots, wash, header, the shared X component (§7.3a). First point at which the thing looks like itself.
7. **Inputs** — card rows, tile-tap logging, physical key (§7.4).
8. **Sound** — the cue table, the I2S tone task, escalation (§9).
9. **Guided stretch flow.**
10. **WiFi + NTP + debug endpoints** — `GET /state`, `GET /logs` (§8, §14). Small, and the NTP half is the part that matters.
11. **Power states** — light sleep, brightness, PMIC events, Dormant.

Steps 1–9 are a finished product; 10 and 11 make it a good one to live with. There is no step for a settings screen or a client — settings are `actions.json` plus a reflash (§2.1), and there is no client.

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
  fixed-done-early.json             13:00 lunch stays 13:00, and does not loop
  unanswered-past-slot.json         window open = due, window closed = missed
  snoozed-not-due.json              snoozed inside the window is neither
  duplicate-event.json              a twice-logged answer still counts once
  replay-idempotent.json            the doubled log derives identically
  card-confirm-partial.json         confirming checked rows leaves unchecked ones due
  stretch-priority.json             a due stretch pre-empts the checklist
  dst-forward.json                  BST day: slots hold their wall-clock time
  dst-day-before.json               the GMT control for the pair
```

Card-confirm-partial is the case that would silently regress if `card_confirm` and `derive` ever disagree about which rows are still open, idempotence under replay is what makes a double-tap on Confirm harmless, and DST is the bug that will otherwise appear twice a year and be impossible to reproduce.

The DST pair only works because its expected epochs are hand-computed constants rather than recorded output — lunch is 82,800s apart across the changeover, not 86,400s, and an implementation doing UTC arithmetic by hand passes one fixture and fails the other. `fixtures/derive/README.md` states that rule and the assertion semantics.

**A suite that passes on its first run has not been shown to work.** These were verified by mutation — the anchor rule dropped, the due window removed, `tm_isdst` forced to 0, the sort made unstable, snooze ignored — and each one is caught. Re-run that check after any change to the walk; the table in the fixtures README lists the mutations.

### 11.2 Day rollover

Handled in the tick (§5.2), not on boot: the board runs for weeks at a time, so midnight is the common case rather than an edge case. There is no "reset the timers" step — `next` derives from an empty log against today's date, so a fresh day is literally an empty struct.

---

## 12. Known constraints

- **Single point of failure.** The board is the product, and now the only client of itself. If it is dead, there is no tracker and no way to view the data short of pulling the flash. Accepted deliberately.
- **Firmware dev loop.** Flash-and-test is slow. §11.1's host-compiled test target for `derive` and the slot maths takes most of the sting out, and `design/` (§11 step 0) removes the hardware from the design loop entirely; build both early.
- **Debug endpoints are unauthenticated.** Anyone on the same network can read `/state` and `/logs`. They are read-only, so the exposure is your habit data rather than control of the device — fine on a home LAN, worth a shared-secret header before it ever sits on a shared or office network. Nothing needs internet access; WiFi is for NTP and `curl`, no account, no cloud.
- **No client, by choice.** No phone app, no browser page, no remote logging. If you are not at the board, nothing happens. §1 has the reasoning; §8.2 has the exit if that stops being true.
- **No reach away from the desk.** Deliberate. If you are not at the board, it does not prompt you, and it does not tell your phone.
- **A board that was off accumulates real misses.** Simplified from an earlier draft that tracked presence explicitly (§3.1) — a board that slept, ran flat, or was unplugged for part of the day will show every slot it missed during that gap as missed when it returns, the same as if it had simply failed to prompt. Accepted for the simplicity; §10 recommends USB-C precisely to keep this rare.
- **Clock.** NTP at boot when WiFi is available, PCF85063 otherwise. The RTC runs off the AXP2101's RTC rail with the main battery behind it — the dedicated backup-cell pads ship empty — so time survives reboots and unplugs while any battery is attached, and is lost when all power is removed. A board that then boots without ever seeing NTP has a wrong date, and the day key is wrong with it. Show the date in the header so this is visible rather than silent.

---

## 13. History, and what the storage change costs

This section used to be "What SQLite buys" and argued the case at length: dedupe as a constraint, retention as one `DELETE`, WAL surviving a power cut, history as queries. Spike S2 (§3.4) measured those claims on the board and two of them were false — WAL is unavailable on this port, and inserts ran at ~3s each before the database reached a tenth of its retention size. The section is kept, rewritten, rather than deleted, because the *reasoning* it contained is still the reasoning that applies: prefer the thing that removes code, and keep the data cheap enough that history stays possible.

### 13.1 What actually carried over

The load-bearing claim was never SQL, it was §3.1's model: an append-only log of uniquely-keyed events, with everything else derived. That model is untouched. What changed is only how bytes reach flash.

| Was going to be | Is |
|---|---|
| `UNIQUE (action, slot)` in the schema | a scan of the open day in RAM (§3.4) — at most ~30 comparisons |
| `DELETE FROM events WHERE day < …` | `unlink` per file, ISO names sorting lexically |
| WAL surviving a power cut | `fsync` per line; a torn write loses at most the last line, never the file |

The dedupe case is the one that got weaker and the one to watch. As a constraint the database refused a duplicate no matter which code path reached it, including paths nobody had written yet. As a loop it is enforced by code that a future edit could quietly break — which is precisely why `duplicate-event.json` and `replay-idempotent.json` (§11.1) exist and why they are mutation-checked.

### 13.2 Where the line is

Unchanged, and now trivially true: **storage holds facts, `derive` holds meaning.** There is no query language to be tempted by, so the temptation §13.2 used to warn about — pushing the slot walk into a recursive CTE — is gone with it. `derive` measured at 0.77ms over a full day on the board, against a 1 Hz tick.

### 13.3 What history looks like now

Retention is still 400 days, because the data is still tiny: ~30 lines/day at ~50 bytes is ~1.5KB/day, so 400 days is ~600KB against a 10MB partition. History is accumulating from day one exactly as before; what changed is where the query runs.

```sh
# Water over the last 30 days — the files are the interface
curl -s http://wfh.local/logs/days | tail -30 | while read d; do
  printf "%s %s\n" "$d" "$(curl -s http://wfh.local/logs/$d | grep -c 'water\sdone')"
done
```

That is a worse developer experience than one `GROUP BY` and a better one than nothing. The interesting question §13.3 raised — "you skip your 15:30 snack four days in five" is a fact about the schedule being wrong, not about the person — is still answerable, just on a laptop over pulled files rather than on the device.

Add `GET /logs/<day>` alongside the §8 debug endpoints when the time comes. Whether anything renders it is still a separate decision.


## 14. Device logs

There is currently no answer to "why did it crash at 3am" beyond a serial cable plugged into a board that's sitting on someone's desk, which in practice means no answer at all.

**A rolling 24-hour buffer, verbose, written as things happen.** State transitions (Active/Idle/Dormant, power events), WiFi association and drops, watchdog resets, SQLite errors — anything worth an `ESP_LOG` call is worth keeping past the point the serial console scrolled past it.

```c
// firmware/main/devlog.c
// Two files, append + rotate — not an in-place byte ring. LittleFS is
// copy-on-write: appends are what it is good at, and rewriting the middle
// of a file (which is what a ring does all day) costs block copies and
// flash wear for nothing. No SQLite involvement: this is diagnostic
// scratch, not data the product depends on being correct.
#define DEVLOG_PATH      "/fs/devlog.0"        // current; renamed to .1 when full
#define DEVLOG_ROTATE    (128 * 1024)          // ×2 files ≈ 24h of verbose logging

void devlog_write(const char *tag, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char line[256];
    int n = snprintf(line, sizeof(line), "%lld [%s] ", (long long)time(NULL), tag);
    n += vsnprintf(line + n, sizeof(line) - n, fmt, ap);
    va_end(ap);

    rotate_append(DEVLOG_PATH, DEVLOG_ROTATE, line, n);    // .0 → .1 at the cap
    ESP_LOGI(tag, "%s", line);                             // still visible over serial
}
```

```c
// GET /logs — pullable over curl without unplugging anything.
static esp_err_t logs_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/plain");
    return rotate_send(req);                   // streams devlog.1 then devlog.0
}
```

Every call site that currently does `ESP_LOGx(...)` on something worth remembering becomes `devlog_write(...)`, which does both — visible on serial live, and pullable over `curl` afterward for the 3am reboot nobody watched. Rotation is capped and automatic specifically so this can be sprinkled liberally without a slow flash-fill-up turning into its own bug.

---

## 15. Risks

§11 orders the build; this section names what could sink it, what retires each risk, and the fallback where one exists. Three half-day spikes (§15.3) come before committing to the full build order — each exists to kill the risks marked with it.

### 15.1 The register

| # | Risk | Odds | Pain | Retired by |
|---|---|---|---|---|
| R1 | ~~SQLite insert latency degrades on LittleFS~~ — **fired.** ~3s/insert at 688 rows, WAL unavailable. Kill switch taken: one append-only file per day (§3.4) | — | — | **closed by S2** |
| R2 | ~~Board in hand is V2~~ — **closed.** The board is V1 (SH8601 + FT5x06, read from the factory firmware's strings) | — | — | **closed** |
| R3 | ~~AXP2101 rail misconfig~~ — **closed.** Vendor BSP configures the rails; panel lit at 368x448 | — | — | **closed by S1** |
| R4 | ~~Codec/PA chain~~ — **closed.** ES8311 opens via `esp_codec_dev`, §9 cues play through the amp gate | — | — | **closed by S3** |
| R5 | Light sleep breaks a peripheral (touch wake, I2S after wake, tick cadence) | medium | medium | spike S3, §11 step 11 |
| R6 | TZ/DST wrong — POSIX string, no zoneinfo on the board | low | high | §2.1 `tz` + `dst-forward` fixture |
| R7 | ~~LVGL effects miss frame budget~~ — **closed.** Wash costs 2.4ms at 1 Hz; the animated waterline costs 555us/frame at 30fps (1.7% of budget). The cliff is not the effect, it is `clip_corner` — see below | — | — | **closed** |

**R1 is the one that could force a design change**, which is why it gets benchmarked before the build starts. There are field reports of the ESP32 SQLite port slowing to seconds per insert in the low thousands of rows on LittleFS — and 400-day retention (§3.4) means ~12,000 rows. The reported cases smell like per-write connection churn and no page cache, both of which this design already avoids (one long-lived connection, §3.4), but that is a hypothesis to test, not a fact to lean on. Mitigations in order: `PRAGMA cache_size` big enough to hold the whole ~1MB database in PSRAM, `page_size=4096` set before first write, and measuring again. **Kill switch:** `store_add_event` / `store_load_day` are the entire storage API — if SQLite still can't hold p99 under ~50ms per insert at 15k rows, swap the implementation for per-day JSONL append files and turn §13.3's history queries into a laptop script over `curl`-pulled files. `derive` and everything above it never know.

**R2:** both revisions have official drivers — `espressif/esp_lcd_sh8601` for V1, `espressif/esp_lcd_co5300` for V2, FT3168 via the `ft5x06` touch driver family, CST820 via `esp_lcd_touch_cst816s`/`kodediy cst820` — so this is a *selection* problem, not a porting project. The risk is writing code against one before reading the label on the other. §11 step 1 already orders it: label first, vendor demo second (Waveshare's `waveshareteam/ESP32-S3-Touch-AMOLED-1.8` repo), product code last.

**R3:** the display and codec rails hang off AXP2101 LDOs, so a wrong PMIC init is a *dark screen*, not an error message — bring-up must copy the vendor demo's rail config before trusting any of §10. Driver: `XPowersLib` (upstream has an ESP-IDF example; packaged as `cube32esp/xpowerslib`).

**R6:** the fixture exists (`dst-forward.json`, §11.1); the config now carries the POSIX rule string (§2.1, §3.1a). The residual risk is authoring the string wrong once — which the header's visible date/time (§12) surfaces the same day.

### 15.2 Scaffolding

The two files the plan was missing, so step 1 starts from them rather than inventing them mid-bring-up.

```
# firmware/partitions.csv — 16MB flash
nvs,      data, nvs,      0x9000,   24K
phy_init, data, phy,      0xf000,   4K
factory,  app,  factory,  0x10000,  4M
storage,  data, littlefs, ,         10M
```

No OTA pair: settings changes are a reflash over the permanently-attached USB-C cable (§2.1), so A/B slots buy nothing but lost flash.

```yaml
# firmware/main/idf_component.yml — V1 parts; starred lines swap for V2
dependencies:
  lvgl/lvgl: "^9"                        # §7 snippets use v8 names — rename on adoption
  espressif/esp_lcd_sh8601: "^2"         # * V2: espressif/esp_lcd_co5300
  espressif/esp_lcd_touch_ft5x06: "^1"   # * V2: esp_lcd_touch_cst816s (or kodediy/esp_lcd_touch_cst820)
  espressif/esp_codec_dev: "^1"
  joltwallet/littlefs: "^1"
  espressif/mdns: "^1"
  cube32esp/xpowerslib: "^0.3"           # AXP2101
```

SQLite is not in the registry: vendor `nopnop2002/esp32-idf-sqlite3` (the IDF-5-updated fork) into `components/sqlite3` and pin the commit in a README line. WiFi credentials ride the same path as the action table — `config/secrets.json`, gitignored, generated into `wifi.g.h` by the same `gen-config` step.

### 15.3 Three spikes, then the build order

- **S1 — panel, touch, PMIC.** ✅ **Passed.** `spikes/s1-board` draws the §7 grid at real size on the panel, with touch and the §9 cues wired. Panel lights at 368x448; touch reports through LVGL and **every tap landed inside the tile it named** — checked against the layout rectangles, so the coordinate mapping has no rotation or mirroring error despite the BSP's `sw_rotate`. R2 and R3 closed. R7 still open: the tiles drawn were static, and the animated wash is the thing with a frame budget.

  Three findings below cost most of the spike and would have cost far more later.
- **S2 — storage under load (half day).** ✅ **Run, and it triggered the kill switch.** SQLite managed ~3s per insert at 688 rows with WAL unavailable; the file backend does 18.9ms p50, flat from 3k to 15k rows. Full numbers in §3.4. The stated pass bar was p99 < 50ms and the file backend does not meet it either — p99 is 558ms of flash garbage collection — but it does not degrade, which was the property that actually mattered. The residual p99 question is stated in §3.4 and left open deliberately.
- **S3 — sound and sleep.** ◐ Sound done in the same flash as S1: ES8311 opens through the BSP's I2C and plays §9's bloom/success/ready with the real envelope. R4 closed. **The sleep half is not done** — light sleep, wake-on-touch and tick cadence remain, so R5 is still open.

**The two findings from S1, both worth more than the spike cost.**

1. **The vendor BSP is versioned by board revision, and the default is the wrong one.** `waveshare/esp32_s3_touch_amoled_1_8` 2.x drives CO5300 + CST816S — V2 hardware — and on a V1 board it initialises a CO5300 against an SH8601 panel and then aborts on `Touch not found`. The 1.x line (`~1.1.4`) is the V1 BSP. Pin the major version to the board revision; `^2` on a V1 board is a crash, not a warning.
2. **The V1 BSP creates the I/O expander and then never drives a pin of it — and three panel-critical lines hang off that expander.** `bsp_io_expander_init()` returns a handle; nothing in the BSP calls `set_dir` or `set_level`. So unless the application does it:

   | Expander pin | Line | Consequence of leaving it |
   |---|---|---|
   | EXIO0 | `LCD_RESET` | controller never released from reset |
   | EXIO1 | `DSI_PWR_EN` | **panel has no VCI — the screen stays black** |
   | EXIO2 | `TP_RESET` | touch never answers; `bsp_display_start()` aborts in `bsp_touch_new` |

   The panel-power one is the nastiest failure in this whole build so far, because **it does not look like a failure**: `sh8601: LCD panel create success`, `panel up: 368x448`, brightness accepted, every SPI write returning `ESP_OK` — into a display with no power. There is no error anywhere to grep for. Drive EXIO1 high, pulse EXIO0 and EXIO2 low-then-high, *then* call `bsp_display_start()`.

   An I2C scan is what separates "touch absent" from "touch in reset" — the driver error is identical either way:

```
i2c scan (before expander): 0x18 0x20 0x34 0x51 0x6B          ES8311, TCA9554, AXP2101, PCF85063, QMI8658
i2c scan (after touch reset): 0x18 0x20 0x34 0x38 0x51 0x6B    + FT3168
```

That scan is also the cheapest possible board-health check, and worth keeping in bring-up permanently.

3. **`clip_corner` is the frame-budget cliff, not the animation.** Clipping children to a rounded corner makes LVGL allocate a mask layer and blend the tile per-pixel on every redraw. At the 1 Hz tick that is invisible; at 30fps it pinned the LVGL task hard enough to starve the idle task and trip the task watchdog — with a backtrace deep in `lv_draw_layer_alloc_buf` that says nothing about corners. Removing it took the animated waterline to **555us/frame, 1.7% of a 33ms budget**, stable over thousands of frames. The lesson generalises: on this panel, per-pixel masking is the expensive thing, and moving geometry is nearly free.

A failed spike changes the plan while the plan is still cheap to change. That is the entire budget: a day and a half before §11 step 1.
