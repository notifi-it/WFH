// Board simulator: runs the real firmware UI on the laptop and writes a PNG.
//
// This compiles the *same* ui.c the board runs, against a memory display
// instead of a panel. That is the whole point — the HTML mock shows what the
// design should be, and this shows what the firmware actually draws, with
// correct colours and no serial capture in between to misreport them.
//
//   make -C sim && sim/board grid.png
//
// Everything below the UI is stubbed with a plausible day, because the
// question this answers is "what does the screen look like", not "is derive
// right" — derive has its own host tests.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

#include "lvgl.h"

#include "card.h"
#include "config.h"
#include "day.h"
#include "feedback.h"
#include "input.h"
#include "ui.h"

#define W 368
#define H 448

// ------------------------------------------------------------ stubbed board

day_log_t  g_day;
day_view_t g_view;
settings_t g_settings;

void   day_reload(time_t now)                  { (void)now; }
bool   day_apply_event(const log_event_t *ev)  { (void)ev; return true; }
time_t day_current_or_next_slot(int a)         { (void)a; return 0; }
void   input_done(int a)                       { printf("input_done(%s)\n", g_settings.actions[a].id); }
void   input_undo(int a, time_t slot)          { printf("input_undo(%s, %lld)\n", g_settings.actions[a].id, (long long)slot); }
void   feedback_play(cue_t cue)                { printf("feedback_play(%d)\n", (int)cue); }

// The card's "missed HH:MM" reads the due list, so this one is real.
time_t day_due_slot(int a) {
    for (int i = 0; i < g_view.n_due; i++) if (g_view.due[i] == a) return g_view.due_slot[i];
    return 0;
}

/** A late morning, 12:20: some done, some skipped, some missed, all
 *  counting down. Chosen to exercise every dot state and a range of wash
 *  heights; water gets a full timeline for the history screen. */
time_t g_fake_now;                          // what ui.c sees as time(NULL)

static void fake_day(void) {
    g_settings = *config_default();
    const time_t now = time(NULL);

    const int done[]  = { 4, 2, 2, 1, 0, 1 };
    const int skip[]  = { 0, 1, 0, 0, 0, 0 };
    const int miss[]  = { 1, 0, 2, 0, 0, 0 };
    const int mins[]  = { 4, 18, 1, 33, 26, 55 };

    // The same tallies as a timeline, matching design/board-v4c.html.
    static const char *const tl[] = { "DDDDM", "DDS", "DDMM", "U", "", "D" };

    for (int i = 0; i < g_settings.n_actions; i++) {
        g_view.counts[i]  = done[i];
        g_view.skipped[i] = skip[i];
        g_view.missed[i]  = miss[i];
        g_view.next[i]    = now + mins[i] * 60;
        for (const char *p = tl[i]; *p; p++) {
            g_view.slots[i][g_view.n_slots[i]++] =
                *p == 'D' ? SLOT_DONE : *p == 'S' ? SLOT_SKIP : *p == 'U' ? SLOT_DUE : SLOT_MISS;
        }
    }

    // Four slots let slide, for the card and the popup's "missed at" state.
    // Times of day, so the screens read like a real late morning.
    struct tm tm;
    localtime_r(&now, &tm);
    const int due[]     = { 0, 1, 3, 4 };
    const int due_hm[][2] = { { 11, 40 }, { 11, 15 }, { 10, 45 }, { 13, 0 } };
    for (int i = 0; i < 4; i++) {
        tm.tm_hour = due_hm[i][0]; tm.tm_min = due_hm[i][1]; tm.tm_sec = 0;
        g_view.due[i]      = due[i];
        g_view.due_slot[i] = mktime(&tm);
        g_view.open_slot[due[i]] = g_view.due_slot[i];
    }
    g_view.n_due = 4;
    g_day.snoozed_until[1] = now + 12 * 60;     // water: popup's snoozed state

    // Water's day for the history screen, matching design/history-v4.html#water:
    // 09:45 done, 10:30 done, 11:15 skipped, 12:00 open — and "now" is 12:20.
    tm.tm_hour = 12; tm.tm_min = 20; tm.tm_sec = 0;
    g_fake_now = mktime(&tm);
    static const struct { int h, m, at_h, at_m; uint8_t st; } water[] = {
        { 9, 45,  9, 51, SLOT_DONE }, { 10, 30, 10, 29, SLOT_DONE },
        { 11, 15, 11, 20, SLOT_SKIP }, { 12, 0, 0, 0, SLOT_DUE },
    };
    g_view.n_slots[1] = 0;
    for (size_t i = 0; i < sizeof water / sizeof water[0]; i++) {
        tm.tm_hour = water[i].h; tm.tm_min = water[i].m;
        g_view.slot_time[1][i] = mktime(&tm);
        tm.tm_hour = water[i].at_h; tm.tm_min = water[i].at_m;
        g_view.slot_ts[1][i]   = water[i].at_h ? mktime(&tm) : 0;
        g_view.slots[1][i]     = water[i].st;
        g_view.n_slots[1]++;
    }
    tm.tm_hour = 12; tm.tm_min = 45;
    g_view.next[1] = mktime(&tm);
}

