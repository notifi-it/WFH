# WFH Health Tracker — Implementation Plan

Status: UX prototype built and reviewed. This document is the spec for the real build.

---

## 1. Scope

A square-format habit tracker that prompts seven actions on independent timers during working hours, chimes and vibrates, and logs one tap per completion. Today-at-a-glance only, no historical views in v1.

Two phases:

- **Phase 1** — browser app, local state, source of truth on the device it runs on.
- **Phase 2** — ESP32-S3 AMOLED board becomes the scheduler and source of truth; the browser app becomes a viewer and remote.

The data model is identical in both, so nothing built in Phase 1 is discarded.

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

**Explicitly out of scope:** eye breaks (20-20-20), the lunchtime walk, and any "silence for the day" control. All three were considered and cut.

### 2.1 Action definitions as data

The whole app is driven off one table. Adding an action must never mean touching the scheduler.

```ts
// src/config/actions.ts
export type ActionId =
  | 'stand' | 'water' | 'roll' | 'snack' | 'lunch' | 'stretch' | 'shutdown';

export type Cadence =
  | { kind: 'interval'; everyMin: number; startOffsetMin: number }
  | { kind: 'fixed'; times: string[] };   // "HH:MM", local time

export interface ActionDef {
  id: ActionId;
  name: string;
  blurb: string;                 // one line shown on the prompt card
  icon: string;                  // akar-icons name
  tint: string;                  // hex
  target: number;                // daily target = number of dots
  cadence: Cadence;
  flow: 'tap' | 'stretch';
  priority: number;              // higher wins a collision; see §5
}

export const ACTIONS: ActionDef[] = [
  { id: 'stand',    name: 'Stand break',   blurb: 'Up on your feet for a minute.',
    icon: 'person-check', tint: '#7fd4a8', target: 10, flow: 'tap', priority: 1,
    cadence: { kind: 'interval', everyMin: 40, startOffsetMin: 12 } },

  { id: 'water',    name: 'Water',         blurb: 'Glass of water. Refill while you are up.',
    icon: 'drop',         tint: '#6ec3e0', target: 8,  flow: 'tap', priority: 1,
    cadence: { kind: 'interval', everyMin: 45, startOffsetMin: 7 } },

  { id: 'roll',     name: 'Shoulder roll', blurb: 'Ten slow rolls back, then drop the shoulders.',
    icon: 'refresh',      tint: '#b6a3e8', target: 8,  flow: 'tap', priority: 1,
    cadence: { kind: 'interval', everyMin: 60, startOffsetMin: 3 } },

  { id: 'snack',    name: 'Snack',         blurb: 'Something small before the slump.',
    icon: 'apple',        tint: '#e8b06a', target: 2,  flow: 'tap', priority: 2,
    cadence: { kind: 'fixed', times: ['10:45', '15:30'] } },

  { id: 'lunch',    name: 'Lunch',         blurb: 'Away from the desk.',
    icon: 'bowl',         tint: '#e8926a', target: 1,  flow: 'tap', priority: 2,
    cadence: { kind: 'fixed', times: ['13:00'] } },

  { id: 'stretch',  name: 'Stretches',     blurb: 'Four stretches, about three minutes.',
    icon: 'sun',          tint: '#e0d16a', target: 2,  flow: 'stretch', priority: 3 },
  // cadence: { kind: 'fixed', times: ['11:30', '16:30'] }

  { id: 'shutdown', name: 'Shut down',     blurb: 'Close the laptop. That is the day.',
    icon: 'moon',         tint: '#8f9aa8', target: 1,  flow: 'tap', priority: 2,
    cadence: { kind: 'fixed', times: ['18:00'] } },
];

export const BY_ID = Object.fromEntries(ACTIONS.map(a => [a.id, a])) as
  Record<ActionId, ActionDef>;
```

`startOffsetMin` is the jitter from §4 expressed as data: stand fires at 09:12, water at 09:07, roll at 09:03. The three interval actions now collide far less often than they would from a common 09:00 anchor.

```ts
// src/config/stretches.ts
export interface StretchStep { name: string; seconds: number; cue: string; }

export const STRETCH_SET: StretchStep[] = [
  { name: 'Chin tucks',           seconds: 30, cue: 'Draw the chin straight back. Hold, release, repeat.' },
  { name: 'Doorway pec stretch',  seconds: 40, cue: 'Forearms on the frame, step through, chest open.' },
  { name: 'Cat-cow',              seconds: 40, cue: 'On all fours, arch and round with the breath.' },
  { name: 'Hip flexor stretch',   seconds: 50, cue: 'Half kneel, tuck the pelvis, 25s each side.' },
];

export const STRETCH_TOTAL_SEC = STRETCH_SET.reduce((n, s) => n + s.seconds, 0); // 160
```

---

## 3. Data model

**Revised from the draft above.** The original shape stored `counts`, `skipped`, `missed` and `next` alongside `events`. Four of those five fields are derivable from the fifth, and every one of them is a field that can drift out of agreement with the event log. This section replaces them with derivation. The reasoning is in §3.1 and it is what makes the battery question in §10.3 answerable at all.

### 3.1 What is actually true

Three things are facts. Everything else is a view over them.

1. **The schedule** — config, in `ACTIONS`. Not per-day data.
2. **The event log** — what the user did. Append-only. `done` and `skip`, nothing else.
3. **The awake windows** — the periods the app was actually running and able to prompt.

The load-bearing idea: **a miss is not an event, it is an absence.** Nothing happens when a slot is missed — that is what missing it means. So there is nothing to write. A slot is missed if it is in the past, it fell inside an awake window, and no `done` or `skip` answers it.

Deriving it rather than writing it buys three things:

- It cannot drift. There is no second copy to disagree with the log.
- **Backfill disappears entirely.** §6.4 agonised over whether to mark slots missed on load, because writing a miss is a commitment. Derived, the question answers itself: slots that passed while nothing was running aren't in an awake window, so they aren't missed. No repair pass, no setting.
- **A board that was asleep, or a laptop that was shut, is just a gap in `awake`.** No special case. This is the whole answer to §10.3.

Storing misses would mean a board that ran flat overnight wakes up and writes forty misses for slots nobody was ever prompted about. Deriving them means it wakes up and writes nothing, because nothing happened.

```json
{
  "version": 2,
  "date": "2026-08-14",
  "events": [
    { "id": "…", "action": "water", "kind": "done", "ts": 1723645210, "slot": 1723645020 },
    { "id": "…", "action": "stand", "kind": "skip", "ts": 1723646400, "slot": 1723646400 }
  ],
  "awake": [ [1723640400, 1723645210], [1723649000, 1723652600] ],
  "snoozedUntil": { "stand": 1723647300 }
}
```

Storage keys:

- `wfh:day:YYYY-MM-DD` — one key per day: event log plus awake windows
- `wfh:settings` — working hours, per-action cadences, sound and haptics on/off

One append per completion. Reads on mount only.

