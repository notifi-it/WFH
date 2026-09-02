// Host test for the slot walk (§11.1). Builds with plain cc — no ESP-IDF,
// no board, runs in milliseconds. `derive` is a pure function of
// (log, now, settings), which is the whole reason this is possible.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "derive.h"
#include "fixtures.g.h"

static int g_failures;
static const char *g_fixture;

static void failf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("  FAIL  %s: ", g_fixture);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    g_failures++;
}

static int atoi_n(const char *s, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) v = v * 10 + (s[i] - '0');
    return v;
}

/** Midday on the fixture's date. Noon rather than midnight because midnight
 *  is the hour a DST transition can delete, and we only need a handle on the
 *  calendar day — wfh_at_time does the real work from here. */
static time_t day_noon(const char *ymd) {
    struct tm t;
    memset(&t, 0, sizeof t);
    t.tm_year  = atoi_n(ymd, 4) - 1900;
    t.tm_mon   = atoi_n(ymd + 5, 2) - 1;
    t.tm_mday  = atoi_n(ymd + 8, 2);
    t.tm_hour  = 12;
    t.tm_isdst = -1;
    return mktime(&t);
}

static time_t hhmm_on(time_t ref, const char *hhmm) {
    struct tm day;
    localtime_r(&ref, &day);
    return wfh_at_time(&day, hhmm);
}

static const char *clock_str(time_t t) {
    static char buf[4][32];
    static int slot;
    char *b = buf[slot++ % 4];
    if (t == 0) { snprintf(b, 32, "none"); return b; }
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(b, 32, "%02d:%02d", tm.tm_hour, tm.tm_min);
    return b;
}

static void check_map(const char *what, const settings_t *s, const int *got,
                      const fx_ki_t *want, int n_want) {
    int expect[ACTIONS_MAX] = {0};
    for (int i = 0; i < n_want; i++) {
        const int a = config_action_index(s, want[i].key);
        if (a < 0) { failf("expect.%s names unknown action '%s'", what, want[i].key); return; }
        expect[a] = want[i].value;
    }
    for (int a = 0; a < s->n_actions; a++) {
        if (got[a] != expect[a]) {
            failf("%s[%s] = %d, want %d", what, s->actions[a].id, got[a], expect[a]);
        }
    }
}

