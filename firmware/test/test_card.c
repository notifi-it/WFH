// Host tests for the checklist card (§6). Scenario-shaped rather than
// fixture-shaped, because the card's behaviour is about a *sequence* —
// check a box, walk away, come back an hour later — not a snapshot.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "card.h"
#include "config.h"
#include "derive.h"

static int g_failures;
static const char *g_case;

static void ck(bool cond, const char *fmt, ...) {
    if (cond) return;
    va_list ap;
    va_start(ap, fmt);
    printf("  FAIL  %s: ", g_case);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    g_failures++;
}

typedef struct {
    int done[16],    n_done;
    int stretch[16], n_stretch;
    int shown_card,  shown_grid;
} calls_t;

static void on_done(int a, void *ctx)    { calls_t *c = ctx; c->done[c->n_done++] = a; }
static void on_card(void *ctx)           { ((calls_t *)ctx)->shown_card++; }
static void on_grid(void *ctx)           { ((calls_t *)ctx)->shown_grid++; }
static void on_stretch(int a, void *ctx) { calls_t *c = ctx; c->stretch[c->n_stretch++] = a; }

static const settings_t *S;
static calls_t   CALLS;
static card_host_t HOST;

static void reset(card_t *c) {
    memset(c, 0, sizeof *c);
    memset(&CALLS, 0, sizeof CALLS);
    HOST = (card_host_t){ .log_done = on_done, .show_card = on_card,
                          .show_grid = on_grid, .take_stretch = on_stretch, .ctx = &CALLS };
}

static int id(const char *name) { return config_action_index(S, name); }

/** Build a due list from action ids, NULL-terminated. */
static int due_list(int *out, ...) {
    va_list ap;
    va_start(ap, out);
    int n = 0;
    for (const char *name = va_arg(ap, const char *); name; name = va_arg(ap, const char *)) {
        out[n++] = id(name);
    }
    va_end(ap);
    return n;
}

static bool holds(const card_t *c, int i, const char *name) {
    return i < c->len && c->rows[i].action == id(name);
}

static bool absent(const card_t *c, const char *name) {
    for (int i = 0; i < c->len; i++) if (c->rows[i].action == id(name)) return false;
    return true;
}

// ---------------------------------------------------------------- scenarios

static void checks_survive_a_resync(void) {
    g_case = "a half-made decision survives the next tick";
    card_t c; reset(&c);
    int due[8]; int n = due_list(due, "stand", "water", "snack", NULL);

    card_sync(&c, due, n, S, 1000, &HOST);
    card_toggle(&c, 0);
    card_toggle(&c, 2);

    // An hour of ticks with the same actions still due.
    const bool changed = card_sync(&c, due, n, S, 4600, &HOST);

    ck(!changed, "resync with an unchanged due set reported a change");
    ck(c.len == 3, "card has %d rows, want 3", c.len);
    ck(c.rows[0].checked && !c.rows[1].checked && c.rows[2].checked,
       "checked state was not sticky: %d/%d/%d",
       c.rows[0].checked, c.rows[1].checked, c.rows[2].checked);
    ck(CALLS.shown_card == 1, "screen was rebuilt %d times, want 1", CALLS.shown_card);
}

static void row_answered_elsewhere_disappears(void) {
    g_case = "a tile tap removes the row";
    card_t c; reset(&c);
    int due[8];

    card_sync(&c, due, due_list(due, "stand", "water", NULL), S, 1000, &HOST);
    card_toggle(&c, 1);                                   // water ticked but not confirmed
    card_sync(&c, due, due_list(due, "stand", NULL), S, 1060, &HOST);

    ck(c.len == 1 && holds(&c, 0, "stand"), "card did not drop the answered row");
    ck(CALLS.n_done == 0, "sync logged something on its own");
}

