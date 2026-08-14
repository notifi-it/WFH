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

```json
{
  "date": "2026-08-14",
  "events": [ { "id": "...", "action": "water", "ts": 1723645200 } ],
  "counts": { "stand": 4, "water": 3 },
  "skipped": { "stand": 1 },
  "missed":  { "water": 2 },
  "next":    { "stand": 1723645200 }
}
```

Storage keys:

- `wfh:day:YYYY-MM-DD` — one key per day, full event list
- `wfh:settings` — working hours, per-action cadences, sound and haptics on/off

One write per completion. Reads on mount only.

### 3.1 Types

`counts`, `skipped` and `missed` are derivable from `events`, but they are persisted anyway: the grid reads them every render and recomputing a reduce over the event list on each of those is pointless work. `events` stays the audit trail and the tie-breaker if the two ever disagree.

```ts
// src/state/types.ts
import type { ActionId } from '../config/actions';

export type EventKind = 'done' | 'skip' | 'miss';

export interface LogEvent {
  id: string;          // crypto.randomUUID()
  action: ActionId;
  kind: EventKind;
  ts: number;          // epoch seconds, local clock
  slot: number;        // epoch seconds of the slot this answers — dedupe key
}

export type Tally = Partial<Record<ActionId, number>>;

export interface DayState {
  version: 1;
  date: string;                            // YYYY-MM-DD, local
  events: LogEvent[];
  counts: Tally;
  skipped: Tally;
  missed: Tally;
  next: Partial<Record<ActionId, number>>;      // epoch seconds
  snoozedUntil: Partial<Record<ActionId, number>>;
  lastSeen: number;                              // epoch seconds, for backfill
}

export interface Settings {
  version: 1;
  workStart: string;    // "09:00"
  workEnd: string;      // "18:00"
  cadences: Partial<Record<ActionId, Cadence>>;  // overrides ACTIONS defaults
  sound: boolean;
  haptics: boolean;
  backfill: 'since-open' | 'since-start' | 'off';   // see §6.4
}
```

`slot` is the field that makes the whole thing idempotent. Two tabs, a replayed event, a board and a browser both logging the same prompt — all collapse to one entry because `(action, slot)` is unique.

### 3.2 Storage layer

```ts
// src/state/storage.ts
import type { DayState, Settings } from './types';

const DAY = (d: string) => `wfh:day:${d}`;
const SETTINGS = 'wfh:settings';

export const todayKey = (at = new Date()): string => {
  const p = (n: number) => String(n).padStart(2, '0');
  return `${at.getFullYear()}-${p(at.getMonth() + 1)}-${p(at.getDate())}`;
};

export function loadDay(date: string): DayState | null {
  try {
    const raw = localStorage.getItem(DAY(date));
    if (!raw) return null;
    const parsed = JSON.parse(raw) as DayState;
    return parsed.version === 1 ? parsed : migrateDay(parsed);
  } catch {
    return null;          // corrupt key: start the day clean rather than crash
  }
}

export function saveDay(state: DayState): void {
  try {
    localStorage.setItem(DAY(state.date), JSON.stringify(state));
  } catch (err) {
    // QuotaExceededError: drop the oldest days and retry once.
    pruneDays(14);
    try { localStorage.setItem(DAY(state.date), JSON.stringify(state)); } catch { /* give up */ }
  }
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

**Cross-tab coherence.** §9 accepts last-write-wins, but the cheap 80% fix is a `storage` event listener — the browser fires it in *other* tabs when a tab writes:

```ts
window.addEventListener('storage', e => {
  if (e.key === `wfh:day:${todayKey()}` && e.newValue) {
    dispatch({ type: 'external-update', state: JSON.parse(e.newValue) });
  }
});
```

Two tabs then converge on each write instead of silently diverging all day.

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

```ts
// src/scheduler/tick.ts
export interface TickResult {
  state: DayState;
  enqueue: ActionId[];      // newly due, in priority order
  missed: ActionId[];       // slots that expired unanswered
}