static void run_fixture(const fixture_t *fx) {
    g_fixture = fx->name;

    setenv("TZ", fx->tz, 1);
    tzset();

    settings_t s = *config_default();
    if (fx->actions)    { s.actions = fx->actions; s.n_actions = fx->n_actions; }
    if (fx->work_start) s.work_start = fx->work_start;
    if (fx->work_end)   s.work_end   = fx->work_end;
    if (fx->grace_min >= 0) s.grace_min = fx->grace_min;

    const time_t ref = day_noon(fx->date);
    const time_t now = hhmm_on(ref, fx->now);

    day_log_t log;
    memset(&log, 0, sizeof log);

    for (int i = 0; i < fx->n_events; i++) {
        const int a = config_action_index(&s, fx->events[i].action);
        if (a < 0) { failf("event names unknown action '%s'", fx->events[i].action); return; }
        event_kind_t kind;
        if (!wfh_kind_parse(fx->events[i].kind, &kind)) { failf("event has unknown kind '%s'", fx->events[i].kind); return; }
        log_event_t *e = &log.events[log.events_len++];
        e->action = a;
        e->kind   = kind;
        e->ts     = hhmm_on(ref, fx->events[i].ts);
        e->slot   = hhmm_on(ref, fx->events[i].slot);
    }
    for (int i = 0; i < fx->n_snoozes; i++) {
        const int a = config_action_index(&s, fx->snoozes[i].key);
        if (a < 0) { failf("snooze names unknown action '%s'", fx->snoozes[i].key); return; }
        log.snoozed_until[a] = hhmm_on(ref, fx->snoozes[i].value);
    }

    day_view_t v;
    wfh_derive(&log, now, &s, &v);

    check_map("counts",  &s, v.counts,  fx->counts,  fx->n_counts);
    check_map("skipped", &s, v.skipped, fx->skipped, fx->n_skipped);
    check_map("missed",  &s, v.missed,  fx->missed,  fx->n_missed);

    for (int i = 0; i < fx->n_next; i++) {
        const int a = config_action_index(&s, fx->next[i].key);
        if (a < 0) { failf("expect.next names unknown action '%s'", fx->next[i].key); continue; }
        const time_t want = hhmm_on(ref, fx->next[i].value);
        if (v.next[a] != want) {
            failf("next[%s] = %s, want %s", s.actions[a].id, clock_str(v.next[a]), clock_str(want));
        }
    }
    for (int i = 0; i < fx->n_next_none; i++) {
        const int a = config_action_index(&s, fx->next_none[i]);
        if (a >= 0 && v.next[a] != 0) {
            failf("next[%s] = %s, want none", s.actions[a].id, clock_str(v.next[a]));
        }
    }
    for (int i = 0; i < fx->n_next_epoch; i++) {
        const int a = config_action_index(&s, fx->next_epoch[i].key);
        if (a < 0) continue;
        if ((long long)v.next[a] != fx->next_epoch[i].value) {
            failf("next[%s] epoch = %lld, want %lld  (%s vs hand-computed)",
                  s.actions[a].id, (long long)v.next[a], fx->next_epoch[i].value,
                  clock_str(v.next[a]));
        }
    }

    if (v.n_due != fx->n_due) {
        failf("due has %d entries, want %d", v.n_due, fx->n_due);
        for (int i = 0; i < v.n_due; i++) printf("          got due[%d] = %s\n", i, s.actions[v.due[i]].id);
    } else {
        for (int i = 0; i < fx->n_due; i++) {
            if (strcmp(s.actions[v.due[i]].id, fx->due[i]) != 0) {
                failf("due[%d] = %s, want %s", i, s.actions[v.due[i]].id, fx->due[i]);
            }
        }
    }

    if (strcmp(fx->mode, "replay") == 0) {
        day_log_t twice = log;
        for (int i = 0; i < log.events_len; i++) twice.events[twice.events_len++] = log.events[i];

        day_view_t again;
        wfh_derive(&twice, now, &s, &again);
        if (memcmp(&v, &again, sizeof v) != 0) failf("applying the log twice changed the view");
    }
}

/** Water every 45 from 09:00: 09:45, 10:30, 11:15 let slide, 12:00 answered
 *  at 12:05. The row must read miss, miss, miss, done — the fill sits on
 *  the slot that was answered, not on the first dot. */
static void timeline_fills_the_answered_slot(void) {
    g_fixture = "timeline: the dot that fills is the slot answered";
    setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0", 1);
    tzset();
    settings_t s = *config_default();
    const int a = config_action_index(&s, "water");
    const time_t ref = day_noon("2026-09-02");

    day_log_t log;
    memset(&log, 0, sizeof log);
    log_event_t *e = &log.events[log.events_len++];
    e->action = a; e->kind = KIND_DONE;
    e->ts = hhmm_on(ref, "12:05"); e->slot = hhmm_on(ref, "12:00");

    day_view_t v;
    wfh_derive(&log, hhmm_on(ref, "12:10"), &s, &v);

    static const uint8_t want[] = { SLOT_MISS, SLOT_MISS, SLOT_MISS, SLOT_DONE };
    if (v.n_slots[a] != 4) failf("n_slots = %d, want 4", v.n_slots[a]);
    for (int i = 0; i < 4 && i < v.n_slots[a]; i++) {
        if (v.slots[a][i] != want[i]) failf("slot %d = %d, want %d", i, v.slots[a][i], want[i]);
    }
    if (v.counts[a] != 1 || v.missed[a] != 3) failf("tallies %d done / %d missed, want 1 / 3", v.counts[a], v.missed[a]);
}