static void confirm_takes_only_the_checked(void) {
    g_case = "Confirm logs checked rows, snoozes the rest, closes";
    card_t c; reset(&c);
    day_log_t log; memset(&log, 0, sizeof log);
    int due[8];

    card_sync(&c, due, due_list(due, "stand", "water", "snack", NULL), S, 1000, &HOST);
    card_toggle(&c, 0);
    card_toggle(&c, 2);
    card_confirm(&c, &log, 1000, &HOST);

    ck(CALLS.n_done == 2, "logged %d done, want 2", CALLS.n_done);
    ck(log.snoozed_until[id("water")] == 1000 + 900, "unchecked row was not snoozed");
    ck(log.snoozed_until[id("stand")] == 0, "a checked row was snoozed as well as logged");
    ck(c.len == 0 && CALLS.shown_grid == 1, "Confirm did not close the card");
}

static void delay_all_is_absolute(void) {
    g_case = "Delay-all snoozes everything and never compounds";
    card_t c; reset(&c);
    day_log_t log; memset(&log, 0, sizeof log);
    int due[8];

    card_sync(&c, due, due_list(due, "stand", "water", NULL), S, 1000, &HOST);
    card_toggle(&c, 0);
    card_delay_all(&c, &log, 1000, &HOST);

    ck(log.snoozed_until[id("stand")] == 1000 + 900, "checked row was not snoozed");
    ck(log.snoozed_until[id("water")] == 1000 + 900, "unchecked row was not snoozed");
    ck(c.len == 0 && CALLS.shown_grid == 1, "card did not close to the grid");
    ck(CALLS.n_done == 0, "Delay-all logged an answer");

    // Mash it: the deadline restates, it does not stack.
    card_sync(&c, due, due_list(due, "stand", NULL), S, 1000, &HOST);
    card_delay_all(&c, &log, 1000, &HOST);
    ck(log.snoozed_until[id("stand")] == 1000 + 900, "a second press pushed the deadline out");
}

static void stretch_takes_the_screen(void) {
    g_case = "a due stretch pre-empts and the rest keep waiting";
    card_t c; reset(&c);
    int due[8];

    card_sync(&c, due, due_list(due, "stretch", "snack", "stand", NULL), S, 1000, &HOST);

    ck(CALLS.n_stretch == 1 && CALLS.stretch[0] == id("stretch"), "stretch did not take the screen");
    ck(c.len == 2, "card has %d rows, want the 2 non-stretch ones", c.len);
    ck(absent(&c, "stretch"), "the stretch joined the checklist");
    ck(CALLS.shown_card == 0, "the card drew over the stretch");
}

static void derive_feeds_the_card(void) {
    g_case = "derive's due set drives the card end to end";
    card_t c; reset(&c);

    setenv("TZ", S->tz, 1);
    tzset();

    struct tm t;
    memset(&t, 0, sizeof t);
    t.tm_year = 126; t.tm_mon = 7; t.tm_mday = 13; t.tm_hour = 12; t.tm_isdst = -1;
    const time_t ref = mktime(&t);
    struct tm day; localtime_r(&ref, &day);
    const time_t now = wfh_at_time(&day, "11:35");

    day_log_t log; memset(&log, 0, sizeof log);
    day_view_t v;
    wfh_derive(&log, now, S, &v);

    card_sync(&c, v.due, v.n_due, S, now, &HOST);

    ck(v.n_due == 5, "derive found %d due, want 5", v.n_due);
    ck(CALLS.n_stretch == 1, "the 11:30 stretch did not pre-empt");
    ck(c.len == 3, "card has %d rows, want 3", c.len);  // roll is guided now, never a row
    ck(holds(&c, 0, "snack"), "priority order was lost between derive and the card");
}

int main(void) {
    S = config_default();

    void (*cases[])(void) = {
        checks_survive_a_resync, row_answered_elsewhere_disappears,
        confirm_takes_only_the_checked, delay_all_is_absolute,
        stretch_takes_the_screen, derive_feeds_the_card,
    };
    const int n = (int)(sizeof cases / sizeof cases[0]);

    printf("card: %d scenarios\n", n);
    for (int i = 0; i < n; i++) {
        const int before = g_failures;
        cases[i]();
        if (g_failures == before) printf("  ok    %s\n", g_case);
    }
    if (g_failures) { printf("\n%d assertion%s failed\n", g_failures, g_failures == 1 ? "" : "s"); return 1; }
    printf("\nall passed\n");
    return 0;
}