export function tick(state: DayState, now: number, s: Settings): TickResult {
  if (!inWorkingHours(now, s)) return { state, enqueue: [], missed: [] };

  let next = { ...state.next };
  let missedTally = { ...state.missed };
  const events: LogEvent[] = [];
  const enqueue: ActionId[] = [];
  const missed: ActionId[] = [];

  for (const def of ACTIONS) {
    const due = next[def.id];
    if (due == null) continue;                              // no slots left today
    if (now < due) continue;                                // not yet
    if ((state.snoozedUntil[def.id] ?? 0) > now) continue;  // delayed

    const after = slotAfter(def, due, s);

    if (isQueued(def.id)) {
      // The previous prompt is still sitting unanswered and its replacement is
      // now due. Record exactly one miss and roll forward. Misses never stack:
      // the action stays in the queue once, not n times.
      missed.push(def.id);
      missedTally[def.id] = (missedTally[def.id] ?? 0) + 1;
      events.push(mkEvent(def.id, 'miss', now, due));
    } else {
      enqueue.push(def.id);
    }
    next[def.id] = after ?? undefined;
  }

  enqueue.sort((a, b) => BY_ID[b].priority - BY_ID[a].priority);

  return {
    state: { ...state, next, missed: missedTally,
             events: [...state.events, ...events], lastSeen: now },
    enqueue, missed,
  };
}
```

`isQueued` is injected from the queue module rather than read off `DayState` — the queue is UI state, not persisted state, and a reload should not resurrect yesterday's unanswered card.

### 4.3 Completing early

```ts
export function logDone(state: DayState, id: ActionId, now: number, s: Settings): DayState {
  const def = BY_ID[id];
  const slot = state.next[id] ?? now;
  const rolled = def.cadence.kind === 'interval'
    ? slotAfter(def, now, s)     // interval: reset from NOW, so early credit carries
    : slotAfter(def, slot, s);   // fixed: 13:00 lunch stays 13:00 whenever you eat

  return {
    ...state,
    counts: { ...state.counts, [id]: (state.counts[id] ?? 0) + 1 },
    next:   { ...state.next, [id]: rolled ?? undefined },
    events: [...state.events, mkEvent(id, 'done', now, slot)],
  };
}
```

The asymmetry is deliberate and is the one place the two cadence kinds must not share a code path. Drinking water at 11:20 should push the next glass to 12:05. Eating lunch at 12:40 should not push tomorrow's — or today's second — fixed slot anywhere.

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

export function reduce(q: QueueState, a: QueueAction, day: DayState, s: Settings) {
  switch (a.type) {
    case 'enqueue': {
      // Stretches pre-empt: they take the head and everything else rolls forward
      // one interval rather than waiting behind three minutes of guided flow.
      const stretch = a.ids.filter(id => BY_ID[id].flow === 'stretch');
      if (stretch.length) {
        const bumped = rollForward(day, [...q.queue, ...a.ids.filter(i => !stretch.includes(i))], a.now, s);
        return { q: { queue: stretch }, day: bumped };
      }
      const fresh = a.ids.filter(id => !q.queue.includes(id));   // never double-queue
      return { q: { queue: [...q.queue, ...fresh] }, day };
    }

    case 'done':
      return { q: { queue: q.queue.slice(1) }, day: logDone(day, q.queue[0], a.now, s) };

    case 'skip':
      return { q: { queue: q.queue.slice(1) }, day: logSkip(day, q.queue[0], a.now, s) };

    case 'delay-all': {
      // One tap covers the whole meeting. Snoozing is absolute, not additive,
      // so mashing the button never pushes anything past now + 15.
      const until = a.now + 15 * 60;
      const snoozedUntil = { ...day.snoozedUntil };
      q.queue.forEach(id => { snoozedUntil[id] = until; });
      return { q: { queue: [] }, day: { ...day, snoozedUntil } };
    }

    case 'dismiss':
      // Not a skip and not a miss. The user closed the card; the slots stay
      // where they are and will be re-raised on the next tick if still due.
      return { q: { queue: [] }, day };
  }
}
```

