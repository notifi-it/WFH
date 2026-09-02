# WFH Health Tracker — Implementation Plan

Status: Firmware built and running on the board; this document is the spec and the record of decisions.

---

## 1. Scope

A square-format habit tracker that prompts six actions on independent timers during working hours, chimes, and logs one tap per completion. The grid shows today at a glance; each action's popup opens that action's day (§7.7).

**A standalone desk object with a debug port. Nothing else.**

Hardware: Waveshare ESP32-S3-Touch-AMOLED-1.8 (368x448, ESP32-S3R8, 8MB PSRAM, 16MB flash, QMI8658 IMU, PCF85063 RTC, AXP2101 PMIC, ES8311 codec + NS4150B amp into an onboard 8Ω/1W speaker, TCA9554 I/O expander, battery connector — no cell included).

**The board ships in two revisions and the difference is not cosmetic:** V1 pairs the SH8601 display controller with FT3168 touch; V2 (shipping from May 30, 2026) pairs CO5300 with CST820. Display bring-up and the touch driver both change with it. Check the label on the back before writing a line of driver code; everything schematic-derived in this plan (§9.1, §10) was verified against V1.

The board owns everything: the schedule, the clock, the event log, the prompting, the sound, and the only screen. There is no companion app, no browser client, no second way to log anything.

**The web app was cut.** The plan began as a browser app with the board as a later phase; collapsing that into one board-authoritative build removed a throwaway browser scheduler. The web app survived that collapse as "a viewer and a remote" and grew an HTTP server, a WebSocket, a client class and a frontend architecture before anyone re-asked whether it should exist. Its justifications did not hold:

- **Settings** — changed once at setup, maybe a few times after. `actions.json` plus a reflash covers it (§2.1).
- **Logs** — the board sits on USB-C (§10), so the serial cable is already attached.
- **History** — real, and served on the board itself, per action (§7.7).

What makes a future web version cheap is the event log in §3 and the snapshot shape in §8 being clean, not a React app existing now.

The board sits on the desk and prompts the person sitting at it. It does not chase them elsewhere: no phone push, no notifications away from the desk.

**Explicitly out of scope:** eye breaks (20-20-20), the lunchtime walk, any "silence for the day" control, any companion client, and the end-of-day shut-down prompt (cut after seeing it on the board: it was the one tile that never told you anything you did not already know). Multi-day history is a laptop script over pulled files (§13.3).

---

## 2. Actions

| Action | Cadence | Daily target | Tint | Flow |
|---|---|---|---|---|
| Stand break | every 40 min | 10 | green `#7fd4a8` | single tap |
| Water | every 45 min | 8 | blue `#6ec3e0` | single tap |
| Shoulder roll | every 60 min | 8 | violet `#b6a3e8` | guided, 1 step |
| Snack | 10:45, 15:30 | 2 | amber `#e8b06a` | single tap |
| Lunch | 13:00 | 1 | orange `#e8926a` | single tap |
| Stretches | 11:30, 16:30 | 2 | yellow `#e0d16a` | guided, 3 steps |

Working hours 09:00–18:00. No prompts outside the window, save a 30-minute grace past `workEnd` (`graceMin`) so the last slot of the day keeps an open window long enough to be answered (§5.2). Timers reset at 09:00 the next day.

**Stretch sequence:** chin tucks (30s), doorway pec stretch (40s), cat-cow (40s). The hip flexor stretch was cut in the v4 redesign, taking the set from ~3 minutes to ~2.

### 2.1 Config as data, not literals

The action table is authored once as JSON and generated into the firmware, so changing a cadence is a config edit and a reflash rather than hunting literals through C:

```json
// config/actions.json — the only place this table is edited
{
  "workStart": "09:00",
  "workEnd": "18:00",
  "tz": "GMT0BST,M3.5.0/1,M10.5.0",
  "graceMin": 30,
  "actions": [
    { "id": "stand", "name": "Stand break", "blurb": "Up on your feet for a minute.",
      "icon": "person-check", "tint": "#7fd4a8", "target": 10, "flow": "tap", "priority": 1,
      "cadence": { "kind": "interval", "everyMin": 40 } },

    { "id": "water", "name": "Water", "blurb": "Glass of water. Refill while you are up.",
      "icon": "drop", "tint": "#6ec3e0", "target": 8, "flow": "tap", "priority": 1,
      "cadence": { "kind": "interval", "everyMin": 45 } },

    { "id": "roll", "name": "Shoulder roll", "blurb": "Ten slow rolls back, then drop the shoulders.",
      "icon": "refresh", "tint": "#b6a3e8", "target": 8, "flow": "stretch", "priority": 1,
      "cadence": { "kind": "interval", "everyMin": 60 } },

    { "id": "snack", "name": "Snack", "blurb": "Something small before the slump.",
      "icon": "apple", "tint": "#e8b06a", "target": 2, "flow": "tap", "priority": 2,
      "cadence": { "kind": "fixed", "times": ["10:45", "15:30"] } },

    { "id": "lunch", "name": "Lunch", "blurb": "Away from the desk.",
      "icon": "bowl", "tint": "#e8926a", "target": 1, "flow": "tap", "priority": 2,
      "cadence": { "kind": "fixed", "times": ["13:00"] } },

    { "id": "stretch", "name": "Stretches", "blurb": "Three stretches, about two minutes.",
      "icon": "sun", "tint": "#e0d16a", "target": 2, "flow": "stretch", "priority": 3,
      "cadence": { "kind": "fixed", "times": ["11:30", "16:30"] } }
  ],
  "stretches": [
    { "name": "Chin tucks",          "seconds": 30, "cue": "Draw the chin straight back, hold, release. Resets a screen-jutted neck." },
    { "name": "Doorway pec stretch", "seconds": 40, "cue": "Forearms on the frame, step through, chest open. Both sides at once." },
    { "name": "Cat-cow",             "seconds": 40, "cue": "All fours: arch up, dip down, with the breath. Loosens the sitting spine." }
  ]
}
```

```
tools/gen-config.mjs  →  firmware/main/actions.g.h      (C: static const action_def_t[])
```

The generated header is committed. `make -C firmware/test check-gen` regenerates it and diffs against the checkout, so a hand-edit of a generated file is caught; there is no CI, so run it before pushing.

This is also the settings mechanism. There is no settings screen and no settings client: working hours and cadences change here, then you reflash. For something touched a handful of times ever, that beats designing a config UI for a 368px panel.

**No jitter.** An earlier draft staggered start offsets (stand at 09:12, water at 09:07, …) specifically to keep the three interval actions from lining up. That was solving the wrong problem: collisions are fine now that the card shows everything due at once as a checklist (§6) rather than answering one at a time. All three actions anchor to `workStart` and simply collide when they collide.

---

## 3. Data model

What the board stores and what it derives. Facts in the store, meaning computed on top — never the other way round.

### 3.1 What is actually true

Two things are facts. Everything else is a view over them.

1. **The schedule** — config, from §2.1. Not per-day data.
2. **The event log** — what the user did. Append-only. `done`, `skip`, and `undo` (§7.7), nothing else.

**A miss is derived, not stored:** a slot is missed if it is in the past and no `done` or `skip` answers it. No third fact needed, no "was the board even on" tracking.