/** Snack at 10:45, unanswered and snoozed at 12:10: off the due list, but
 *  still the open slot — the popup keeps saying "missed at 10:45". */
static void open_slot_survives_a_snooze(void) {
    g_fixture = "open slot: a snooze hides it from due, not from the popup";
    setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0", 1);
    tzset();
    settings_t s = *config_default();
    const int a = config_action_index(&s, "snack");
    const time_t ref = day_noon("2026-09-02");

    day_log_t log;
    memset(&log, 0, sizeof log);
    log.snoozed_until[a] = hhmm_on(ref, "12:25");

    day_view_t v;
    wfh_derive(&log, hhmm_on(ref, "12:10"), &s, &v);

    if (v.open_slot[a] != hhmm_on(ref, "10:45")) failf("open_slot = %s, want 10:45", clock_str(v.open_slot[a]));
    for (int i = 0; i < v.n_due; i++) if (v.due[i] == a) failf("a snoozed slot is still on the due list");
    if (v.next[a] != hhmm_on(ref, "15:30")) failf("next = %s, want 15:30", clock_str(v.next[a]));
}

/** Done at 09:51 on the 09:45 water, undone at 09:58: the latest event
 *  wins, so at 10:00 the slot is open again and counts nothing. */
static void undo_reopens_the_slot(void) {
    g_fixture = "undo: the latest event for a slot is the truth";
    setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0", 1);
    tzset();
    settings_t s = *config_default();
    const int a = config_action_index(&s, "water");
    const time_t ref = day_noon("2026-09-02");

    day_log_t log;
    memset(&log, 0, sizeof log);
    log.events[log.events_len++] = (log_event_t){ .action = a, .kind = KIND_DONE, .ts = hhmm_on(ref, "09:51"), .slot = hhmm_on(ref, "09:45") };
    log.events[log.events_len++] = (log_event_t){ .action = a, .kind = KIND_UNDO, .ts = hhmm_on(ref, "09:58"), .slot = hhmm_on(ref, "09:45") };

    day_view_t v;
    wfh_derive(&log, hhmm_on(ref, "10:00"), &s, &v);

    if (v.counts[a] != 0) failf("counts = %d after undo, want 0", v.counts[a]);
    if (v.open_slot[a] != hhmm_on(ref, "09:45")) failf("open_slot = %s, want 09:45", clock_str(v.open_slot[a]));
    if (v.n_slots[a] < 1 || v.slots[a][0] != SLOT_DUE) failf("slot 0 state = %d, want due", v.n_slots[a] ? v.slots[a][0] : -1);
    if (v.n_slots[a] < 1 || v.slot_ts[a][0] != 0) failf("slot 0 keeps a tap time after undo");
    if (v.n_slots[a] < 1 || v.slot_time[a][0] != hhmm_on(ref, "09:45")) failf("slot 0 time wrong");
}

int main(void) {
    printf("derive: %d fixtures\n", FIXTURE_COUNT);
    for (int i = 0; i < FIXTURE_COUNT; i++) {
        const int before = g_failures;
        run_fixture(&FIXTURES[i]);
        if (g_failures == before) printf("  ok    %s\n", FIXTURES[i].name);
    }
    void (*const extra[])(void) = { timeline_fills_the_answered_slot, open_slot_survives_a_snooze, undo_reopens_the_slot };
    for (size_t i = 0; i < sizeof extra / sizeof extra[0]; i++) {
        const int before = g_failures;
        extra[i]();
        if (g_failures == before) printf("  ok    %s\n", g_fixture);
    }
    if (g_failures) {
        printf("\n%d assertion%s failed\n", g_failures, g_failures == 1 ? "" : "s");
        return 1;
    }
    printf("\nall passed\n");
    return 0;
}