`delay-all` sets `snoozedUntil` to an absolute timestamp rather than adding 15 minutes to an existing snooze. That is what "repeatable, never stacks" means in code.

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
export function dots(day: DayState, id: ActionId): DotState[] {
  const { target } = BY_ID[id];
  const d = day.counts[id] ?? 0, s = day.skipped[id] ?? 0, m = day.missed[id] ?? 0;
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

### 6.4 Backfill

On load, slots already passed since 09:00 are marked missed. **Open question:** this means opening the app mid-morning shows a wall of hollow rings, which is a poor first impression if the user was genuinely doing the habits and just not logging. Alternatives: backfill only from last-open, or treat unlogged as neutral rather than missed.

**Recommendation: `since-open`, and make it the default.** The `lastSeen` field in `DayState` exists for exactly this. Slots that passed while the app was demonstrably running are real misses — the prompt fired, nobody answered. Slots that passed before the app was ever opened today are not misses, because nothing was ever asked; they are pending-and-gone, which renders as neutral.

```ts
export function backfill(state: DayState, now: number, s: Settings): DayState {
  if (s.backfill === 'off') return state;
  const from = s.backfill === 'since-open' ? state.lastSeen : atTime(new Date(), s.workStart);

  let missed = { ...state.missed };
  const events: LogEvent[] = [];
  for (const def of ACTIONS) {
    let slot = state.next[def.id];
    while (slot != null && slot < now) {
      if (slot >= from) {                       // only count what we were awake for
        missed[def.id] = (missed[def.id] ?? 0) + 1;
        events.push(mkEvent(def.id, 'miss', now, slot));
      }
      slot = slotAfter(def, slot, s) ?? undefined;
    }
    state.next[def.id] = slot;
  }
  return { ...state, missed, events: [...state.events, ...events], lastSeen: now };
}
```

The setting stays in the model because it is one line of config and the argument is genuinely a matter of taste, but ship with `since-open`. It is the only option that never accuses the user of missing something the app never asked for.

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

1. Data model and storage read/write
2. Grid rendering from state
3. Scheduler tick, jittered offsets, missed-slot roll-forward
4. Prompt queue, overlay, Done/Skip/Delay-all/X wiring
5. cuelume integration and mute toggle
6. Haptics
7. Guided stretch flow
8. Settings panel — working hours, per-action cadence, sound and haptics
9. Day rollover while the tab is open

### 8.1 Test targets

Steps 3 and 4 are the two that will produce bugs nobody notices for a week, and both are pure functions of `(state, now)`. Cover them before moving on:

```ts
describe('tick', () => {
  it('rolls a missed slot forward exactly once', () => {
    const s = withQueued('water', dayAt('09:07'));
    const r = tick(s, at('09:52'), SETTINGS);          // second slot arrives
    expect(r.missed).toEqual(['water']);
    expect(r.state.missed.water).toBe(1);
    expect(r.state.next.water).toBe(at('10:37'));      // one interval, not two
  });

  it('does not double-queue an action already showing', () => { /* … */ });
  it('emits nothing outside working hours',            () => { /* … */ });
  it('resets an interval action from now on early completion', () => { /* … */ });
  it('leaves a fixed action on its clock time when done early', () => { /* … */ });
  it('gives stretches the head of the queue and bumps the rest', () => { /* … */ });
  it('never stacks delay-all beyond now + 15', () => { /* … */ });
});
```

### 8.2 Day rollover (step 9)

Rollover is a tick concern, not a mount concern. Check the date every tick:

```ts
function runTick(now: number) {
  const key = todayKey();
  if (key !== state.date) {
    saveDay(state);                              // close out yesterday
    setState(freshDay(key, settings));           // resets all `next` to firstSlot
    setQueue({ queue: [] });                     // yesterday's cards are void
    return;
  }
  // … normal tick
}
```

A tab left open overnight is the common case for this app, not an edge case, so this needs to work without a reload.

---

## 9. Known constraints (Phase 1)

- **Browser tab only.** Prompts fire only while the tab is open and unsuspended. Backgrounded tabs get throttled intervals. This is the main reliability gap.
- **No haptics on iOS.**
- **No cross-device sync.** Two tabs open will conflict; last write wins. The `storage` listener in §3.2 narrows this to a genuine simultaneous-write race.
- **Day rollover** while the tab is open needs explicit handling, not just on-mount — see §8.2.

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

`GET /state` returns `DayState` verbatim. That is the whole point of §3 — the browser's reducer and the board's scheduler operate on one shape, so the web app's rendering code does not fork.

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

- Board is always on, so no browser suspension
- Board has a speaker and can drive a vibration motor, so no iOS haptics gap
- RTC survives reboots and battery swaps

### 10.2 The web app as a client

One storage adapter swap, nothing above it changes:

```ts
export interface Backend {
  load(): Promise<DayState>;
  log(ev: LogEvent): Promise<void>;
  subscribe(cb: (s: DayState) => void): () => void;
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