```ts
// src/state/types.ts
import type { ActionId, Cadence } from '../config/actions';

export type EventKind = 'done' | 'skip';        // no 'miss' — see above

export interface LogEvent {
  id: string;          // crypto.randomUUID()
  action: ActionId;
  kind: EventKind;
  ts: number;          // epoch seconds — when the user tapped
  slot: number;        // epoch seconds — which slot this answers; dedupe key
}

export type Awake = [start: number, end: number][];

/** Everything persisted for one day. Facts only. */
export interface DayLog {
  version: 2;
  date: string;                                   // YYYY-MM-DD, local
  events: LogEvent[];
  awake: Awake;
  snoozedUntil: Partial<Record<ActionId, number>>; // transient, but survives reload
}

/** Everything rendered. Derived on demand, never stored. */
export interface DayView {
  counts:  Partial<Record<ActionId, number>>;
  skipped: Partial<Record<ActionId, number>>;
  missed:  Partial<Record<ActionId, number>>;
  next:    Partial<Record<ActionId, number>>;
  due:     ActionId[];                            // slots due right now
}

export interface Settings {
  version: 2;
  workStart: string;    // "09:00"
  workEnd: string;      // "18:00"
  cadences: Partial<Record<ActionId, Cadence>>;   // overrides ACTIONS defaults
  sound: boolean;
  haptics: boolean;
}
```

`slot` is the field that makes the whole thing idempotent. Two tabs, a replayed request, a board and a browser both logging the same prompt — all collapse to one entry because `(action, slot)` is unique. It is also what lets a miss be derived: it is the join key between the schedule and the log.

`snoozedUntil` is the one piece of non-derivable mutable state, because "the user asked for quiet at 11:03" is a fact the log doesn't otherwise record. It is small, it self-expires, and nothing breaks if it is lost.

### 3.2 Derivation

One pass per action, walking that action's slots forward through the day and folding in the events that answer them. Seven actions, at most ~30 slots each, called once per 10s tick — this is a few hundred comparisons, which is nothing. There is no performance argument for caching it.

```ts
// src/state/derive.ts
export function derive(log: DayLog, now: number, s: Settings, day = new Date()): DayView {
  const view: DayView = { counts: {}, skipped: {}, missed: {}, next: {}, due: [] };

  // Index the log once: (action, slot) → kind
  const answered = new Map<string, EventKind>();
  for (const e of log.events) answered.set(`${e.action}@${e.slot}`, e.kind);

  for (const def of ACTIONS) {
    let slot = firstSlot(def, s, day);
    let anchor: number | null = null;      // set when an interval action is done early

    while (slot != null) {
      const kind = answered.get(`${def.id}@${slot}`);

      if (kind === 'done') {
        view.counts[def.id] = (view.counts[def.id] ?? 0) + 1;
        // Interval actions reset from the tap, so the next slot moves. §4.3
        if (def.cadence.kind === 'interval') {
          anchor = eventTs(log, def.id, slot)!;
        }
      } else if (kind === 'skip') {
        view.skipped[def.id] = (view.skipped[def.id] ?? 0) + 1;
      } else if (slot <= now) {
        // Unanswered and in the past. Missed only if we were awake to ask.
        if (wasAwake(log.awake, slot)) {
          view.missed[def.id] = (view.missed[def.id] ?? 0) + 1;
        }
        // else: nothing was running, nobody was prompted, so it is not a miss.
      } else {
        // First future slot. This is `next`, and we stop here.
        view.next[def.id] = slot;
        break;
      }

      if (slot <= now && !kind && isDueNow(slot, now, log.snoozedUntil[def.id])) {
        view.due.push(def.id);
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

The one subtlety is `anchor`. §4.3's rule — an interval action done early resets its timer from the tap — means the slot grid for `stand` is not a fixed lattice; it bends every time you get ahead. Folding the anchor forward during the walk reproduces exactly the sequence the live scheduler produced, which is what makes the derived view agree with what the user actually saw.

### 3.3 Awake windows

Written by the same tick that drives everything else. Extend the current window if the last tick was recent; otherwise open a new one, because a gap means the app was not running.

```ts
// src/state/awake.ts
const GAP = 120;   // seconds; > 1 tick of throttling, < the shortest cadence

export function markAwake(awake: Awake, now: number): Awake {
  const last = awake[awake.length - 1];
  if (last && now - last[1] <= GAP) {
    return [...awake.slice(0, -1), [last[0], now]];    // extend
  }
  return [...awake, [now, now]];                       // new window after a gap
}
```

`GAP` at 120s is the judgement call. Too tight and a throttled background tab fragments into hundreds of windows and starts reporting phantom misses for slots it *was* awake for. Too loose and a genuinely-closed laptop looks awake. Two minutes clears the ~60s background clamp with margin while staying well under the 40-minute shortest cadence.

The array stays small — a normal day is one to five windows — and it compresses naturally because extension mutates the last entry rather than appending.

### 3.4 Storage layer

```ts
// src/state/storage.ts
import type { DayLog, LogEvent, Settings } from './types';

const DAY = (d: string) => `wfh:day:${d}`;
const SETTINGS = 'wfh:settings';

export const todayKey = (at = new Date()): string => {
  const p = (n: number) => String(n).padStart(2, '0');
  return `${at.getFullYear()}-${p(at.getMonth() + 1)}-${p(at.getDate())}`;
};

export const emptyDay = (date: string): DayLog =>
  ({ version: 2, date, events: [], awake: [], snoozedUntil: {} });

export function loadDay(date: string): DayLog {
  try {
    const raw = localStorage.getItem(DAY(date));
    if (!raw) return emptyDay(date);
    const parsed = JSON.parse(raw);
    return parsed.version === 2 ? parsed as DayLog : migrateDay(parsed);
  } catch {
    return emptyDay(date);      // corrupt key: start clean rather than crash
  }
}

export function saveDay(log: DayLog): void {
  try {
    localStorage.setItem(DAY(log.date), JSON.stringify(log));
  } catch {
    // QuotaExceededError: drop the oldest days and retry once.
    pruneDays(14);
    try { localStorage.setItem(DAY(log.date), JSON.stringify(log)); } catch { /* give up */ }
  }
}

/**
 * The only write path for user actions. Idempotent on (action, slot):
 * a retry, a second tab, or the board and the browser both logging the
 * same prompt all collapse to one entry.
 */
export function appendEvent(log: DayLog, ev: LogEvent): DayLog {
  if (log.events.some(e => e.action === ev.action && e.slot === ev.slot)) return log;
  const next = { ...log, events: [...log.events, ev] };
  saveDay(next);
  return next;
}

export function pruneDays(keep: number): void {
  const keys = Object.keys(localStorage)
    .filter(k => k.startsWith('wfh:day:'))
    .sort();                                  // ISO dates sort lexically
  keys.slice(0, Math.max(0, keys.length - keep)).forEach(k => localStorage.removeItem(k));
}

export const loadSettings = (): Settings =>
  ({ ...DEFAULT_SETTINGS, ...safeParse(localStorage.getItem(SETTINGS)) });

export const saveSettings = (s: Settings): void =>
  localStorage.setItem(SETTINGS, JSON.stringify(s));
```

Note the `version` field and the `migrateDay` hook. Phase 2 will add fields (a `source: 'web' | 'board'` on each event, at minimum) and yesterday's keys must not become unreadable.

**Cross-tab coherence.** An append-only log of uniquely-keyed events merges cleanly, so two tabs no longer need last-write-wins — union the event lists and the result is correct regardless of who wrote last:

```ts
window.addEventListener('storage', e => {
  if (e.key !== `wfh:day:${todayKey()}` || !e.newValue) return;
  const theirs = JSON.parse(e.newValue) as DayLog;
  dispatch({ type: 'merge', log: theirs });
});

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

