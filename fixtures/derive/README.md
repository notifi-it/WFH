# derive fixtures

One JSON file per case. `node tools/gen-fixtures.mjs` turns them into
`firmware/test/fixtures.g.h`; `make -C firmware/test` builds and runs them
with plain `cc` — no ESP-IDF, no board.

## Fields

| Field | Meaning |
|---|---|
| `why` | what breaks if this case regresses — write it before the assertions |
| `tz` | POSIX rule string (no zoneinfo on the board, §3.1a) |
| `date`, `now` | the moment being derived; `now` is `"HH:MM"` local on `date` |
| `actions` | optional action table for this case only; omitted means the shipping schedule from `config/actions.json` |
| `events`, `snoozedUntil` | the log going in; times are `"HH:MM"` local |
| `mode` | `once` (default), or `replay` to also assert the doubled log derives identically |

## What `expect` asserts

- `counts`, `skipped`, `missed` — **exhaustive**. An action not listed must be 0.
- `next` — **partial**, only the listed actions are checked.
- `nextNone` — these actions must have no further slot today.
- `due` — **exact and ordered**, so priority and tie-order are both pinned.
- `nextEpoch` — literal epoch integers.

## The one rule that matters

Every `"HH:MM"` is resolved by `wfh_at_time()` — the function under test — so
those assertions cannot catch a bug in local-time arithmetic itself. That is
what `nextEpoch` is for: the integers in `dst-forward.json` and
`dst-day-before.json` are computed by hand and checked in as constants. Lunch
is 82,800s apart across that pair, not 86,400s, because a day loses an hour to
BST. **Never regenerate those numbers from program output** — an implementation
that does UTC arithmetic by hand passes one fixture and fails the other, and
that is the entire point of the pair.

## Keeping them honest

A suite that passes on the first run has not been shown to work. These were
verified by mutation: break `derive` on purpose, confirm the fixtures fail.
All six caught, worth re-running after any change to the walk.

| Mutation | Caught by |
|---|---|
| drop the §5.3 anchor rule | `card-confirm-partial`, `interval-done-early` |
| anchor fixed actions too (with the loop guard removed) | `fixed-done-early` — counts hit 256 |
| no due window: past always means missed | 17 assertions across 4 fixtures |
| local time by hand (`tm_isdst = 0`) | `dst-forward` only |
| unstable sort of `due` | `stretch-priority`, `replay-idempotent` |
| ignore `snoozedUntil` | `snoozed-not-due` |
