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
        log_event_t *e = &log.events[log.events_len++];
        e->action = a;
        e->kind   = strcmp(fx->events[i].kind, "done") == 0 ? KIND_DONE : KIND_SKIP;
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

int main(void) {
    printf("derive: %d fixtures\n", FIXTURE_COUNT);
    for (int i = 0; i < FIXTURE_COUNT; i++) {
        const int before = g_failures;
        run_fixture(&FIXTURES[i]);
        if (g_failures == before) printf("  ok    %s\n", FIXTURES[i].name);
    }
    if (g_failures) {
        printf("\n%d assertion%s failed\n", g_failures, g_failures == 1 ? "" : "s");
        return 1;
    }
    printf("\nall passed\n");
    return 0;
}