This is the second thing the model revision buys. A shape whose only mutation is "append an event with a unique key" is a CRDT by construction — union is the merge function, and it is commutative and idempotent, so it does not matter what order the tabs write in or how many times an event arrives. The same `merge` is what reconciles the browser with the board in §10.2.

---

## 4. Scheduler

- Single interval at 10s resolution. Not one timer per action.
- Each action holds `nextDue` and an optional `snoozedUntil`.
- On tick, for each action: if `now >= nextDue` and inside working hours, enqueue it.
- **Doing an action early resets its timer from now**, so getting ahead pushes the next slot out rather than being penalised.
- **Missed slots roll forward one full interval and do not stack.** No catch-up queue, no overdue badge.
- **Jitter the start offsets** so the three interval-based actions rarely align. Water starts at 09:07, shoulder roll at 09:03, and so on.

### 4.1 Slot arithmetic

```ts
// src/scheduler/slots.ts
export const atTime = (day: Date, hhmm: string): number => {
  const [h, m] = hhmm.split(':').map(Number);
  const d = new Date(day); d.setHours(h, m, 0, 0);
  return Math.floor(d.getTime() / 1000);
};

export const inWorkingHours = (now: number, s: Settings, day = new Date()): boolean =>
  now >= atTime(day, s.workStart) && now < atTime(day, s.workEnd);

/** First slot of the day for an action. */
export function firstSlot(def: ActionDef, s: Settings, day = new Date()): number | null {
  const start = atTime(day, s.workStart);
  if (def.cadence.kind === 'interval') {
    return start + def.cadence.startOffsetMin * 60;
  }
  const next = def.cadence.times.map(t => atTime(day, t)).sort((a, b) => a - b)[0];
  return next ?? null;
}

/** The slot after `from`, or null if the action has no more slots today. */
export function slotAfter(def: ActionDef, from: number, s: Settings, day = new Date()): number | null {
  const end = atTime(day, s.workEnd);
  if (def.cadence.kind === 'interval') {
    const next = from + def.cadence.everyMin * 60;
    return next < end ? next : null;
  }
  const next = def.cadence.times.map(t => atTime(day, t)).sort((a, b) => a - b)
    .find(t => t > from);
  return next ?? null;
}
```

Everything is epoch seconds off the local clock. Do not store slot times as strings and do not do date maths in UTC — a user in a DST-shifting zone would see the whole day slide by an hour on the changeover, and `setHours` on a local `Date` handles that correctly for free.

### 4.2 The tick

One `setInterval` at 10s. The tick is a pure function of `(state, now)` so it can be unit-tested by feeding it timestamps, with no fake timers.

The tick no longer computes tallies or rolls anything forward — `derive` does that. All the tick does is mark the app awake and report which actions became due since the last pass.

```ts
// src/scheduler/tick.ts
export interface TickResult {
  log: DayLog;
  enqueue: ActionId[];      // newly due, in priority order
  view: DayView;
}

export function tick(log: DayLog, now: number, s: Settings, showing: ActionId[]): TickResult {
  const marked = { ...log, awake: markAwake(log.awake, now) };
  if (!inWorkingHours(now, s)) {
    return { log: marked, enqueue: [], view: derive(marked, now, s) };
  }

  const view = derive(marked, now, s);

  // Due, not already on screen, not snoozed. `derive` has already excluded
  // anything answered, so there is no double-queueing to guard against and
  // no miss to record — an unanswered slot simply stays unanswered and
  // becomes a derived miss the moment its successor comes due.
  const enqueue = view.due.filter(id =>
    !showing.includes(id) && (marked.snoozedUntil[id] ?? 0) <= now);

  return { log: marked, enqueue, view };
}
```

Two things fall out that previously needed explicit code:

- **"Missed slots roll forward and do not stack"** is no longer a rule the tick enforces; it is a consequence of the walk in `derive`, which visits each slot exactly once. There is no path that can double-count.
- **The awake window written on every tick is the same write that makes misses correct.** Marking presence and detecting absence are the same mechanism, so they cannot disagree.

`showing` is passed in from the queue rather than read off the log — the queue is UI state, not persisted state, and a reload should not resurrect yesterday's unanswered card.

### 4.3 Completing early

Logging is now just an append. The rolling-forward lives in `derive`'s walk, which reads the `done` event's `ts` and re-anchors from it.

```ts
export function logDone(log: DayLog, id: ActionId, now: number, view: DayView): DayLog {
  // The slot being answered: the one currently due, or the next one if the
  // user tapped the tile ahead of the prompt.
  const slot = view.due.includes(id) ? currentSlot(view, id) : view.next[id] ?? now;
  return appendEvent(log, { id: crypto.randomUUID(), action: id, kind: 'done', ts: now, slot });
}
```

The asymmetry between the two cadence kinds is unchanged, it has just moved into `derive`: `anchor` is only set for `interval` actions, so drinking water at 11:20 pushes the next glass to 12:05, while eating lunch at 12:40 leaves the 13:00 slot exactly where it was. It is the one branch where the two kinds must not share a path, and having it in one place rather than at every call site is the point.

### 4.4 Background throttling

`setInterval` in a hidden tab is clamped to roughly once a minute, and Safari can suspend it entirely. The tick is timestamp-driven rather than count-driven, so no state is lost — but prompts arrive late. Two mitigations, both cheap:

```ts
document.addEventListener('visibilitychange', () => {
  if (document.visibilityState === 'visible') runTick(Date.now() / 1000 | 0);
});
```

and, for the tab that *is* open but backgrounded, hoist the interval into a Web Worker, which is throttled far less aggressively:

```js
// src/scheduler/ticker.worker.js
setInterval(() => postMessage(Date.now()), 10_000);
```

Neither closes the gap fully. That is what Phase 2 is for.

---

## 5. Prompt queue

Collisions are common: stand (40m), water (45m) and roll (60m) align exactly every 6 hours and near-align constantly.

- Prompts **queue**; only one card is visible at a time.
- The card shows `1 OF n` when more are waiting.
- **Done** logs the action and advances to the next in the queue.
- **Skip this one** logs an explicit skip for that action only, rolls it forward, advances.
- **Delay all 15 min** clears the entire queue. A meeting blocks everything, so one tap covers all of it. Repeatable, never stacks.
- **X (top right)** dismisses the whole queue and returns to the grid. Present on every overlay including mid-stretch.

Stretches never merge into a collision group. If stretches collide with anything, stretches take priority and the rest roll forward.

### 5.1 Reducer