**This was simplified from an earlier draft that also tracked awake windows** — periods the board was demonstrably running — so a slot that passed while the board was off, asleep, or flat could be told apart from one the board genuinely failed to prompt for. That bought a real property (a board that ran flat for 90 minutes didn't get blamed for 90 minutes of misses) at the cost of real complexity: a second table, a gap-detection heuristic with a tunable constant, and a join in `derive` that had to reason about two kinds of interval simultaneously.

**Removed.** The rule now is the plain one: **unanswered and in the past means missed.** The tradeoff this accepts: a board that was off — asleep, unplugged, dead battery — for any part of the working day will show every slot it missed during that gap as a genuine miss when it comes back, the same as if it had been on and simply not prompted. If that turns out to matter in practice, the fix is to reintroduce presence tracking in one narrow form; §10's USB-C recommendation makes the gap rare, so it is not worth the model complexity up front.

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
// (log_event_t / day_log_t / derive, §3.3). A future client would code
// against these through GET /state (§8.2); none exists.
export type EventKind = 'done' | 'skip' | 'undo';   // no 'miss' — see above

export interface LogEvent {
  id: string;          // uuid v4 — written for external tooling, never parsed
  action: ActionId;
  kind: EventKind;
  ts: number;          // epoch seconds — when the user tapped
  slot: number;        // epoch seconds — which slot this answers; dedupe key
}

/** One day as the board tracks it. `events` is the persisted part (§3.4);
 *  `snoozedUntil` is RAM-only — the store has no line for it, deliberately.
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

`slot` is the field that makes the whole thing idempotent. A double-tap on Done, a confirm that includes an already-answered row, a replayed event — all collapse to one entry because the latest event per `(action, slot)` is the truth and a repeat of it is not written (§3.4). It is also the join key between the schedule and the log, which is what lets a miss be derived at all.

There is no `source` field. With the board as the only writer there is nothing to attribute.

### 3.1a Timestamps: epoch everywhere except display

**Every timestamp that is stored, transmitted, or compared is an epoch integer.** `ts`, `slot`, `snoozedUntil` values, the `now` field in the API snapshot — all epoch seconds, UTC-equivalent, no timezone attached because none is needed.

The one exception is `cadence.times` in `actions.json` (`"11:30"`, `"16:30"`, …) — a wall-clock string, because that's the natural way for a human to author a schedule. It is converted to an epoch value immediately by `at_time()` (§5.1) and never travels as a string past that point; nothing downstream of config ever parses a time-of-day string again.

Local wall-clock time reappears exactly once more: at render, when a timestamp becomes "next 14:32" on a popup or "done at 09:51" on a history row. That conversion happens in the UI layer only and is never fed back into the model.

**Why this matters for timezones.** All slot arithmetic happens in the board's own local time (§5.1, via `mktime`/`localtime_r` against a `TZ` set at boot). The `TZ` value is the `tz` field of §2.1's config, and it must be a **POSIX rule string** (`"GMT0BST,M3.5.0/1,M10.5.0"`) — ESP32's newlib has no zoneinfo database, so an IANA name like `Europe/London` means nothing on the board. The board's clock is the only clock in the system — there is no second device whose timezone could disagree. The remaining gap is the board itself: nothing auto-detects a new timezone if it physically moves (no GPS), so a relocated board needs its `TZ` updated and reflashed.

### 3.2 Derivation

One pass per action, walking that action's slots forward and folding in the events that answer them. Six actions, at most ~30 slots each — a few hundred comparisons, cheap enough to run on every tick on an ESP32.

```ts
export function derive(log: DayLog, now: number, s: Settings, day = new Date()): DayView {
  const view: DayView = { counts: {}, skipped: {}, missed: {}, next: {}, due: [] };

  // Later lines overwrite earlier ones: the latest event per slot wins
  // (§3.4). A slot whose latest is an undo is unanswered again.
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
        // Unanswered (or undone) and in the past: due while its window is
        // open, missed once it closes. A slot's window runs from its time
        // until the action's next slot (or workEnd + the §5.2 grace, for
        // the last one); snooze defers due-ness within the window.
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

**Open question, surfaced by the fixtures.** Deriving the window from the *next slot* is right for the interval actions — a stand prompt gives up after 40 minutes — but the fixed ones have enormous gaps, so an unanswered 10:45 snack stays on the card until 15:30, and an unanswered 11:30 stretch until 16:30. `stretch-priority.json` (§11.1) asserts exactly that today, which is how it was noticed. Two ways to go: accept it (the card is the outstanding list, and a snack you never ate *is* outstanding), or cap the window at some minutes-open constant. The cap is a tunable, and this plan has spent two revisions deleting tunables (§3.1), so the default stands. Change the fixture with it if it changes.

### 3.3 `derive` runs once, on the board

Two earlier drafts got this wrong in opposite directions: one implemented `derive` twice (C for the board, TypeScript for a browser client) with shared fixtures to keep them in step; the next kept one implementation but shipped its output to that client.

With no client, the board computes its own view for its own screen. The fixtures in §11.1 test that one implementation.

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

`<ts>\t<slot>\t<action>\t<kind>\t<uuid>`, kind ∈ `done | skip | undo`. The uuid is written for external tooling and never parsed by the firmware. Plain text and greppable. TSV rather than the JSON the kill switch originally sketched: identical properties, one `fprintf` to write, one `sscanf` to read, and no JSON parser in the firmware.

**Appending is the one thing LittleFS is genuinely good at**, and the design leans on exactly that. Durability comes from `fflush` + `fsync` after each line, which is what `synchronous=FULL` was buying.

**The open day stays open.** This is the difference between the flat line above and a slow crawl, and it was measured rather than assumed: opening a file costs a linear scan of the directory, so with 400 days retained an open-per-write makes every tap pay for every day ever recorded. Holding the handle and the day's events in RAM turned p50 from ~90ms-and-climbing into 24ms-and-flat.

**Uniqueness is "latest wins".** The `UNIQUE (action, slot)` constraint that the schema used to carry is now a rule over the open day's RAM copy: the latest event per `(action, slot)` is the truth. A repeat of whatever is already latest is a duplicate and is not written; a different kind — an undo after a done, a done after an undo — is a change and appends. A slot whose latest event is an undo reads as never answered. This is the one property the tests have to keep pinned (§11.1, `duplicate-event`, `replay-idempotent`).

```c
// firmware/main/store_files.c
static char      g_open_day[11];
static FILE     *g_fp;
static day_log_t g_cache;      // the same in-RAM day §4 already wanted

bool store_add_event(const log_event_t *ev, const settings_t *s) {
    const char *day = store_day_key(ev->ts);
    if (!open_day(day, s)) return false;       // reloads + reopens only on rollover

    const log_event_t *latest = wfh_latest_event(&g_cache, ev->action, ev->slot);
    if (latest ? latest->kind == ev->kind : ev->kind == KIND_UNDO) return false;

    fprintf(g_fp, "%lld\t%lld\t%s\t%s\t%08lx%08lx\n", …);
    fflush(g_fp);
    fsync(fileno(g_fp));
    g_cache.events[g_cache.events_len++] = *ev;
    return true;
}
```

**Retention** is `store_prune(400)`: `unlink` on any file whose name sorts before the cutoff — ISO dates sort lexically, so the comparison is `strcmp`. It runs at the midnight rollover in the tick task (§5.2). Measured at 1.9s for a 400-day sweep, once a day.

**p99 is 558ms and the worst case is 1.2s.** That is flash garbage collection, it is inherent, and it does not grow with the data. At ~30 writes a day it means a sub-second hitch roughly every few days. If that ever shows up as a felt problem the answer is to move the write off the UI path — but §4's guarantee is that the screen reflects what was *stored*, so that is a real trade to make deliberately, not a tweak. **Not decided here.**

**What this costs.** No SQL, so multi-day history (§13.3) is a script over pulled files rather than a query on the device. The event log itself is unchanged — §3.1's model was never relational, it was always an append-only log, which is what it is now stored as.


## 4. Firmware architecture

Everything that mutates the log funnels through one function.

```
┌─────────────┐  due actions  ┌─────────────┐
│  scheduler  │ ────────────► │     ui      │  LVGL: grid, card, popup, flow, history
│   1 Hz tick │               │    touch    │
└──────┬──────┘               └──────┬──────┘
       │                             │ input_done() / input_undo()                  §7.4
       │                             ▼
       │                    ┌─────────────────┐      ┌──────────┐
       └───────────────────►│ day_apply_event │─────►│ feedback │ sound
                            │  ← ONLY writer  │      └──────────┘
                            └────────┬────────┘
                                     │
                          ┌──────────┴──────────┐
                          ▼                     ▼
                    ┌───────────────────┐  ┌────────────┐
                    │     day files     │  │ derive()   │
                    │ /fs/d/YYYY-MM-DD  │  │ → DayView  │
                    └───────────────────┘  └────────────┘
```

`day_apply_event` is the only function that writes the log. Every input path in §7.4 funnels through it, and nothing outside the firmware can write.

```c
// firmware/main/day.c
// The choke point. Everything that logs an event comes through here.
// Returns true if this was new, false if we already had it.
bool day_apply_event(const log_event_t *ev) {
    if (!store_add_event(ev, &g_settings)) return false;   // duplicate: stay quiet

    day_reload(time(NULL));                 // refresh the in-RAM copy
    ui_refresh();                           // redraw tiles from a fresh derive
    return true;
}
```

Two properties this gives everything upstream of it:

- **A duplicate is not an error.** Tapping Done twice, or confirming a card whose row was already answered from the popup, gets a quiet no-op rather than a failure or a double count.
- **The screen always reflects what was stored.** The redraw is driven by a `derive` over the freshly-reloaded log, not by patching the view in place, so the tiles cannot show something the store does not contain.

Reloading the whole day after each append rather than patching the in-RAM struct is deliberate: it is a few hundred microseconds, it happens ~30 times a day, and it guarantees the RAM copy can never drift from what is actually stored.

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
    t.tm_hour  = (hhmm[0]-'0')*10 + (hhmm[1]-'0');
    t.tm_min   = (hhmm[3]-'0')*10 + (hhmm[4]-'0');
    t.tm_sec   = 0;
    t.tm_isdst = -1;                      // let mktime resolve the DST flag
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

        if (!day_is_today(&g_day, now)) {          // rollover, §11.2
            day_init(&g_day, now);
            store_prune(400);                      // §3.4 retention
        }

        // Prompt window, not working hours: workStart ≤ now < workEnd + grace.
        // Slots stop generating at workEnd (§5.1), but the last slot's window
        // runs on to workEnd + grace, so a prompt raised near the end of the
        // day can still be answered instead of flipping straight to missed.
        // The grace creates no new slots.
        if (in_prompt_window(now, &g_settings)) {
            day_view_t v;
            derive(&g_day, now, &g_settings, &v);
            card_sync(v.due, v.n_due, now);        // §6 — reconciles the checklist card
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```

The tick writes no tallies and rolls nothing forward — `derive` owns all of that. All it does is raise prompts.

### 5.3 Completing early

Logging is just an append (§7.4's `input_done`). The interval/fixed asymmetry lives entirely in `derive`'s `anchor` and nowhere else: drinking water at 11:20 pushes the next glass to 12:05, while eating lunch at 12:40 leaves the 13:00 slot exactly where it was. That branch lives in one place rather than at every call site.

```c
/** Which slot is this tap answering? The open one if there is one, else the
 *  next upcoming one — which is what makes a popup Done ("I just drank one")
 *  work through the same path as answering a prompt. One slot ahead is the
 *  limit: a second early tap does nothing until that slot opens. */
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

- **One card ("Due now"), one row per currently-due action** — icon, name, and "missed HH:MM" for the slot it is asking about. Nothing is hidden behind a "next" tap.
- **Tapping a row toggles it.** The whole row is the target, not a checkbox. Nothing is logged yet.
- **Log N done** logs `done` for every checked row in one commit, snoozes the unchecked ones 15 minutes, and closes the card. Unchecked rows are still due and unanswered, not skipped — but a card that stays up showing the rows you did not tick reads as the button not having worked. With nothing checked the button is disabled.
- **+15m for all** snoozes every action currently on the card — checked or not — and clears it. Absolute, not additive, so mashing the button never pushes anything past now + 15.
- **X** is +15m-for-all with a smaller target: nothing is logged, everything on the card is snoozed 15 minutes, and the card closes. The dot states in §7.2 distinguish *decided against* from *never answered*; closing the card is neither, so it must not consume any slot — including ones that were checked but never confirmed. An earlier version wrote nothing at all, and the next tick brought the card straight back.

There is no per-row Skip on the card. Skip lives nowhere in the current UI: a slot you do not answer becomes a miss when its window closes, and the popup's single button is Done (or Start for a guided action, §7.6).

Stretches never join the checklist. If a stretch is due alongside anything else, the stretch takes the screen on its own (§7.6) and everything else stays on the card, waiting.

**Built and tested** — `firmware/main/card.c`, with the scenarios in `firmware/test/test_card.c`. Every side effect goes through a `card_host_t` of function pointers (`log_done`, `show_card`, `show_grid`, `take_stretch`), which is what lets the whole state machine run on a laptop; the firmware wires those to `input_done` and the LVGL screens, the test wires them to a recorder.

Writing it turned up two bugs in this section's earlier pseudocode, both fixed in the implementation:

- **The card redrew every tick.** `card_sync` ended with `if (g_card_len > 0) ui_show_card();`, and the card screen is rebuilt on show. At 1 Hz that wipes a half-made decision once a second — the exact thing `card_sync` exists to protect. It now returns whether the row set actually changed and only redraws on a change.
- **The stretch was pushed as a row, then handled.** The code added every due action first and only afterwards checked whether one of them was a stretch. A stretch-flow action is now skipped when rows are built, so it can never be checked, confirmed, or counted as a row.

The scenarios were mutation-checked the same way as `derive`: broken row lookup, confirming checked rows without snoozing the unchecked ones, +15m-for-all compounding, +15m-for-all ignoring unchecked rows, the stretch joining the list, and redrawing every tick.

`card_sync` runs every tick rather than the card being built once when it opens, so a card left mid-decision — two rows checked, pulled away, back an hour later — still reflects exactly what is due, with the two checks intact and nothing double-logged.

**One edge to be aware of.** A snoozed slot that passes unanswered still derives as a miss, because `derive` only sees the *current* `snoozed_until`, not that the slot was snoozed at the time. With the shortest cadence at 40 minutes a 15-minute delay cannot span a slot boundary, so this is unreachable as configured. It becomes reachable if a cadence in `actions.json` ever goes below ~15 minutes — at which point the fix is to log the snooze as an event carrying the slot it covered, not to special-case `derive`. Floor the configurable cadence at 20 minutes.

---

## 7. Board UI

368x448 AMOLED, LVGL 9 via the vendor BSP (v9 names: `lv_button_*`, `lv_screen_*`). The 2x3 grid owns the whole panel: there is no header, and no clock, date, battery or sound indicator anywhere in the UI. Fonts are LVGL's bundled Montserrat (14/16/28/36/48); a generated custom font lacks the `LV_SYMBOL_*` glyphs the X and history marks use.

```
┌────────────────────┬────────────────────┐
│  Stand             │  Water             │
│  ●●●●○○○○○○        │  ●●●◌○○○○          │  ← 3 rows × 140px
├────────────────────┼────────────────────┤
│  Shoulder roll     │  Snack             │
├────────────────────┼────────────────────┤
│  Lunch             │  Stretches         │
└────────────────────┴────────────────────┘
```

### 7.1 Grid (default)

- 6 tiles, 2 columns, 3 even rows at 140px — no header bar, so the grid owns all 448px
- Each tile: icon, name, progress dots, and the wash
- **Countdown** is the wash: a tinted block rising from the bottom of the tile as the slot approaches. No countdown text on the tile; the number lives on the popup.

The wash is a flat, deeper-tinted block (no gradient) whose height is the fraction of the interval elapsed, set once per second from the scheduler tick. Only its surface animates: a thin crest drawn as an `lv_line` of 61 float points, the sum of two sines, updated at 30fps. The body below it is static geometry. Per-pixel effects (`clip_corner`, gradients over the tile) are what cost frames on this panel (§15.3, R7); a moving polyline costs 555us/frame.

```c
static void tile_set_fill(tile_t *t, float frac) {            // frac 0 → 1
    lv_obj_set_height(t->wash, (lv_coord_t)(TILE_H * frac));  // the tile constant, not lv_obj_get_content_height()
    lv_obj_align(t->wash, LV_ALIGN_BOTTOM_MID, 0, 0);
}
```

Tapping a tile opens that action's popup (§7.3b). Nothing is logged from the grid itself.

### 7.2 Dot states

| State | Appearance | Meaning |
|---|---|---|
| Done | filled, action tint | logged |
| Skipped | mid-grey solid | deliberately skipped |
| Missed | hollow ring | due window closed with no response (§3.2) |
| Open / due | hollow ring | past, unanswered, window still open — fills when answered |
| Upcoming | dark solid | still to come |

Skipped and missed are visually distinct because deciding not to eat lunch and forgetting to log lunch are different facts about the day.

Dots render in slot order, left to right: a timeline, not a tally. The dot that fills is the slot that was answered, and a slot let slide stays hollow in front of it — miss two and answer the third, and the third fills. A due slot is hollow as well — the card and popup already call it missed, and it fills the moment it is answered. This is what makes the one-slot rule visible: an answer can only land on the slot that is currently open (or the next one if nothing is open, one slot ahead at most), so earlier misses are never backfilled. `derive` exposes the per-slot states (`slots[]`, `n_slots[]`) alongside the tallies. An overshoot (more done than the target) grows the row rather than truncating.

### 7.3 The prompt card

One screen, N rows (§6), built by `build_card()` in `firmware/main/ui.c`. Each row is tinted to its own action rather than the whole screen taking one tint, since a card can hold several actions at once.

- A row is icon, name, and "missed HH:MM" for the slot being asked about. The whole row is the toggle; a checked row is drawn in its tint.
- The bar at the bottom holds **Log N done** (disabled at N = 0) and **+15m for all**.
- The X in the top-right corner is the shared `x_button()` (§7.3a) wired to the same handler as +15m for all.

`card_sync` rebuilds the rows only when the due set changes (§6); a redraw does not touch the checked state.

A single due action still renders as this same one-row card rather than reverting to a dedicated single-prompt screen — one component, always, so there is nothing separate to keep in sync.

### 7.3a One X

`x_button()` is a static function in `ui.c` and the only place an X is built. It is used by the card, the popup, the history view and the guided flow, so the X looks and behaves identically everywhere it appears. It is a 64px square, flush in the top-right corner, transparent, with a 12px extended click area — a 44px target was missed more often than hit on a 1.8" panel. Each caller passes its own close handler: the card's and the flow's snooze 15 minutes (§6, §7.6), the popup's returns to the grid, the history view's returns to the popup.

### 7.3b The popup

Tapping a tile opens that action's popup: icon, name, blurb, then the slot it is asking about and the one after it, side by side — "missed HH:MM" (only when an open, unanswered slot exists; with "snoozed to HH:MM" under it if it is snoozed) and "next HH:MM". One button: **Done** for a tap action, **Start** for a guided one (§7.6). No Skip, no +15m, no count. Top-left is the history icon (§7.7); top-right is the X, which returns to the grid.

### 7.4 Inputs

Touch is the only input. Every way of answering ends at the same three functions, and nothing else in the firmware writes an event — and with no client, nothing outside the firmware can.

```
touch: card rows + Log N done  ─┐
touch: popup Done               ─┼──► input_done(action)        ──► day_apply_event()
touch: guided flow finishing    ─┘
touch: flow X (abandon)         ────► snoozed_until[action] = now + 15 min (no event)
touch: history row Undo         ────► input_undo(action, slot)  ──► day_apply_event()
```

```c
// firmware/main/input.c — every input path lands here

void input_done(int action) {
    if (answered_ahead(action)) return;       // one slot ahead is the limit, §5.3
    log_event_t ev = {
        .action = action,
        .kind   = KIND_DONE,
        .ts     = time(NULL),                 // RTC-backed; right after a reboot
        .slot   = day_current_or_next_slot(action),
    };
    if (ev.slot == 0) return;                 // nothing left today to answer
    if (day_apply_event(&ev)) feedback_play(CUE_SUCCESS);   // false = we already had it
}

void input_undo(int action, time_t slot) {
    // No guard: undo is the guard. It only ever takes back a tap.
    log_event_t ev = { .action = action, .kind = KIND_UNDO, .ts = time(NULL), .slot = slot };
    if (day_apply_event(&ev)) feedback_play(CUE_READY);
}
```

Gating the cue on `day_apply_event`'s return matters in use: confirming a card that includes an already-answered row should chime once for what actually landed, not once per row. The duplicate is caught by the store's latest-wins rule (§3.4), and the silence for that row tells nothing new happened.

`input_done` does not advance a queue — §6's `card_confirm`/`card_delay_all` own screen transitions, since a card can hold several rows and confirming does not necessarily mean the card is empty.

**No physical key.** The board's BOOT button (`GPIO0`, active low, RTC-capable) is the only button usable at runtime — PWR reaches the ESP only through the I/O expander (`EXIO4`) and its long-press is a PMIC power-off — but nothing reads it. Resting a finger on BOOT while plugging in USB drops the board into download mode instead of booting the app.

**No IMU tap.** If the QMI8658's tap detection is ever wired (its INT1 lands on the expander, so it would be an I2C poll), it may only answer a row already on the card, never originate an event: the false-positive rate on desk knocks is far too high for anything else.

### 7.5 The sound icon

**Removed.** There is no header and the popup carries no sound icon. Sound is always on; there is no toggle, no `CUE_SOUND_ON`, and no haptic — this board has no motor, and adding one is a BOM, wiring and enclosure change that one gesture does not justify.

### 7.6 Stretch flow

Replaces the prompt screen. One stretch per screen with a countdown ring. Each step waits on a Start tap (you need a moment to get into position), runs to zero, and auto-advances; the last step finishing logs the done — there is no Done button. A guided action's popup says Start rather than Done, and opens the flow. Skip and +15m are not offered mid-flow; the X backs out with a 15-minute snooze and logs nothing.

Derive the remaining time from an absolute end timestamp rather than decrementing a counter, so a delayed render resumes at the correct point instead of stretching a 40-second hold into a minute.

Abandoning the set part-way logs a `skip`, not a partial `done` — a half-finished stretch set is a skipped stretch set, and the dot should say so.

### 7.7 One action's day (history)

Top-left of the popup: a clock-arrow that opens that action's day. Head: icon, name, "2 of 8 today". Then one row per slot, in order — slot time large, a plain-words state (*done at 09:51*, *skipped at 11:20*, *missed*, *open*, *open · snoozed to HH:MM*, *upcoming*), and the same mark as the tile dots. A NOW rule splits past from upcoming, and upcoming rows are dimmed. Done and skipped rows carry **Undo**. The X returns to the popup.

Undo appends a `KIND_UNDO` event for that (action, slot); it never deletes. Under §3.4's rule — the latest event per (action, slot) is the truth — a slot whose latest is an undo reads exactly as if it had never been answered: open if its window is still running, missed if not. The store's idempotence rule follows the same line: a repeat of whatever is already latest is a duplicate, a different kind is a change. Undo rebuilds the list, so it must defer with `lv_async_call()` rather than clean the screen from inside the button's own event handler. `design/history-v4.html`; `#water` shows a busy day.

---

## 8. Debug endpoints

**Not built.** WiFi currently does one thing, SNTP (§8.1), and the radio is stopped before the panel powers because the two cannot be alive together. Any endpoint would have to run inside a panel-off window, which is a constraint the design below does not yet address.

The intent: not an app. Two read-only endpoints you `curl` when something looks wrong, and NTP.

| | | |
|---|---|---|
| `GET /state` | → `{ log, view, power, now }` | what the board thinks is true |
| `GET /logs` | → last 24h of log lines, `text/plain` | why it rebooted at 3am, §14 |

No `POST`. **The board is the only writer of events** — there is no second client, so there is no cross-writer reconciliation, no `source` attribution, and no clock-skew problem. Everything §3 says about `(action, slot)` uniqueness still holds, but it only has to survive a double-tap on the panel rather than two devices racing.

```c
// firmware/main/api.c — the whole HTTP surface
static esp_err_t state_get(httpd_req_t *req) {
    char *json = snapshot_json();               // log + derived view + power + now
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
        { .uri = "/logs",  .method = HTTP_GET, .handler = logs_get },   // §14
    };
    for (int i = 0; i < 2; i++) httpd_register_uri_handler(g_server, &routes[i]);
}
```

The endpoints would be unauthenticated: anyone on the same network could read `/state` and `/logs`. Read-only, so the exposure is habit data rather than control of the device — fine on a home LAN, worth a shared-secret header before it ever sits on a shared or office network.

The snapshot shape is the contract a future web version would implement against:

```jsonc
{
  "log":   { "version": 3, "date": "2026-08-14", "events": [ … ] },
  "view":  { "counts": {…}, "skipped": {…}, "missed": {…}, "next": {…}, "due": [ … ] },
  "power": { "battPct": 82, "charging": true, "onBattery": false },
  "now":   1723645200
}
```

### 8.1 Why WiFi survives the cut

With no client, the radio earns its place on one job: **NTP**. A board that has never seen network time — and whose RTC backup is flat — has a wrong date, and a wrong date silently writes events under the wrong day key. That is the one failure here that corrupts data rather than merely annoying you.

How it runs (`wifi_time.c`):

- The clock is seeded from the build time if the RTC is unset, so a board with no network still has a plausible date.
- SNTP runs in `app_main`, blocking, **before `panel_power_up()`**. The WiFi radio and the QSPI panel cannot be alive at the same time: with the radio up the panel tears into white bands and freezes. The screen stays dark for the few seconds the sync takes.
- The radio is then **stopped, not deinitialised**. `esp_wifi_deinit()` frees driver RAM that the panel bring-up reuses, and the driver's teardown then writes over LVGL's tick mutex (CLAUDE.md).
- Credentials come from gitignored `config/secrets.json`, generated into gitignored `firmware/main/wifi.g.h` by `tools/gen-config.mjs`. A checkout without secrets builds and runs offline on the build-time seed.

Everything else WiFi was carrying is gone: no static file serving from flash, no WebSocket, no mDNS, no mixed-content constraint, no client socket table.

### 8.2 If a web version ever happens

Build it then, against `GET /state` once it exists. What makes that cheap is §3's event log and the snapshot above — both designed to be transmitted, neither dependent on a client existing. Adding a `POST /event` and a push socket later is a contained change; having built a frontend now against requirements nobody has articulated would not have made it cheaper.

---

## 9. Feedback

Three cues, all sound — no motor is fitted on this board, so there is one channel, not two.

| Event | Sound |
|---|---|
| Prompt appears | `bloom` — rising two-note |
| Action logged | `success` — quick up-tick |
| Stretch step complete, or an Undo | `ready` — single soft note |

```c
// firmware/main/feedback.h
typedef enum { CUE_BLOOM, CUE_SUCCESS, CUE_READY } cue_t;

void feedback_play(cue_t cue);   // non-blocking
```

```c
// firmware/main/feedback.c — the one table
static const tone_t BLOOM[]   = { {523, 90}, {659, 140} };   // C5 → E5
static const tone_t SUCCESS[] = { {659, 70}, {880, 110} };   // E5 → A5
static const tone_t READY[]   = { {440, 120} };              // A4
```

### 9.1 Sound

The chain is I2S → ES8311 codec → NS4150B amp → the onboard 8Ω/1W speaker. Rather than shipping audio files, synthesise the tones — a sine with a short attack and an exponential decay, which is all these cues are.

Pins, from the V1 schematic: MCLK `GPIO16`, BCLK `GPIO9`, LRCK `GPIO45`, data out `GPIO8` (the mic comes back on `GPIO10`, unused here). The codec needs an I2C init before it makes a sound — use `esp_codec_dev` rather than hand-rolling ES8311 registers — and the amp is gated by `PA_CTRL` on `GPIO46`: high to play, low when silent so a silent board is actually silent.

```c
// firmware/main/audio.c
#define SAMPLE_RATE 16000

typedef struct { uint16_t freq_hz; uint16_t ms; } tone_t;

/** Render one tone into `buf` and write it to I2S. Blocking, runs on the
 *  audio task only — never call this from the LVGL or scheduler tasks. */
static void render_tone(const tone_t *t) {
    const int n = (SAMPLE_RATE * t->ms) / 1000;
    static int16_t buf[SAMPLE_RATE / 4];                 // 250ms max per tone
    const float step = 2.0f * M_PI * t->freq_hz / SAMPLE_RATE;

    for (int i = 0; i < n; i++) {
        // Envelope: 5ms attack so it doesn't click, then exponential decay.
        // Without the attack ramp a square-edged start pops audibly on a
        // small speaker.
        float attack = fminf(1.0f, i / (0.005f * SAMPLE_RATE));
        float decay  = expf(-3.0f * i / n);
        float env    = attack * decay * (VOLUME / 100.0f);

        buf[i] = (int16_t)(sinf(step * i) * env * 12000);
    }

    size_t written;
    i2s_channel_write(g_i2s, buf, n * sizeof(int16_t), &written, portMAX_DELAY);
}

/** Queue a cue. Returns immediately — the audio task does the work. */
void audio_play(const tone_t *tones, int n) {
    audio_msg_t msg = { .n = n };
    memcpy(msg.tones, tones, n * sizeof(tone_t));
    xQueueSend(g_audio_q, &msg, 0);                      // drop if the queue is full, don't block
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

The queue means `feedback_play` never blocks a touch handler, and `xQueueSend` with a zero timeout means a burst of cues drops rather than backing up — three prompts arriving at once should not produce eight seconds of queued beeping.

*Note:* `bloom` is soft and may not cut through when heads-down. Test against a brighter `chime` before locking it in, **and test it on the board's actual speaker**, which is small and will not reproduce the low end the way a laptop does.

**Escalate rather than repeat.** If a prompt goes unanswered for two minutes, re-cue once. If it is still unanswered when the next slot arrives it becomes a derived miss and the board goes quiet. It must never nag in a loop.

```c
// In the scheduler tick (main.c). One-shot per card, `>=` not `==` so one
// skipped tick cannot skip the escalation.
if (g_card.len > 0 && !g_card.escalated && now - g_card.raised_at >= ESCALATE_AFTER_S) {
    g_card.escalated = true;                 // cleared when the card empties
    feedback_play(CUE_BLOOM);
}
```

The re-cue plays at the same volume. A louder re-cue would have to ride inside the queued message: `audio_play` is asynchronous, so bumping a global volume and restoring it around the call would race the audio task and mostly play the re-cue at normal volume anyway.

---

## 10. Power

**Not built.** The board runs lit at a fixed brightness on USB-C. This section is the design for battery operation.

**Recommendation: run it on USB-C.** It sits on a desk next to a laptop; a permanently-connected cable is not a hardship and it removes the question entirely. The battery then covers a power cut and the occasional unplugging rather than being the operating mode — and the design below means neither corrupts the day's record. §3.1 and §12 lean on this: a board that is off accumulates real misses, and the cable is what keeps that rare.

**Rough numbers.** The 1.8" AMOLED dominates — call it 80–150mA lit depending on how much of the panel is emitting, against maybe 40–50mA for the ESP32-S3 with WiFi associated and the screen off. A 500mAh-class cell is therefore a handful of hours screen-on and most of a day screen-off. Measure at the PMIC rather than trusting this paragraph.

So **screen-on time is the budget, not uptime** — which suits an app that is idle 99% of the time: a few prompts an hour at most, each needing a few seconds of attention.

| State | Trigger | Wakes on | Screen | WiFi |
|---|---|---|---|---|
| Active | prompt firing, or touch within 20s | — | on | off (§8.1) |
| Idle | no touch for 20s, inside working hours | any touch | off | off |
| Dormant | outside the prompt window (§5.2) | RTC timer (next `workStart`) or BOOT press | off | off |

**Idle is the important one.** Nothing is polling the board, so the radio does not need to stay responsive — it only has to wake for NTP occasionally, and with the panel/radio constraint (§8.1) it can only do so with the panel off. Screen off is what matters, since the panel is the actual cost. A touch takes it straight back to Active.

```c
esp_pm_config_t pm = {
    .max_freq_mhz = 240,
    .min_freq_mhz = 40,
    .light_sleep_enable = true,       // wakes on timer or GPIO
};
ESP_ERROR_CHECK(esp_pm_configure(&pm));
```

**Deep sleep is the wrong tool for Idle**: it loses RAM state and costs a full boot, and Idle can happen dozens of times an hour. Use it only for Dormant, where the next event is hours away.

```c
static void enter_dormant(time_t wake_at) {
    if (wake_at > time(NULL)) {                     // guard: a past/zero wake_at would
        uint64_t us = (uint64_t)(wake_at - time(NULL)) * 1000000ULL;
        esp_sleep_enable_timer_wakeup(us);          // underflow into a garbage
    }                                               // multi-year timer
    // BOOT/GPIO0 is RTC-capable, so it can wake us (ext1; the _io variant on
    // newer IDF). PWR cannot — it reaches the ESP only through the expander,
    // which has no line to an RTC GPIO.
    esp_sleep_enable_ext1_wakeup(BIT64(GPIO_NUM_0), ESP_EXT1_WAKEUP_ANY_LOW);
    // Every write is already fsync'd (§3.4), so there is nothing to flush.
    esp_deep_sleep_start();
}
```

**Running flat mid-day writes nothing corrupt, but it does cost misses.** Every line is `fsync`ed before the tap is acknowledged (§3.4), so the board boots with the day file intact and the wall-clock time from the PCF85063; the slots that fell while it was off derive as missed (§3.1).

```c
static void on_power_event(axp2101_event_t ev) {
    switch (ev) {
    case AXP2101_VBUS_REMOVED:
        g_on_battery = true;
        ui_set_brightness(BRIGHTNESS_BATTERY);      // ~40% — biggest single saving
        idle_timeout_set(20 * 1000);
        break;
    case AXP2101_VBUS_INSERTED:
        g_on_battery = false;
        ui_set_brightness(BRIGHTNESS_MAINS);
        idle_timeout_set(60 * 1000);
        break;
    case AXP2101_BATT_LOW:                          // ~15%
        ui_show_battery_warning();                  // on the board's own screen
        break;
    case AXP2101_BATT_CRITICAL:                     // ~5%
        pmic_power_off();                           // AXP2101 register power-off
        break;
    }
}
```

Two wiring facts (V1 schematic) shape this code. First, `on_power_event` is fed by an I2C poll of the AXP2101 status registers on the 1 Hz tick, not by a GPIO interrupt — the PMIC's IRQ pin lands on the expander (`EXIO5`) and the expander's INT line goes nowhere. Second, that same fact is why critical-battery is a **PMIC power-off, not deep sleep**: nothing can wake a deep-sleeping ESP when USB power returns, so a board that deep-slept at 5% would sit dead-looking on a charger until someone pressed the key. A register power-off cuts the rails for microamps, and the AXP2101 powers back on from exactly the right triggers — a PWR press or USB insertion (confirm the power-on-source config at bring-up).

Battery goes in `/state` (§8) if that is ever built; there is no battery indicator on screen.

---

## 11. Build order

Firmware first, because it is the product and it is the long pole. Each step should end somewhere you can leave it. The three §15.3 spikes ran before step 1.

0. **The design loop** ✅ — `design/` renders board screens at 368×448 and screenshots them headlessly (`design/README.md`). Needs no hardware and no ESP-IDF, so design iteration is never blocked behind a toolchain. Settle what the screens look like here; §7 transcribes the result. Shipped mocks: `board-v4c`, `popup-v4`, `card-v4`, `stretch-v4`, `roll-v4`, `history-v4`, `icons`.
1. **Board bring-up** ✅ — ESP-IDF project, display, touch, LVGL, WiFi, NTP-set RTC. First act: read the revision label (§1) and run the vendor demo that matches it — V1 and V2 take different display and touch drivers.
2. **Config generation** ✅ — `actions.json` → `actions.g.h`, with the `check-gen` diff target.
3. **Storage** ✅ — LittleFS partition, `store_files.c`, `store_add_event` / `store_load_day` / `store_prune`, benchmarked on the board (§3.4).
4. **`derive`** ✅ — the walk, the anchor rule, the due window, and the §11.1 fixtures, all host-built and mutation-checked. This was the step to get right.
5. **Scheduler tick and the checklist card** ✅ (§6) — `card.c` host-tested and mutation-checked; the 1 Hz tick in `main.c` drives it. Testable on-desk by moving the RTC forward.
6. **Grid UI** ✅ — tiles, dots, wash and crest, the shared X (§7.3a).
7. **Inputs** ✅ — card rows, popup Done, history Undo (§7.4). Touch only.
8. **Sound** ✅ — the cue table, the I2S tone task, escalation (§9).
9. **Guided flow** ✅ — stretches and the shoulder roll (§7.6).
10. **History** ✅ — per action, from the popup (§7.7).
11. **Debug endpoints** — `GET /state`, `GET /logs` (§8, §14). Blocked on the panel/radio constraint (§8.1).
12. **Power states** — light sleep, brightness, PMIC events, Dormant (§10).

There is no step for a settings screen or a client — settings are `actions.json` plus a reflash (§2.1), and there is no client.

### 11.1 Test targets

Step 4 produces the bugs nobody notices for a week, and `derive` is a pure function of `(log, now, settings)` — so it compiles and runs on a laptop with no board attached. The flash-and-test loop is far too slow to iterate a slot-walk bug on hardware.

```
firmware/test/                  # host build: cc, no ESP-IDF, runs in milliseconds
  test_derive.c                 # every fixture below, plus hand-written scenarios for
                                #   the dot timeline, a snoozed open slot, and undo
  test_card.c                   # the §6 card state machine against a recorder host
  fixtures/derive/*.json        # the cases (generated into fixtures.g.h)
```

`test_derive`, `test_card` and their `.dSYM` directories are build products and gitignored.

```
fixtures/derive/
  interval-done-early.json      re-anchors the next slot from the tap
  fixed-done-early.json         13:00 lunch stays 13:00, and does not loop
  unanswered-past-slot.json     window open = due, window closed = missed
  snoozed-not-due.json          snoozed inside the window is neither
  duplicate-event.json          a twice-logged answer still counts once
  replay-idempotent.json        the doubled log derives identically
  card-confirm-partial.json     confirming checked rows snoozes the unchecked ones
  stretch-priority.json         a due stretch pre-empts the checklist
  dst-forward.json              BST day: slots hold their wall-clock time
  dst-day-before.json           the GMT control for the pair
```

`card-confirm-partial` is the case that would silently regress if `card_confirm` and `derive` ever disagree about which rows are still open; idempotence under replay is what makes a double-tap on Log harmless; and DST is the bug that will otherwise appear twice a year and be impossible to reproduce.

The DST pair only works because its expected epochs are hand-computed constants rather than recorded output — lunch is 82,800s apart across the changeover, not 86,400s, and an implementation doing UTC arithmetic by hand passes one fixture and fails the other. `fixtures/derive/README.md` states that rule and the assertion semantics.

**A suite that passes on its first run has not been shown to work.** These were verified by mutation — the anchor rule dropped, the due window removed, `tm_isdst` forced to 0, the sort made unstable, snooze ignored — and each one is caught. Re-run that check after any change to the walk; the table in the fixtures README lists the mutations.

### 11.2 Day rollover

Handled in the tick (§5.2), not on boot: the board runs for weeks at a time, so midnight is the common case rather than an edge case. There is no "reset the timers" step — `next` derives from an empty log against today's date, so a fresh day is an empty struct. Retention (§3.4) runs in the same branch.

---

## 12. Known constraints

- **Single point of failure.** The board is the product, and the only client of itself. If it is dead, there is no tracker and no way to view the data short of pulling the flash. Accepted deliberately.
- **Firmware dev loop.** Flash-and-test is slow. §11.1's host-compiled tests for `derive` and the card take most of the sting out, `design/` (§11 step 0) removes the hardware from the design loop, and `make -C sim grid` runs the real LVGL UI on the laptop.
- **No client, by choice.** No phone app, no browser page, no remote logging. If you are not at the board, nothing happens. §1 has the reasoning; §8.2 has the exit if that stops being true.
- **No reach away from the desk.** If you are not at the board, it does not prompt you, and it does not tell your phone.
- **A board that was off accumulates real misses.** Simplified from an earlier draft that tracked presence explicitly (§3.1). Accepted for the simplicity; §10's USB-C recommendation keeps it rare.
- **Clock.** SNTP at boot when WiFi is configured and reachable, PCF85063 otherwise, the build time as a last resort. The RTC runs off the AXP2101's RTC rail with the main battery behind it — the dedicated backup-cell pads ship empty — so time survives reboots and unplugs while any battery is attached, and is lost when all power is removed. A board that then boots without ever seeing NTP has a wrong date, and the day key is wrong with it. Nothing on screen shows the date; the boot log reports whether SNTP synced or the clock stayed on its seed.

---

## 13. History, and what the storage change costs

This section used to be "What SQLite buys" and argued the case at length: dedupe as a constraint, retention as one `DELETE`, WAL surviving a power cut, history as queries. Spike S2 (§3.4) measured those claims on the board and two of them were false — WAL is unavailable on this port, and inserts ran at ~3s each before the database reached a tenth of its retention size. The reasoning it contained still applies: prefer the thing that removes code, and keep the data cheap enough that history stays possible.

### 13.1 What actually carried over

The load-bearing claim was never SQL, it was §3.1's model: an append-only log of uniquely-keyed events, with everything else derived. That model is untouched. What changed is only how bytes reach flash.

| Was going to be | Is |
|---|---|
| `UNIQUE (action, slot)` in the schema | latest event per `(action, slot)` in the open day's RAM copy (§3.4) — at most ~30 comparisons |
| `DELETE FROM events WHERE day < …` | `unlink` per file, ISO names sorting lexically |
| WAL surviving a power cut | `fsync` per line; a torn write loses at most the last line, never the file |

The dedupe case is the one that got weaker and the one to watch. As a constraint the database refused a duplicate no matter which code path reached it, including paths nobody had written yet. As a rule in code it can be broken by a future edit — which is why `duplicate-event.json` and `replay-idempotent.json` (§11.1) exist and why they are mutation-checked.

### 13.2 Where the line is

**Storage holds facts, `derive` holds meaning.** There is no query language, so the slot walk cannot be pushed into a recursive CTE. `derive` measured at 0.77ms over a full day on the board, against a 1 Hz tick.

### 13.3 What history looks like now

Retention is 400 days, because the data is tiny: ~30 lines/day at ~50 bytes is ~1.5KB/day, so 400 days is ~600KB against a 10MB partition.

Today's history is on the board, per action (§7.7). Anything across days is a laptop script over the day files: pull `/fs/d/` off the LittleFS partition and `grep -c 'water\tdone'` per file. That is a worse developer experience than one `GROUP BY` and a better one than nothing. The question §13.3 originally raised — "you skip your 15:30 snack four days in five" is a fact about the schedule being wrong, not about the person — is still answerable, on a laptop over pulled files rather than on the device.

If §8 is ever built, `GET /logs/<day>` alongside it makes the pull a `curl`. Whether anything renders it is a separate decision.


## 14. Device logs

**Not built.** Today the answer to "why did it crash at 3am" is the serial console, which was scrolled past hours ago.

The design: **a rolling 24-hour buffer, verbose, written as things happen.** State transitions, WiFi association and drops, watchdog resets, store errors — anything worth an `ESP_LOG` call is worth keeping past the point the serial console scrolled past it.

```c
// firmware/main/devlog.c
// Two files, append + rotate — not an in-place byte ring. LittleFS is
// copy-on-write: appends are what it is good at, and rewriting the middle
// of a file (which is what a ring does all day) costs block copies and
// flash wear for nothing. Diagnostic scratch, not data the product depends
// on being correct.
#define DEVLOG_PATH   "/fs/devlog.0"       // current; renamed to .1 when full
#define DEVLOG_ROTATE (128 * 1024)         // ×2 files ≈ 24h of verbose logging

void devlog_write(const char *tag, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char line[256];
    int n = snprintf(line, sizeof(line), "%lld [%s] ", (long long)time(NULL), tag);
    n += vsnprintf(line + n, sizeof(line) - n, fmt, ap);
    va_end(ap);

    rotate_append(DEVLOG_PATH, DEVLOG_ROTATE, line, n);   // .0 → .1 at the cap
    ESP_LOGI(tag, "%s", line);                            // still visible over serial
}
```

Every call site that does `ESP_LOGx(...)` on something worth remembering becomes `devlog_write(...)`, which does both — visible on serial live, and readable afterward (over §8's `GET /logs` if built, or off the partition) for the 3am reboot nobody watched. Rotation is capped and automatic so this can be sprinkled liberally without a slow flash-fill-up turning into its own bug.

---

## 15. Risks

§11 orders the build; this section names what could sink it, what retires each risk, and the fallback where one exists. Three half-day spikes (§15.3) came before the full build order — each exists to kill the risks marked with it.

### 15.1 The register

| # | Risk | Odds | Pain | Retired by |
|---|---|---|---|---|
| R1 | ~~SQLite insert latency degrades on LittleFS~~ — **fired.** ~3s/insert at 688 rows, WAL unavailable. Kill switch taken: one append-only file per day (§3.4) | — | — | **closed by S2** |
| R2 | ~~Board in hand is V2~~ — **closed.** The board is V1 (SH8601 + FT5x06, read from the factory firmware's strings) | — | — | **closed** |
| R3 | ~~AXP2101 rail misconfig~~ — **closed.** Vendor BSP configures the rails; panel lit at 368x448 | — | — | **closed by S1** |
| R4 | ~~Codec/PA chain~~ — **closed.** ES8311 opens via `esp_codec_dev`, §9 cues play through the amp gate | — | — | **closed by S3** |
| R5 | Light sleep breaks a peripheral (touch wake, I2S after wake, tick cadence) | medium | medium | spike S3, §11 step 12 |
| R6 | TZ/DST wrong — POSIX string, no zoneinfo on the board | low | high | §2.1 `tz` + `dst-forward` fixture |
| R7 | ~~LVGL effects miss frame budget~~ — **closed.** Wash costs 2.4ms at 1 Hz; the animated crest costs 555us/frame at 30fps (1.7% of budget). The cliff is not the effect, it is `clip_corner` — see below | — | — | **closed** |
| R8 | ~~WiFi and the QSPI panel coexisting~~ — **closed by design.** The radio runs to completion and is stopped before the panel powers (§8.1) | — | — | **closed** |

**R1** was the one that could force a design change, which is why it was benchmarked before the build started. It did: `store_add_event` / `store_load_day` were the entire storage API, so the file backend replaced SQLite underneath them and `derive` and everything above it never knew.

**R2:** both revisions have official drivers — `espressif/esp_lcd_sh8601` for V1, `espressif/esp_lcd_co5300` for V2, FT3168 via the `ft5x06` touch driver family, CST820 via `esp_lcd_touch_cst816s`/`kodediy cst820` — so this is a *selection* problem, not a porting project. The risk is writing code against one before reading the label on the other. §11 step 1 orders it: label first, vendor demo second (Waveshare's `waveshareteam/ESP32-S3-Touch-AMOLED-1.8` repo), product code last.

**R3:** the display and codec rails hang off AXP2101 LDOs, so a wrong PMIC init is a *dark screen*, not an error message — bring-up must copy the vendor demo's rail config before trusting any of §10. Driver: `XPowersLib` (upstream has an ESP-IDF example; packaged as `cube32esp/xpowerslib`).

**R6:** the fixture exists (`dst-forward.json`, §11.1); the config carries the POSIX rule string (§2.1, §3.1a). The residual risk is authoring the string wrong once, which shows up as every slot landing an hour off and the boot log's reported time disagreeing with the wall clock.

### 15.2 Scaffolding

```
# firmware/partitions.csv — 16MB flash
nvs,      data, nvs,      0x9000,  24K
phy_init, data, phy,      0xf000,  4K
factory,  app,  factory,  0x10000, 4M
storage,  data, littlefs, ,        10M
```

No OTA pair: settings changes are a reflash over the USB-C cable the board lives on (§2.1, §10), so A/B slots buy nothing but lost flash.

```yaml
# firmware/main/idf_component.yml
dependencies:
  idf: ">=5.5,<6.1"
  joltwallet/littlefs: "^1.16"
  waveshare/esp32_s3_touch_amoled_1_8:
    version: "~1.1.4"        # 1.x is the V1 line (SH8601 + FT5x06); 2.x drives V2 and aborts here
    public: true
```

The BSP brings in LVGL 9, the SH8601 and FT5x06 drivers, `esp_codec_dev` and the expander driver. WiFi credentials ride the same path as the action table — `config/secrets.json`, gitignored, generated into `wifi.g.h` by the same `gen-config` step.

### 15.3 Three spikes, then the build order

- **S1 — panel, touch, PMIC.** ✅ **Passed.** `spikes/s1-board` draws the §7 grid at real size on the panel, with touch and the §9 cues wired. Panel lights at 368x448; touch reports through LVGL and **every tap landed inside the tile it named** — checked against the layout rectangles, so the coordinate mapping has no rotation or mirroring error despite the BSP's `sw_rotate`. R2 and R3 closed.
- **S2 — storage under load (half day).** ✅ **Run, and it triggered the kill switch.** SQLite managed ~3s per insert at 688 rows with WAL unavailable; the file backend does 18.9ms p50, flat from 3k to 15k rows. Full numbers in §3.4. The stated pass bar was p99 < 50ms and the file backend does not meet it either — p99 is 558ms of flash garbage collection — but it does not degrade, which was the property that actually mattered. The residual p99 question is stated in §3.4 and left open deliberately.
- **S3 — sound and sleep.** ◐ Sound done in the same flash as S1: ES8311 opens through the BSP's I2C and plays §9's bloom/success/ready with the real envelope. R4 closed. **The sleep half is not done** — light sleep, wake-on-touch and tick cadence remain, so R5 is still open.

**Three findings from S1.**

1. **The vendor BSP is versioned by board revision, and the default is the wrong one.** `waveshare/esp32_s3_touch_amoled_1_8` 2.x drives CO5300 + CST816S — V2 hardware — and on a V1 board it initialises a CO5300 against an SH8601 panel and then aborts on `Touch not found`. The 1.x line (`~1.1.4`) is the V1 BSP. Pin the major version to the board revision; `^2` on a V1 board is a crash, not a warning.
2. **The V1 BSP creates the I/O expander and then never drives a pin of it — and three panel-critical lines hang off that expander.** `bsp_io_expander_init()` returns a handle; nothing in the BSP calls `set_dir` or `set_level`. So unless the application does it:

   | Expander pin | Line | Consequence of leaving it |
   |---|---|---|
   | EXIO0 | `LCD_RESET` | controller never released from reset |
   | EXIO1 | `DSI_PWR_EN` | **panel has no VCI — the screen stays black** |
   | EXIO2 | `TP_RESET` | touch never answers; `bsp_display_start()` aborts in `bsp_touch_new` |

   The panel-power one does not look like a failure: `sh8601: LCD panel create success`, `panel up: 368x448`, brightness accepted, every SPI write returning `ESP_OK` — into a display with no power. There is no error anywhere to grep for. Drive EXIO1 high, pulse EXIO0 and EXIO2 low-then-high, *then* call `bsp_display_start()`.

   An I2C scan is what separates "touch absent" from "touch in reset" — the driver error is identical either way:

   ```
   i2c scan (before expander):    0x18 0x20 0x34 0x51 0x6B        ES8311, TCA9554, AXP2101, PCF85063, QMI8658
   i2c scan (after touch reset):  0x18 0x20 0x34 0x38 0x51 0x6B   + FT3168
   ```

   That scan is the cheapest board-health check there is; keep it in bring-up permanently.

3. **`clip_corner` is the frame-budget cliff, not the animation.** Clipping children to a rounded corner makes LVGL allocate a mask layer and blend the tile per-pixel on every redraw. At the 1 Hz tick that is invisible; at 30fps it pinned the LVGL task hard enough to starve the idle task and trip the task watchdog — with a backtrace deep in `lv_draw_layer_alloc_buf` that says nothing about corners. Removing it took the animated crest to **555us/frame, 1.7% of a 33ms budget**, stable over thousands of frames. On this panel, per-pixel masking is the expensive thing, and moving geometry is nearly free.

The spikes ran before §11 step 1 so that a failed one changed the plan while the plan was still cheap to change.