// ------------------------------------------------------------- png + display

static uint8_t g_fb[W * H * 2];

static void flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *px) {
    for (int y = area->y1; y <= area->y2; y++) {
        const int row = (y * W + area->x1) * 2;
        const int len = (area->x2 - area->x1 + 1) * 2;
        memcpy(g_fb + row, px, len);
        px += len;
    }
    lv_display_flush_ready(d);
}

static void chunk(FILE *f, const char *tag, const uint8_t *data, uint32_t n) {
    uint8_t len[4] = { n >> 24, n >> 16, n >> 8, n };
    fwrite(len, 1, 4, f);
    fwrite(tag, 1, 4, f);
    fwrite(data, 1, n, f);
    uLong c = crc32(crc32(0, (const Bytef *)tag, 4), data, n);
    uint8_t crc[4] = { c >> 24, c >> 16, c >> 8, c };
    fwrite(crc, 1, 4, f);
}

static void write_png(const char *path) {
    uint8_t *rows = malloc(H * (1 + W * 3));
    size_t o = 0;
    for (int y = 0; y < H; y++) {
        rows[o++] = 0;
        for (int x = 0; x < W; x++) {
            const uint16_t v = g_fb[(y * W + x) * 2] | (g_fb[(y * W + x) * 2 + 1] << 8);
            rows[o++] = ((v >> 11) & 31) * 255 / 31;
            rows[o++] = ((v >> 5) & 63) * 255 / 63;
            rows[o++] = (v & 31) * 255 / 31;
        }
    }
    uLongf zlen = compressBound(o);
    uint8_t *z = malloc(zlen);
    compress2(z, &zlen, rows, o, 6);

    FILE *f = fopen(path, "wb");
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    const uint8_t ihdr[13] = { W >> 24, W >> 16, W >> 8, W, H >> 24, H >> 16, H >> 8, H, 8, 2, 0, 0, 0 };
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (uint32_t)zlen);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(rows); free(z);
    printf("wrote %s (%dx%d)\n", path, W, H);
}

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : "board.png";
    const char *screen = argc > 2 ? argv[2] : "grid";

    lv_init();
    lv_display_t *d = lv_display_create(W, H);
    static uint8_t buf[W * H * 2];
    lv_display_set_buffers(d, buf, NULL, sizeof buf, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(d, flush_cb);

    fake_day();
    ui_build();
    if (strcmp(screen, "popup") == 0) ui_show_popup(1);      // water
    if (strcmp(screen, "stretch") == 0) ui_show_stretch(5);
    if (strcmp(screen, "rollflow") == 0) ui_show_stretch(2);
    if (strcmp(screen, "history") == 0) ui_show_history(1);   // water
    if (strcmp(screen, "dayend") == 0) ui_show_dayend();
    if (strcmp(screen, "card") == 0) {                       // 3-way collision
        g_card.rows[0] = (card_row_t){ .action = 0, .checked = true  };
        g_card.rows[1] = (card_row_t){ .action = 1, .checked = true  };
        g_card.rows[2] = (card_row_t){ .action = 3, .checked = false };
        g_card.rows[3] = (card_row_t){ .action = 4, .checked = false };
        g_card.len = 4;
        ui_show_card();
    }

    // Let LVGL settle: screen load animations and the first full render.
    for (int i = 0; i < 60; i++) { lv_tick_inc(16); lv_timer_handler(); }

    write_png(out);
    return 0;
}