```ts
// src/state/queue.ts
export interface QueueState { queue: ActionId[]; }

export type QueueAction =
  | { type: 'enqueue'; ids: ActionId[]; now: number }
  | { type: 'done'; now: number }
  | { type: 'skip'; now: number }
  | { type: 'delay-all'; now: number }
  | { type: 'dismiss' };

export function reduce(q: QueueState, a: QueueAction, log: DayLog, view: DayView, s: Settings) {
  switch (a.type) {
    case 'enqueue': {
      // Stretches pre-empt: they take the head on their own. Everything else
      // is dropped from the queue rather than rolled forward — an unanswered
      // slot needs no bookkeeping now that misses are derived, and it will be
      // re-raised on the next tick if it is still due after the stretch set.
      const stretch = a.ids.filter(id => BY_ID[id].flow === 'stretch');
      if (stretch.length) return { q: { queue: stretch }, log };

      const fresh = a.ids.filter(id => !q.queue.includes(id));   // never double-queue
      return { q: { queue: [...q.queue, ...fresh] }, log };
    }

    case 'done':
      return { q: { queue: q.queue.slice(1) }, log: logDone(log, q.queue[0], a.now, view) };

    case 'skip':
      return { q: { queue: q.queue.slice(1) }, log: logSkip(log, q.queue[0], a.now, view) };

    case 'delay-all': {
      // One tap covers the whole meeting. Snoozing is absolute, not additive,
      // so mashing the button never pushes anything past now + 15.
      const until = a.now + 15 * 60;
      const snoozedUntil = { ...log.snoozedUntil };
      q.queue.forEach(id => { snoozedUntil[id] = until; });
      return { q: { queue: [] }, log: { ...log, snoozedUntil } };
    }

    case 'dismiss':
      // Not a skip and not a miss. The user closed the card; nothing is
      // written, and the slots re-raise on the next tick if still due.
      return { q: { queue: [] }, log };
  }
}
```

`delay-all` sets `snoozedUntil` to an absolute timestamp rather than adding 15 minutes to an existing snooze. That is what "repeatable, never stacks" means in code.

**One edge to be aware of.** A snoozed slot that passes unanswered still derives as a miss, because `derive` only knows the *current* `snoozedUntil`, not that the slot was snoozed at the time. With the shortest cadence at 40 minutes a 15-minute delay cannot span a slot boundary, so this is unreachable as configured. It becomes reachable if §8's settings panel lets a cadence go below ~15 minutes — at which point the fix is to log the snooze as an event (`kind: 'delay'`, with the slot it covered) rather than to special-case `derive`. Worth a guard in the settings panel: floor the configurable cadence at 20 minutes.

`dismiss` deliberately logs nothing. The distinction the dot states draw in §6 is between *decided against* and *never answered*; closing a card is neither, so it must not consume the slot.

---

## 6. UI

Square container, `aspect-ratio: 1/1`, max ~540px.

### Grid (default)

- 7 tiles, 2 columns
- Each tile: Akar icon, name, progress dots, countdown in minutes
- **Countdown** renders as a slow tinted wash rising from the bottom of the tile as the slot approaches
- **Animated grain** overlay on each tile plus a fainter layer on the whole frame, drifting in 6 discrete steps so it reads as film grain rather than a sliding gradient
- Respects `prefers-reduced-motion`

### 6.1 Frame and tile

Seven tiles in two columns means one tile spans both. `shutdown` takes the full-width final row — it is the one action that ends the day, so the layout reads as a full stop.

```css
.frame {
  aspect-ratio: 1 / 1;
  max-width: 540px;
  margin-inline: auto;
  display: grid;
  grid-template-columns: 1fr 1fr;
  grid-auto-rows: 1fr;
  gap: 10px;
  padding: 10px;
  background: #0d1014;
  border-radius: 20px;
  position: relative;
  isolation: isolate;          /* keeps the frame grain off the overlay */
}

.tile { grid-column: span 1; }
.tile--wide { grid-column: 1 / -1; }
```

### 6.2 The rising wash

The wash is the countdown. `--fill` is the fraction of the current interval elapsed, written once per tick as a CSS custom property — no re-render, no animation frame loop.

```css
.tile {
  position: relative;
  overflow: hidden;
  border-radius: 14px;
  background: #151a21;
  --tint: #7fd4a8;
  --fill: 0;                    /* 0 → 1, set from JS */
}

.tile::before {
  content: '';
  position: absolute; inset: 0;
  background: linear-gradient(to top, color-mix(in oklab, var(--tint) 26%, transparent), transparent);
  transform: scaleY(var(--fill));
  transform-origin: bottom;
  transition: transform 1.2s linear;
  pointer-events: none;
}
```

```ts
// once per 10s tick, for each tile
const fill = clamp01((now - slotStart) / (slotEnd - slotStart));
el.style.setProperty('--fill', String(fill));
```

`color-mix` in oklab keeps the seven tints at even perceptual weight; mixing in sRGB would make the yellow and amber tiles read considerably hotter than the violet at the same percentage.

### 6.3 Grain

Six discrete steps, not a slide. A translating gradient reads as a moving sheet; stepping between fixed offsets reads as grain.

```css
@keyframes grain-drift {
  0%,15%   { transform: translate(0,      0); }
  16%,31%  { transform: translate(-2%,  1%); }
  32%,47%  { transform: translate(1%,  -2%); }
  48%,63%  { transform: translate(-1%, -1%); }
  64%,79%  { transform: translate(2%,   1%); }
  80%,100% { transform: translate(0,    2%); }
}

.grain {
  position: absolute; inset: -6%;
  background-image: var(--grain-svg);     /* inline feTurbulence data URI */
  opacity: .06;
  mix-blend-mode: overlay;
  pointer-events: none;
  animation: grain-drift 1.6s steps(1) infinite;
}
.frame > .grain { opacity: .03; }

@media (prefers-reduced-motion: reduce) {
  .grain { animation: none; }
  .tile::before { transition: none; }
}
```

The grain texture is one inline `feTurbulence` SVG as a data URI, defined once on `:root` and reused by every tile — a shared `background-image` string is one decoded bitmap in the compositor, whereas seven separate URIs would be seven.

### Dot states

| State | Appearance | Meaning |
|---|---|---|
| Done | filled, action tint | logged |
| Skipped | mid-grey solid | deliberately skipped |
| Missed | hollow ring | slot passed with no response |
| Pending | dark solid | still to come |

Skipped and missed are visually distinct because deciding not to eat lunch and forgetting to log lunch are different facts about the day.

```ts
// Dots render in a fixed order — done, skipped, missed, then pending padding —
// so a tile's dots never reshuffle as the day fills in.
export function dots(view: DayView, id: ActionId): DotState[] {
  const { target } = BY_ID[id];
  const d = view.counts[id] ?? 0, s = view.skipped[id] ?? 0, m = view.missed[id] ?? 0;
  return [
    ...Array(d).fill('done'),
    ...Array(s).fill('skipped'),
    ...Array(m).fill('missed'),
    ...Array(Math.max(0, target - d - s - m)).fill('pending'),
  ].slice(0, Math.max(target, d + s + m));   // overshoot grows the row, never truncates done
}
```

```css
.dot            { width: 6px; height: 6px; border-radius: 50%; }
.dot--done      { background: var(--tint); }
.dot--skipped   { background: #4a525c; }
.dot--missed    { background: none; box-shadow: inset 0 0 0 1px #3a424c; }
.dot--pending   { background: #1e242c; }
```

### Prompt overlay

Tinted to the action. Large icon, name, one line of context, three buttons plus the X. Tint propagates to the icon, the Done button, the stretch ring and the confirmation tick.

