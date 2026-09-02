#include "derive.h"

#include <string.h>

time_t wfh_at_time(const struct tm *day, const char *hhmm) {
    struct tm t = *day;                    // mktime mutates; work on a copy
    t.tm_hour  = (hhmm[0] - '0') * 10 + (hhmm[1] - '0');
    t.tm_min   = (hhmm[3] - '0') * 10 + (hhmm[4] - '0');
    t.tm_sec   = 0;
    t.tm_isdst = -1;
    return mktime(&t);
}

time_t wfh_slot_after(const action_def_t *def, time_t from, const settings_t *s) {
    struct tm day;
    localtime_r(&from, &day);
    const time_t end = wfh_at_time(&day, s->work_end);

    if (def->cadence.kind == CADENCE_INTERVAL) {
        const time_t next = from + (time_t)def->cadence.every_min * 60;
        return next < end ? next : 0;
    }
    for (int i = 0; i < def->cadence.n_times; i++) {
        const time_t t = wfh_at_time(&day, def->cadence.times[i]);
        if (t > from) return t;
    }
    return 0;
}

time_t wfh_first_slot(const action_def_t *def, time_t ref, const settings_t *s) {
    struct tm day;
    localtime_r(&ref, &day);
    const time_t start = wfh_at_time(&day, s->work_start);

    if (def->cadence.kind == CADENCE_INTERVAL) return wfh_slot_after(def, start, s);

    for (int i = 0; i < def->cadence.n_times; i++) {
        const time_t t = wfh_at_time(&day, def->cadence.times[i]);
        if (t >= start) return t;          // >= so an action authored at
    }                                      // work_start still fires
    return 0;
}

time_t wfh_window_close(const action_def_t *def, time_t slot, const settings_t *s) {
    const time_t next = wfh_slot_after(def, slot, s);
    if (next) return next;

    struct tm day;
    localtime_r(&slot, &day);
    return wfh_at_time(&day, s->work_end) + (time_t)s->grace_min * 60;
}

const log_event_t *wfh_latest_event(const day_log_t *log, int action, time_t slot) {
    const log_event_t *last = NULL;
    for (int i = 0; i < log->events_len; i++) {
        if (log->events[i].action == action && log->events[i].slot == slot) last = &log->events[i];
    }
    return last;
}

const char *wfh_kind_name(event_kind_t k) {
    switch (k) {
    case KIND_DONE: return "done";
    case KIND_SKIP: return "skip";
    case KIND_UNDO: return "undo";
    }
    return "skip";
}

bool wfh_kind_parse(const char *name, event_kind_t *out) {
    if (strcmp(name, "done") == 0) { *out = KIND_DONE; return true; }
    if (strcmp(name, "skip") == 0) { *out = KIND_SKIP; return true; }
    if (strcmp(name, "undo") == 0) { *out = KIND_UNDO; return true; }
    return false;
}

/** The slot's answer, or NULL if it has none. An undo as the latest word
 *  means the slot was never answered (§3.4). */
static const log_event_t *find_event(const day_log_t *log, int action, time_t slot) {
    const log_event_t *last = wfh_latest_event(log, action, slot);
    return last && last->kind != KIND_UNDO ? last : NULL;
}

/** Priority descending, insertion sort because it is stable: ties must keep
 *  config order so the card's row order is predictable day to day. qsort is
 *  not stable and would let equal-priority rows shuffle between ticks. */
static void sort_due(day_view_t *v, const settings_t *s) {
    for (int i = 1; i < v->n_due; i++) {
        const int    a = v->due[i];
        const time_t t = v->due_slot[i];
        int j = i - 1;
        while (j >= 0 && s->actions[v->due[j]].priority < s->actions[a].priority) {
            v->due[j + 1]      = v->due[j];
            v->due_slot[j + 1] = v->due_slot[j];
            j--;
        }
        v->due[j + 1]      = a;
        v->due_slot[j + 1] = t;
    }
}

void wfh_derive(const day_log_t *log, time_t now, const settings_t *s, day_view_t *out) {
    memset(out, 0, sizeof(*out));

    for (int a = 0; a < s->n_actions; a++) {
        const action_def_t *def = &s->actions[a];

        time_t slot   = wfh_first_slot(def, now, s);
        time_t anchor = 0;                 // set when an interval action is done early

        for (int guard = 0; slot > 0 && guard < SLOT_WALK_MAX; guard++) {
            const log_event_t *ev = find_event(log, a, slot);
            slot_state_t state;

            if (ev && ev->kind == KIND_DONE) {
                out->counts[a]++;
                state = SLOT_DONE;
                if (def->cadence.kind == CADENCE_INTERVAL) anchor = ev->ts;   // §5.3
            } else if (ev && ev->kind == KIND_SKIP) {
                out->skipped[a]++;
                state = SLOT_SKIP;
            } else if (slot <= now) {
                // Unanswered and in the past: due while its window is open,
                // missed once it closes. A snooze defers due-ness inside the
                // window without making it a miss — §3.2.
                if (now < wfh_window_close(def, slot, s)) {
                    state = SLOT_DUE;
                    out->open_slot[a] = slot;
                    if (log->snoozed_until[a] <= now) {
                        out->due[out->n_due]        = a;
                        out->due_slot[out->n_due++] = slot;
                    }
                } else {
                    out->missed[a]++;
                    state = SLOT_MISS;
                }
            } else {
                out->next[a] = slot;       // first future slot; stop here
                break;
            }
            if (out->n_slots[a] < SLOTS_MAX) {
                const int k = out->n_slots[a]++;
                out->slots[a][k]     = state;
                out->slot_time[a][k] = slot;
                out->slot_ts[a][k]   = ev ? ev->ts : 0;
            }

            time_t adv = wfh_slot_after(def, anchor ? anchor : slot, s);
            anchor = 0;
            // An anchor earlier than the slot it answered (a tile tap logged
            // well before its slot) could otherwise walk backwards forever.
            if (adv != 0 && adv <= slot) adv = wfh_slot_after(def, slot, s);
            slot = adv;
        }
    }

    sort_due(out, s);
}