```tsx
export function Prompt({ id, position, total, on }: PromptProps) {
  const def = BY_ID[id];
  return (
    <div className="overlay" style={{ '--tint': def.tint } as CSSProperties}>
      <button className="overlay__x" onClick={on.dismiss} aria-label="Dismiss all prompts">×</button>
      {total > 1 && <p className="overlay__count">{position} OF {total}</p>}
      <Icon name={def.icon} className="overlay__icon" />
      <h2>{def.name}</h2>
      <p className="overlay__blurb">{def.blurb}</p>
      <button className="btn btn--primary" onClick={on.done}>Done</button>
      <button className="btn" onClick={on.skip}>Skip this one</button>
      <button className="btn" onClick={on.delayAll}>Delay all 15 min</button>
    </div>
  );
}
```

Every tint reaches the DOM as one custom property on the overlay root. No per-action stylesheet, no class-per-colour.

### Stretch flow

Replaces the prompt overlay. One stretch per screen with a countdown ring, auto-advancing on completion, Next to advance early, Skip to abandon the set.

The ring is a stroked SVG circle driven by `stroke-dashoffset`, which the compositor can animate without layout:

```tsx
const R = 54, C = 2 * Math.PI * R;

<svg viewBox="0 0 120 120" className="ring">
  <circle cx="60" cy="60" r={R} className="ring__track" />
  <circle cx="60" cy="60" r={R} className="ring__fill"
          strokeDasharray={C}
          strokeDashoffset={C * (1 - remaining / step.seconds)}
          style={{ stroke: 'var(--tint)' }} />
</svg>
```

```ts
// One rAF-free countdown: seconds resolution is all the ring needs.
useEffect(() => {
  const end = Date.now() + step.seconds * 1000;
  const t = setInterval(() => {
    const left = Math.max(0, (end - Date.now()) / 1000);
    setRemaining(left);
    if (left === 0) { cue('ready'); advance(); }
  }, 200);
  return () => clearInterval(t);
}, [stepIndex]);
```

Deriving `remaining` from an absolute end time rather than decrementing a counter means a stalled tab resumes at the correct point instead of stretching a 40-second hold into a minute.

Abandoning the set part-way logs a `skip`, not a partial `done` — a half-finished stretch set is a skipped stretch set, and the dot should say so.

### 6.4 Backfill — resolved, and now a non-issue

The open question was: on load, do slots already passed since 09:00 get marked missed? Marking them means opening the app mid-morning shows a wall of hollow rings, which is a poor first impression if the user was doing the habits and just not logging.

**There is now no backfill pass, no setting, and nothing to decide.** Under §3 a miss requires an awake window covering the slot. Slots that passed before the app was opened aren't inside one, so they are not missed and never were — they render neutral without any code running. There is no repair step on mount because there is nothing to repair.

This is the clearest case for the model revision. The original question was only difficult because writing a miss is a *commitment* — once written, a row of hollow rings is a claim about the user's day that has to be got right at write time, with no way to revise it later. Deriving it means the app never makes a claim it wasn't in a position to observe.

The rule in one line, and it holds for a closed laptop, a backgrounded tab, and a board with a flat battery alike:

> The app can only mark you as having missed something it actually asked you about.

---

## 7. Feedback

**Sound** — `cuelume` (MIT, ESM, ~5kB, synthesized live, no audio files).

| Event | Cue |
|---|---|
| Prompt appears | `bloom` |
| Action logged | `success` |
| Stretch step complete | `ready` |

Global mute toggle wired to `setEnabled()`. Falls back to synthesized oscillator tones if the module fails to load.

*Note:* `bloom` is soft and may not cut through when heads-down. Test against `chime` and `ready` before locking it in.

```ts
// src/feedback/sound.ts
type Cue = 'bloom' | 'success' | 'ready' | 'chime';
let impl: { play(c: Cue): void; setEnabled(v: boolean): void } | null = null;

export async function initSound(enabled: boolean) {
  try {
    const m = await import('cuelume');
    m.setEnabled(enabled);
    impl = { play: c => m.play(c), setEnabled: m.setEnabled };
  } catch {
    impl = oscillatorFallback(enabled);
  }
}

export const cue = (c: Cue) => impl?.play(c);

function oscillatorFallback(enabled: boolean) {
  let on = enabled, ctx: AudioContext | null = null;
  const TONES: Record<Cue, number[]> = {
    bloom: [523, 659], success: [659, 880], ready: [440], chime: [880, 1047],
  };
  return {
    setEnabled: (v: boolean) => { on = v; },
    play(c: Cue) {
      if (!on) return;
      ctx ??= new AudioContext();
      TONES[c].forEach((f, i) => {
        const o = ctx!.createOscillator(), g = ctx!.createGain();
        o.frequency.value = f; o.type = 'sine';
        const t = ctx!.currentTime + i * 0.09;
        g.gain.setValueAtTime(0.0001, t);
        g.gain.exponentialRampToValueAtTime(0.2, t + 0.02);
        g.gain.exponentialRampToValueAtTime(0.0001, t + 0.28);
        o.connect(g).connect(ctx!.destination);
        o.start(t); o.stop(t + 0.3);
      });
    },
  };
}
```

**Autoplay policy is a real blocker and needs handling on day one.** An `AudioContext` created before any user gesture starts `suspended`, so the very first prompt of the day is silent on a freshly loaded tab. Resume it on the first interaction and never think about it again:

```ts
const unlock = () => { ctx?.resume(); document.removeEventListener('pointerdown', unlock); };
document.addEventListener('pointerdown', unlock, { once: true });
```

**Haptics** — `navigator.vibrate`: triple pulse on prompt, short tick on Done, lighter ticks on skip and delay.

```ts
// src/feedback/haptics.ts
const PATTERNS = {
  prompt: [40, 60, 40, 60, 40],
  done:   [25],
  skip:   [12],
  delay:  [12, 40, 12],
} as const;

export const buzz = (k: keyof typeof PATTERNS, enabled: boolean) => {
  if (!enabled || !('vibrate' in navigator)) return;
  navigator.vibrate(PATTERNS[k]);
};
```

**Constraint:** iOS Safari does not support `navigator.vibrate` at all. On iPhone this is sound-only. Feature-detect and hide the haptics toggle in settings entirely when `'vibrate' in navigator` is false, rather than showing a switch that does nothing.

---

## 8. Build order

1. Data model, append-only storage, and `derive` — with the property tests from §8.1 before anything renders
2. Grid rendering from the derived view
3. Scheduler tick, jittered offsets, awake windows
4. Prompt queue, overlay, Done/Skip/Delay-all/X wiring
5. cuelume integration and mute toggle
6. Haptics
7. Guided stretch flow
8. Settings panel — working hours, per-action cadence, sound and haptics
9. Day rollover while the tab is open

### 8.1 Test targets

Steps 3 and 4 are the two that will produce bugs nobody notices for a week, and both are pure functions of `(state, now)`. Cover them before moving on:

```ts
describe('derive', () => {
  it('counts an unanswered past slot as missed when awake', () => {
    const log = withAwake([['09:00', '12:00']]);            // no events
    expect(derive(log, at('10:00'), S).missed.water).toBe(1);   // 09:07 slot
  });

  it('does NOT count it when the gap covers the slot', () => {
    const log = withAwake([['09:00', '09:05'], ['11:00', '12:00']]);
    expect(derive(log, at('11:30'), S).missed.water).toBe(0);
  });

  it('re-anchors an interval action from the tap when done early', () => {
    const log = withEvent('water', 'done', at('09:40'), at('09:52'));
    expect(derive(log, at('10:00'), S).next.water).toBe(at('10:25'));
  });

  it('leaves a fixed action on its clock time when done early', () => {
    const log = withEvent('lunch', 'done', at('12:40'), at('13:00'));
    expect(derive(log, at('12:45'), S).counts.lunch).toBe(1);
    expect(derive(log, at('12:45'), S).next.lunch).toBeUndefined();
  });

  it('is idempotent under duplicate events', () => {
    const once  = appendEvent(base, EV);
    const twice = appendEvent(once, { ...EV, id: 'different-uuid' });
    expect(derive(twice, NOW, S)).toEqual(derive(once, NOW, S));
  });

  it('merges two tabs to the same view regardless of order', () => {
    expect(derive(merge(a, b), NOW, S)).toEqual(derive(merge(b, a), NOW, S));
  });
});

describe('tick', () => {
  it('does not re-queue an action already showing', () => { /* … */ });
  it('emits nothing outside working hours',          () => { /* … */ });
  it('gives stretches the head of the queue',        () => { /* … */ });
  it('never stacks delay-all beyond now + 15',       () => { /* … */ });
});
```

The last two `derive` tests are the ones worth having. Commutativity of `merge` and idempotence under replay are the properties the whole cross-device story in §10 rests on, and they are cheap to assert now and expensive to retrofit.

### 8.2 Day rollover (step 9)

Rollover is a tick concern, not a mount concern. Check the date every tick:

```ts
function runTick(now: number) {
  const key = todayKey();
  if (key !== log.date) {
    saveDay(log);                    // close out yesterday
    setLog(emptyDay(key));           // no state to reset — the schedule is config
    setQueue({ queue: [] });         // yesterday's cards are void
    return;
  }
  // … normal tick
}
```

A tab left open overnight is the common case for this app, not an edge case, so this needs to work without a reload. Note there is no "reset all the timers" step: `next` is derived from an empty log against today's date, so a fresh day is literally an empty object.

---

## 9. Known constraints (Phase 1)

- **Browser tab only.** Prompts fire only while the tab is open and unsuspended. Backgrounded tabs get throttled intervals. This is the main reliability gap.
- **No haptics on iOS.**
- **No cross-device sync.** Two tabs merge correctly via §3.4, but two *devices* have no transport between them until Phase 2.
- **Day rollover** while the tab is open needs explicit handling, not just on-mount — see §8.2.

What is explicitly *no longer* a constraint, thanks to §3: a closed tab does not accumulate misses, so the app can be shut and reopened freely without the day's record becoming a wall of hollow rings.

---

## 10. Phase 2 — ESP32 integration

Hardware: Waveshare ESP32-S3-Touch-AMOLED-1.8 (368x448, ESP32-S3R8, 8MB PSRAM, 16MB flash, QMI8658 IMU, PCF85063 RTC, AXP2101 PMIC, speaker, battery).

**Architecture: board is the source of truth.**

- Board runs the scheduler in ESP-IDF with LVGL, and an `esp_http_server` on the LAN
- `GET /state` returns counts, next-due times and skip/miss tallies
- `POST /event` logs a completion
- Web app polls `/state`, or opens a WebSocket for push
- Discovery via mDNS at `wfh.local`, so no hardcoded IP

### 10.1 Wire format

`GET /state` returns the `DayLog` verbatim — events plus awake windows, not the derived view. The browser derives its own tallies from it with the same `derive` from §3.2. Shipping facts rather than conclusions means the two never disagree about what a miss is, and it keeps the payload small: a full day is roughly 30 events, well under 4KB of JSON.

```c
// GET /state
static esp_err_t state_get(httpd_req_t *req) {
    char *json = day_state_to_json(&g_day);      // same field names as §3.1
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

// POST /event  {"action":"water","kind":"done","ts":1723645200,"slot":1723645020}
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
    day_apply_event(&g_day, &ev);   // idempotent on (action, slot) — see §3.1
    day_persist(&g_day);            // NVS
    lv_async_call(ui_refresh, NULL);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}
```

`day_apply_event` deduping on `(action, slot)` is what makes a flaky LAN safe: the browser can retry a `POST` it never got a response to, and a double-tap on the board while the browser also logs the same prompt collapses to one event.

**Why this resolves the Phase 1 constraints:**

- Board runs continuously rather than inside a suspendable browser tab — with the caveats in §10.3
- Board has a speaker and can drive a vibration motor, so no iOS haptics gap
- RTC survives reboots and battery swaps

### 10.3 Power — the board is not always on

The draft claimed "board is always on". That is only true on USB-C. On battery it is not, and the plan has to say what happens instead.

**Rough numbers for this board.** The 1.8" AMOLED is the dominant draw by a wide margin — call it 80–150mA lit depending on how much of the panel is actually emitting, against maybe 40–50mA for the ESP32-S3 with WiFi associated and the screen off. A small LiPo in the 500mAh class is therefore a handful of hours screen-on and most of a day screen-off. Confirm against your actual cell and measure at the PMIC rather than trusting this paragraph.

The design consequence is that **screen-on time is the budget**, not uptime. This app is idle 99% of the time by nature: seven prompts an hour at most, each needing a few seconds of attention. That is a good fit for an aggressively dark panel.

**Three power states.**

| State | Trigger | Screen | WiFi | Draw | Serves HTTP |
|---|---|---|---|---|---|
| Active | prompt firing, or touch within 20s | on | on | high | yes |
| Idle | no touch for 20s, inside working hours | off | modem-sleep (DTIM) | low | yes |
| Dormant | outside working hours | off | off | ~µA | no |

The important one is **Idle**. Screen off with WiFi in modem-sleep keeps the TCP stack alive, so `/state` and `/event` keep working and the web app never notices, while the panel — the actual cost — is dark. Automatic light sleep does this without restructuring anything:

```c
esp_pm_config_t pm = {
    .max_freq_mhz = 240,
    .min_freq_mhz = 40,
    .light_sleep_enable = true,     // wakes on WiFi, timer, or GPIO
};
ESP_ERROR_CHECK(esp_pm_configure(&pm));
esp_wifi_set_ps(WIFI_PS_MAX_MODEM);  // wake only on DTIM beacons
```

**Deep sleep is the wrong tool here** and is worth stating explicitly so nobody reaches for it: it drops the network stack, so the board disappears from the LAN and the web app's `/state` poll fails. Use it only for Dormant — overnight, outside working hours — where nothing is scheduled anyway and being unreachable costs nothing.

```c
// Entering Dormant at 18:00. The RTC wakes us for tomorrow's 09:00.
static void enter_dormant(time_t work_start_tomorrow) {
    uint64_t us = (uint64_t)(work_start_tomorrow - time(NULL)) * 1000000ULL;
    esp_sleep_enable_timer_wakeup(us);
    esp_sleep_enable_ext1_wakeup(BIT64(PIN_BTN), ESP_EXT1_WAKEUP_ANY_LOW); // or a press
    day_persist(&g_day);              // flush the log before we lose RAM
    esp_deep_sleep_start();
}
```

**Running flat mid-day is now a non-event.** This is the payoff from §3. The board dies at 14:00, gets plugged in at 15:30, and boots with the event log intact in NVS and the correct wall-clock time from the PCF85063. It opens a new awake window at 15:30, and the slots between 14:00 and 15:30 are outside every window, so they are *not* misses. The user is not blamed for a dead battery. Under the original model the board would have woken up and written ninety minutes of misses for prompts nobody ever heard.

Persist on every event and close the awake window on the way down, so an unexpected cut loses at most one tick:

```c
static void on_power_event(axp2101_event_t ev) {
    switch (ev) {
    case AXP2101_VBUS_REMOVED:
        g_on_battery = true;
        ui_set_brightness(BRIGHTNESS_BATTERY);   // ~40% — biggest single saving
        idle_timeout_set(20 * 1000);
        break;
    case AXP2101_VBUS_INSERTED:
        g_on_battery = false;
        ui_set_brightness(BRIGHTNESS_MAINS);
        idle_timeout_set(60 * 1000);
        break;
    case AXP2101_BATT_LOW:                        // ~15%
        day_awake_close(&g_day, time(NULL));       // seal the window now
        day_persist(&g_day);
        notify("WFH tracker low", "Battery at 15% — plug it in");
        break;
    case AXP2101_BATT_CRITICAL:                    // ~5%
        day_awake_close(&g_day, time(NULL));
        day_persist(&g_day);
        enter_dormant(0);                          // wake on USB only
        break;
    }
}
```

`day_awake_close` on the low-battery interrupt is the one line that matters for correctness: it seals the window while there is still power to write NVS, so the log records honestly when the board stopped being able to prompt.

Expose the battery in `/state` so the web app can show it and the user is never guessing:

```c
cJSON_AddNumberToObject(root, "battPct",  axp2101_get_batt_percent());
cJSON_AddBoolToObject(root,   "charging", axp2101_is_charging());
cJSON_AddBoolToObject(root,   "onBattery", g_on_battery);
```

**Recommendation: run it on USB-C.** It sits on a desk next to a laptop; a permanently-connected cable is not a hardship, and it removes the whole question. The battery is then what carries it through a power cut and an unplugging, not the normal operating mode — and the design above means neither of those corrupts the day's record.

### 10.4 Input — how the board marks an action done

The board's primary input is the capacitive touch panel, driven through LVGL. A physical key is the useful secondary, because the point of this device is that you can answer it without looking.

**Check the exact touch controller and key GPIOs against the Waveshare schematic for your revision** rather than trusting numbers here — the AMOLED boards have changed touch parts between revisions, and the BOOT key is shared with strapping on some layouts.

**Touch: the prompt screen.** Same three buttons as the web overlay, sized for a thumb on a 368px-wide panel.

```c
// Prompt screen. One LVGL object tree, retinted per action.
static void build_prompt(action_id_t id) {
    const action_def_t *def = &ACTIONS[id];

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(def->tint_dim), 0);

    lv_obj_t *done = lv_btn_create(scr);
    lv_obj_set_size(done, 300, 96);                       // large: thumb, not stylus
    lv_obj_align(done, LV_ALIGN_CENTER, 0, 40);
    lv_obj_set_style_bg_color(done, lv_color_hex(def->tint), 0);
    lv_obj_add_event_cb(done, on_done_cb, LV_EVENT_CLICKED, (void *)(intptr_t)id);

    lv_obj_t *skip = lv_btn_create(scr);
    lv_obj_add_event_cb(skip, on_skip_cb, LV_EVENT_CLICKED, (void *)(intptr_t)id);
    /* … delay-all, X … */

    lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
}
```

**The handler is the whole answer to "does the button mark it as done".** It builds exactly the same event the browser builds — same fields, same `slot`, same dedupe key — and that identity is what makes the board and the web app interchangeable rather than merely connected.

```c
static void on_done_cb(lv_event_t *e) {
    action_id_t id = (action_id_t)(intptr_t)lv_event_get_user_data(e);

    log_event_t ev = {
        .action = id,
        .kind   = KIND_DONE,
        .ts     = time(NULL),                  // PCF85063-backed, correct after reboot
        .slot   = g_queue_current_slot(id),    // the slot being answered
    };
    uuid_v4(ev.id);

    day_apply_event(&g_day, &ev);   // idempotent on (action, slot) — §3.1
    day_persist(&g_day);            // NVS, immediately: survives a battery cut

    haptic_pulse(25);               // motor on a PWM GPIO
    audio_cue(CUE_SUCCESS);
    ws_broadcast_state();           // every connected browser updates now

    queue_advance();                // next card, or back to the grid
    idle_timer_reset();             // screen goes dark 20s from now
}
```

`day_apply_event` is the *only* mutation path on the board, and both the touch handler and the `POST /event` route in §10.1 go through it. A tap on the board and a tap in the browser are the same operation reaching the same function by different transports.

**Physical key.** One press = Done on the current prompt, which is what you want when the prompt is "stand up" and you are already standing. Debounce in software; the panel's key lines are noisy.

```c
static void key_task(void *arg) {
    int64_t last = 0;
    for (;;) {
        if (gpio_get_level(PIN_BTN) == 0) {              // active low — verify!
            int64_t now = esp_timer_get_time();
            if (now - last > 250000) {                    // 250ms debounce
                last = now;
                if (screen_is_dark()) {
                    wake_screen();                        // first press wakes only
                } else if (g_queue_len > 0) {
                    on_done_current();                    // second press answers
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
```

The wake-then-answer split matters: a single press that both lights the panel and logs a completion means every accidental brush marks water as drunk. First press wakes, second press commits.

**Tap-to-answer via the IMU** is worth prototyping as the third input. The QMI8658 has tap detection, so a knock on the desk beside the board could answer the current prompt without reaching for it — genuinely nice for "stand break" — but it needs a real false-positive threshold before it goes anywhere near `day_apply_event`. Prototype it behind a setting, default off.

**The grid screen.** Tapping any tile logs that action immediately, with no prompt — the "I just drank a glass, credit me" path. Same handler, `slot` resolved to the next upcoming slot rather than a current one, which is exactly what `logDone` does in §4.3 when `view.due` doesn't contain the action.

### 10.2 The web app as a client

One storage adapter swap, nothing above it changes:

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

**Offline handling is the same `merge` from §3.4.** The board being asleep, unreachable, or flat is indistinguishable from a failed fetch, so the web app does not need to know which it was: queue events locally, retry, and merge on reconnect. Because `(action, slot)` dedupes and union is commutative, a retry storm after the board comes back is harmless.

```ts
export const resilient = (board: Backend, local: Backend): Backend => ({
  async load() {
    try { return merge(await board.load(), await local.load()); }
    catch { return local.load(); }              // board asleep: run standalone
  },
  async log(ev) {
    await local.log(ev);                        // always land it locally first
    try { await board.log(ev); } catch { outbox.push(ev); }
  },
  subscribe: board.subscribe,
});
```

Note the ordering: local write first, board write second, best-effort. The user's tap is never lost to a network error, and the board catches up when it can.

Note `http://` against an `https://` page is blocked as mixed content. Either serve the web app from the board itself over plain HTTP, or run it from a local file — do not plan on hosting it on an HTTPS origin and talking to the board from there.

When the board is the source of truth, the browser's scheduler must be switched off entirely rather than left running alongside. Two schedulers writing the same `(action, slot)` is survivable because of the dedupe, but two schedulers *chiming* is not.

**Push notifications:** the board can `POST` to `notifi.it/send` to get a native push on iPhone or Mac. One HTTP request, no SDK, no account. This closes the "away from the desk" gap without building an app.

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

Send a push only when the user has been away — gate it on the IMU reporting no movement and the touch panel reporting no interaction for the last few minutes. Pushing every prompt to the phone while the user is sitting at the board would make the phone the thing they mute.

The 368x448 display maps cleanly onto the square grid layout with room for a header, so the UI transfers with minimal rework: a 368x368 square for the seven tiles and 80px of header for the clock and day tallies.

---

## 11. Storage: SQLite?

Considered and deferred. It is a good fit for the shape the data took in §3, and a poor fit for the volume — so the answer depends entirely on whether historical views arrive.

### 11.1 Why it fits the model well

The revision in §3 turned the store into an append-only log of uniquely-keyed rows. That is a table. The idempotence that §3.4 enforces with a `some()` check becomes a schema constraint, which is strictly better — the database refuses the duplicate whether it came from a retry, a second tab, or the board:

```sql
CREATE TABLE events (
  id     TEXT PRIMARY KEY,               -- uuid v4
  day    TEXT NOT NULL,                  -- YYYY-MM-DD, local
  action TEXT NOT NULL,
  kind   TEXT NOT NULL CHECK (kind IN ('done', 'skip')),
  ts     INTEGER NOT NULL,               -- epoch seconds
  slot   INTEGER NOT NULL,
  source TEXT NOT NULL DEFAULT 'web',    -- 'web' | 'board'
  UNIQUE (action, slot)                  -- the dedupe key, enforced
);
CREATE INDEX events_day ON events (day);

CREATE TABLE awake (
  day   TEXT    NOT NULL,
  start INTEGER NOT NULL,
  fin   INTEGER NOT NULL,
  PRIMARY KEY (day, start)
);
```

```sql
-- The whole of appendEvent(). Retries are free.
INSERT OR IGNORE INTO events (id, day, action, kind, ts, slot, source)
VALUES (?, ?, ?, ?, ?, ?, ?);
```

Counts and skips become one query instead of a walk:

```sql
SELECT action, kind, COUNT(*) AS n
FROM events WHERE day = ? GROUP BY action, kind;
```

**But `missed` and `next` do not translate cleanly, and that is the catch.** Both need the slot walk from §3.2, and that walk is not a fixed lattice — §4.3's early-completion rule re-anchors an interval action from the tap, so slot *n+1* depends on the event that answered slot *n*. Expressing that in SQL means a recursive CTE that joins each generated slot back against `events` to decide where the next one lands:

```sql
WITH RECURSIVE slots(action, slot) AS (
  SELECT 'water', :first_slot
  UNION ALL
  SELECT s.action,
         COALESCE((SELECT e.ts FROM events e                     -- re-anchor …
                    WHERE e.action = s.action AND e.slot = s.slot
                      AND e.kind = 'done'), s.slot) + :every_sec  -- … or advance
  FROM slots s WHERE s.slot < :work_end
)
SELECT COUNT(*) FROM slots s
WHERE s.slot <= :now
  AND NOT EXISTS (SELECT 1 FROM events e WHERE e.action = s.action AND e.slot = s.slot)
  AND EXISTS (SELECT 1 FROM awake a WHERE s.slot BETWEEN a.start AND a.fin);
```

That is correct and it is worse than the fifteen lines of TypeScript it replaces — harder to read, harder to test, and it has to be parameterised per action because the cadence lives in config. **Keep `derive` in code even if the storage becomes SQL.** Use the database for facts and aggregates, not for the scheduling semantics.

### 11.2 Phase 1 (browser): not yet

It works. `@sqlite.org/sqlite-wasm` with the `opfs-sahpool` VFS persists properly and — unlike the plain OPFS VFS — does not need `SharedArrayBuffer`, so no COOP/COEP headers and no hosting constraints. Verify that against the current release before committing to it; the VFS story has moved more than once.

The cost is the problem relative to the benefit:

| | localStorage JSON | SQLite WASM |
|---|---|---|
| Payload | 0 | ~1MB wasm |
| Startup | synchronous | async init, worker |
| Writes/day | ~30 | ~30 |
| Data/day | ~4KB | ~4KB |
| Year of data | ~1.4MB | ~1.4MB |

A year fits inside the ~5MB localStorage quota, and §3.4 already prunes to 14 days. **Roughly a megabyte of WebAssembly to manage four kilobytes a day is not a trade worth making** for a v1 whose stated scope is "today at a glance, no historical views".

If localStorage specifically is the worry, IndexedDB is the proportionate step up — no payload, much larger quota, and it survives storage pressure better. That is a twenty-line adapter, not a database engine.

### 11.3 When it becomes the right call

Three triggers, any one of which flips this:

1. **Historical views.** "Water over the last 30 days", streaks, a weekday-vs-Friday comparison. This is where hand-rolled aggregation over per-day JSON keys stops being pleasant and SQL starts paying for itself immediately.
2. **The board as archive (Phase 2).** 16MB of flash holds years of this. `GET /history?from=&to=` backed by a real query is a much better API than shipping a year of JSON blobs to the browser to reduce client-side.
3. **A sync server.** The moment there is a third party mediating between board and browser, its store should be SQLite, and the `(action, slot)` uniqueness makes multi-writer sync nearly free.

On the board it is viable now: the SQLite3 port runs on ESP32 over a FATFS or LittleFS partition, and the 8MB PSRAM comfortably covers the page cache. At ~30 writes a day, flash wear is not a consideration. It costs a few hundred KB of flash and a filesystem partition, against NVS blobs which are ~4KB a day and need no dependency at all. **Ship Phase 2 on NVS; move to SQLite when history lands** — same trigger as the browser.

### 11.4 Keeping the door open

Nothing needs to change now, because §3 already did the work. The store is an append-only log behind the `Backend` interface from §10.2, so swapping the implementation touches one file:

```ts
export const sqliteBackend = async (): Promise<Backend> => {
  const sqlite3 = await sqlite3InitModule();
  const db = new sqlite3.oo1.OpfsSAHPoolDb('/wfh.sqlite3');
  db.exec(SCHEMA);                                    // idempotent, IF NOT EXISTS

  return {
    async load(date = todayKey()) {
      return {
        version: 2, date,
        events: db.selectObjects('SELECT * FROM events WHERE day = ? ORDER BY ts', [date]),
        awake:  db.selectArrays('SELECT start, fin FROM awake WHERE day = ?', [date]),
        snoozedUntil: {},                             // transient; not persisted
      };
    },
    async log(ev) {
      db.exec({ sql: INSERT_OR_IGNORE, bind: [ev.id, todayKey(), ev.action,
                                               ev.kind, ev.ts, ev.slot, 'web'] });
    },
    subscribe: () => () => {},
  };
};
```

`derive` is untouched, the reducers are untouched, the UI is untouched. Two rules keep it that way:

- **Never read the derived view out of the database.** Facts in, `derive` on top. The moment a query returns `missed` directly, the board and the browser can disagree about what a miss is, and §3's guarantee is gone.
- **Migration is a replay, not a conversion.** Read the JSON day keys, `INSERT OR IGNORE` every event, done. Idempotent, re-runnable, and safe to abandon halfway.

**Decision: stay on localStorage for Phase 1 and NVS for Phase 2. Revisit at the first historical view.** Write the schema above into the repo when that happens; do not carry the dependency before then.
