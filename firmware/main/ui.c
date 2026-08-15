// §7. Three screens: the grid (default), the checklist card when several
// things are due at once, and a per-action popup opened by tapping a tile.
// Built once, updated in place — see §6 on why rebuilding every tick is wrong.
#include "ui.h"

#include <stdio.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "day.h"
#include "input.h"

static const char *TAG = "ui";

// No header bar: the grid owns all 448px. Four rows — three pairs plus the
// full-width shutdown — at 104px with 8px gutters comes to 446.
#define TILE_W  172
#define TILE_H  104
#define GAP       8
#define GRID_X0   6
#define GRID_Y0   6

static const uint32_t TINT[] = {
    0x7fd4a8, 0x6ec3e0, 0xb6a3e8, 0xe8b06a, 0xe8926a, 0xe0d16a, 0x8f9aa8,
};

card_t g_card;

static lv_obj_t *g_grid, *g_card_scr, *g_popup;
static lv_obj_t *g_tile[ACTIONS_MAX], *g_wash[ACTIONS_MAX], *g_cd[ACTIONS_MAX];
static lv_obj_t *g_rows[ACTIONS_MAX], *g_checks[ACTIONS_MAX];

// The popup is one screen reused for whichever tile was tapped.
static int       g_popup_action = -1;
static lv_obj_t *g_pop_name, *g_pop_blurb, *g_pop_state, *g_pop_sound, *g_pop_done;

// §7.2's dot row. Four states, and skipped vs missed stay visually distinct:
// deciding not to eat lunch and forgetting to log lunch are different facts.
#define MAX_DOTS 14
typedef enum { DOT_HIDDEN, DOT_DONE, DOT_SKIP, DOT_MISS, DOT_PEND } dot_state_t;
static lv_obj_t   *g_dot[ACTIONS_MAX][MAX_DOTS];
static dot_state_t g_dot_state[ACTIONS_MAX][MAX_DOTS];
static int         g_dot_shown[ACTIONS_MAX];

// The waterline. Only the crest is animated — the body below is a static
// gradient — because a fluid simulation is per-pixel work and a moving
// surface is geometry.
#define WAVE_PTS 25
static lv_obj_t          *g_wave[ACTIONS_MAX];
static lv_point_precise_t g_wave_pts[ACTIONS_MAX][WAVE_PTS];
static uint32_t           g_wave_phase;

// ------------------------------------------------------------------- popup

static void popup_refresh(void) {
    const int a = g_popup_action;
    if (a < 0 || !g_pop_name) return;

    lv_label_set_text(g_pop_name, g_settings.actions[a].name);
    lv_obj_set_style_text_color(g_pop_name, lv_color_hex(TINT[a]), 0);
    lv_label_set_text(g_pop_blurb, g_settings.actions[a].blurb);
    lv_obj_set_style_bg_color(g_pop_done, lv_color_hex(TINT[a]), 0);

    const time_t now = time(NULL), next = g_view.next[a];
    bool due = false;
    for (int i = 0; i < g_view.n_due; i++) if (g_view.due[i] == a) due = true;

    if (due) {
        lv_label_set_text(g_pop_state, "Due now");
    } else if (next > now) {
        lv_label_set_text_fmt(g_pop_state, "%d done  -  next in %dm",
                              g_view.counts[a], (int)((next - now + 59) / 60));
    } else {
        lv_label_set_text_fmt(g_pop_state, "%d done  -  nothing left today", g_view.counts[a]);
    }
    lv_label_set_text(g_pop_sound, g_settings_sound ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE);
    lv_obj_set_style_text_color(g_pop_sound, lv_color_hex(g_settings_sound ? 0xE6EDF3 : 0x5A636D), 0);
}

static void on_tile_cb(lv_event_t *e) {
    g_popup_action = (int)(intptr_t)lv_event_get_user_data(e);
    popup_refresh();
    lv_screen_load_anim(g_popup, LV_SCR_LOAD_ANIM_FADE_IN, 150, 0, false);
}

static void on_pop_done(lv_event_t *e)   { LV_UNUSED(e); if (g_popup_action >= 0) input_done(g_popup_action); ui_show_grid(); }
static void on_pop_skip(lv_event_t *e)   { LV_UNUSED(e); if (g_popup_action >= 0) input_skip(g_popup_action); ui_show_grid(); }
static void on_pop_close(lv_event_t *e)  { LV_UNUSED(e); ui_show_grid(); }
static void on_sound_icon(lv_event_t *e) { LV_UNUSED(e); input_toggle_sound(); }

// --------------------------------------------------------------- card hooks

static void on_row_toggle(lv_event_t *e) {
    card_toggle(&g_card, (int)(intptr_t)lv_event_get_user_data(e));
    ui_show_card();
}
static void on_row_skip(lv_event_t *e) {
    card_skip_row(&g_card, (int)(intptr_t)lv_event_get_user_data(e), ui_card_host());
    ui_show_card();
}
static void on_confirm(lv_event_t *e) { LV_UNUSED(e); card_confirm(&g_card, ui_card_host()); ui_show_card(); }
static void on_delay(lv_event_t *e)   { LV_UNUSED(e); card_delay_all(&g_card, &g_day, time(NULL), ui_card_host()); }
static void on_dismiss(lv_event_t *e) { LV_UNUSED(e); card_dismiss(&g_card, ui_card_host()); }

/** §7.3a: one X, built one way, everywhere it appears. */
static void x_button(lv_obj_t *parent, lv_event_cb_t on_close) {
    lv_obj_t *x = lv_button_create(parent);
    lv_obj_set_size(x, 44, 44);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, -8, 8);
    lv_obj_set_style_bg_opa(x, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(x, 0, 0);
    lv_obj_t *l = lv_label_create(x);
    lv_label_set_text(l, LV_SYMBOL_CLOSE);
    lv_obj_center(l);
    lv_obj_add_event_cb(x, on_close, LV_EVENT_CLICKED, NULL);
}

// -------------------------------------------------------------------- grid

static void build_grid(void) {
    g_grid = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_grid, lv_color_hex(0x0d1117), 0);
    lv_obj_clear_flag(g_grid, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < g_settings.n_actions; i++) {
        const bool last = (i == g_settings.n_actions - 1);
        const int w = last ? (TILE_W * 2 + GAP + 4) : TILE_W;
        const int x = last ? GRID_X0 : (i % 2) * (TILE_W + GAP + 4) + GRID_X0;
        const int y = GRID_Y0 + (i / 2) * (TILE_H + GAP);

        lv_obj_t *t = lv_obj_create(g_grid);
        lv_obj_set_size(t, w, TILE_H);
        lv_obj_set_pos(t, x, y);
        lv_obj_set_style_bg_color(t, lv_color_hex(0x161b22), 0);
        lv_obj_set_style_border_color(t, lv_color_hex(TINT[i]), 0);
        lv_obj_set_style_border_width(t, 2, 0);
        lv_obj_set_style_radius(t, 14, 0);
        lv_obj_set_style_pad_all(t, 0, 0);
        // No clip_corner: clipping children to the rounded corner makes LVGL
        // allocate a mask layer and blend per-pixel on every redraw. Fine at
        // 1 Hz, but at 30fps it starved the LVGL task and tripped the
        // watchdog. Cost is square corners on the wash inside a round border.
        lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(t, on_tile_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        g_tile[i] = t;

        lv_obj_t *wash = lv_obj_create(t);
        lv_obj_set_width(wash, LV_PCT(100));
        lv_obj_set_height(wash, 0);
        lv_obj_set_style_border_width(wash, 0, 0);
        lv_obj_set_style_radius(wash, 0, 0);
        lv_obj_set_style_pad_all(wash, 0, 0);
        lv_obj_set_style_bg_color(wash, lv_color_hex(TINT[i]), 0);
        lv_obj_set_style_bg_grad_color(wash, lv_color_hex(0x161b22), 0);
        lv_obj_set_style_bg_grad_dir(wash, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_main_stop(wash, 255, 0);
        lv_obj_set_style_bg_grad_stop(wash, 0, 0);
        lv_obj_set_style_bg_opa(wash, LV_OPA_40, 0);
        lv_obj_clear_flag(wash, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(wash, LV_ALIGN_BOTTOM_MID, 0, 0);
        g_wash[i] = wash;

        lv_obj_t *wave = lv_line_create(t);
        lv_obj_set_style_line_color(wave, lv_color_hex(TINT[i]), 0);
        lv_obj_set_style_line_width(wave, 2, 0);
        lv_obj_set_style_line_rounded(wave, true, 0);
        lv_obj_set_style_line_opa(wave, LV_OPA_80, 0);
        lv_obj_clear_flag(wave, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        g_wave[i] = wave;

        lv_obj_t *name = lv_label_create(t);
        lv_label_set_text(name, g_settings.actions[i].name);
        lv_obj_set_style_text_color(name, lv_color_hex(TINT[i]), 0);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 8, 8);

        for (int d = 0; d < MAX_DOTS; d++) {
            lv_obj_t *dot = lv_obj_create(t);
            lv_obj_set_style_border_width(dot, 0, 0);
            lv_obj_set_style_pad_all(dot, 0, 0);
            lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
            g_dot[i][d] = dot;
        }

        g_cd[i] = lv_label_create(t);
        lv_obj_set_style_text_font(g_cd[i], &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(g_cd[i], lv_color_hex(0xe6edf3), 0);
        lv_obj_align(g_cd[i], LV_ALIGN_BOTTOM_RIGHT, -8, -8);
    }
}

// -------------------------------------------------------------------- card

static void build_card(void) {
    g_card_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_card_scr, lv_color_hex(0x0d1117), 0);
    lv_obj_clear_flag(g_card_scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(g_card_scr);
    lv_label_set_text(title, "Due now");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xe6edf3), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 20);

    for (int i = 0; i < ACTIONS_MAX; i++) {
        lv_obj_t *row = lv_obj_create(g_card_scr);
        lv_obj_set_size(row, 336, 52);
        lv_obj_set_pos(row, 16, 68 + i * 58);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_pad_all(row, 8, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *chk = lv_checkbox_create(row);
        lv_obj_align(chk, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_add_event_cb(chk, on_row_toggle, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *skip = lv_button_create(row);
        lv_obj_set_size(skip, 68, 36);
        lv_obj_align(skip, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(skip, lv_color_hex(0x21262d), 0);
        lv_obj_t *sl = lv_label_create(skip);
        lv_label_set_text(sl, "Skip");
        lv_obj_center(sl);
        lv_obj_add_event_cb(skip, on_row_skip, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        g_rows[i] = row;
        g_checks[i] = chk;
    }

    lv_obj_t *confirm = lv_button_create(g_card_scr);
    lv_obj_set_size(confirm, 200, 56);
    lv_obj_align(confirm, LV_ALIGN_BOTTOM_LEFT, 16, -16);
    lv_obj_set_style_bg_color(confirm, lv_color_hex(0x2ea043), 0);
    lv_obj_t *cl = lv_label_create(confirm);
    lv_label_set_text(cl, "Confirm");
    lv_obj_center(cl);
    lv_obj_add_event_cb(confirm, on_confirm, LV_EVENT_CLICKED, NULL);

    lv_obj_t *delay = lv_button_create(g_card_scr);
    lv_obj_set_size(delay, 120, 56);
    lv_obj_align(delay, LV_ALIGN_BOTTOM_RIGHT, -16, -16);
    lv_obj_set_style_bg_color(delay, lv_color_hex(0x21262d), 0);
    lv_obj_t *dl = lv_label_create(delay);
    lv_label_set_text(dl, "+15 min");
    lv_obj_center(dl);
    lv_obj_add_event_cb(delay, on_delay, LV_EVENT_CLICKED, NULL);

    x_button(g_card_scr, on_dismiss);
}

// ------------------------------------------------------------------- popup

static void build_popup(void) {
    g_popup = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_popup, lv_color_hex(0x0d1117), 0);
    lv_obj_clear_flag(g_popup, LV_OBJ_FLAG_SCROLLABLE);

    // The sound toggle lives here now the header is gone. §7.5's point still
    // holds — the icon *is* the state — it just needs a home, and the popup
    // is the one screen you always pass through to answer anything.
    g_pop_sound = lv_label_create(g_popup);
    lv_obj_set_style_text_font(g_pop_sound, &lv_font_montserrat_24, 0);
    lv_obj_align(g_pop_sound, LV_ALIGN_TOP_LEFT, 16, 18);
    lv_obj_add_flag(g_pop_sound, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(g_pop_sound, 16);
    lv_obj_add_event_cb(g_pop_sound, on_sound_icon, LV_EVENT_CLICKED, NULL);

    g_pop_name = lv_label_create(g_popup);
    lv_obj_set_style_text_font(g_pop_name, &lv_font_montserrat_24, 0);
    lv_obj_align(g_pop_name, LV_ALIGN_TOP_LEFT, 16, 76);

    g_pop_blurb = lv_label_create(g_popup);
    lv_obj_set_width(g_pop_blurb, 336);
    lv_label_set_long_mode(g_pop_blurb, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(g_pop_blurb, lv_color_hex(0x8b949e), 0);
    lv_obj_align(g_pop_blurb, LV_ALIGN_TOP_LEFT, 16, 118);

    g_pop_state = lv_label_create(g_popup);
    lv_obj_set_style_text_color(g_pop_state, lv_color_hex(0x6e7681), 0);
    lv_obj_align(g_pop_state, LV_ALIGN_TOP_LEFT, 16, 200);

    g_pop_done = lv_button_create(g_popup);
    lv_obj_set_size(g_pop_done, 336, 84);
    lv_obj_align(g_pop_done, LV_ALIGN_BOTTOM_MID, 0, -104);
    lv_obj_t *dlab = lv_label_create(g_pop_done);
    lv_label_set_text(dlab, "Done");
    lv_obj_set_style_text_font(dlab, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(dlab, lv_color_hex(0x0d1117), 0);
    lv_obj_center(dlab);
    lv_obj_add_event_cb(g_pop_done, on_pop_done, LV_EVENT_CLICKED, NULL);

    lv_obj_t *skip = lv_button_create(g_popup);
    lv_obj_set_size(skip, 336, 60);
    lv_obj_align(skip, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(skip, lv_color_hex(0x21262d), 0);
    lv_obj_t *slab = lv_label_create(skip);
    lv_label_set_text(slab, "Skip");
    lv_obj_center(slab);
    lv_obj_add_event_cb(skip, on_pop_skip, LV_EVENT_CLICKED, NULL);

    x_button(g_popup, on_pop_close);
}

// ----------------------------------------------------------------- updates

/** Paint a dot only when its state actually changed. Every style write
 *  invalidates the tile, and the tile also hosts a 30fps waterline —
 *  repainting 70 unchanged dots a second would double the redraw work. */
static void dot_set(int a, int d, dot_state_t s, uint32_t tint) {
    if (g_dot_state[a][d] == s) return;
    g_dot_state[a][d] = s;

    lv_obj_t *o = g_dot[a][d];
    if (s == DOT_HIDDEN) { lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);

    switch (s) {
    case DOT_DONE:                                     // filled, action tint
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(o, lv_color_hex(tint), 0);
        lv_obj_set_style_border_width(o, 0, 0);
        break;
    case DOT_SKIP:                                     // mid-grey solid
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(o, lv_color_hex(0x6e7681), 0);
        lv_obj_set_style_border_width(o, 0, 0);
        break;
    case DOT_MISS:                                     // hollow ring
        lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(o, lv_color_hex(0x6e7681), 0);
        lv_obj_set_style_border_width(o, 1, 0);
        break;
    default:                                           // pending: dark solid
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(o, lv_color_hex(0x30363d), 0);
        lv_obj_set_style_border_width(o, 0, 0);
        break;
    }
}

static void dots_refresh(int a) {
    const int done = g_view.counts[a], skip = g_view.skipped[a], miss = g_view.missed[a];

    // An overshoot grows the row rather than truncating: the day did what it
    // did, and a tile that hides work is worse than one that runs long.
    int n = done + skip + miss;
    if (n < g_settings.actions[a].target) n = g_settings.actions[a].target;
    if (n > MAX_DOTS) n = MAX_DOTS;

    if (n != g_dot_shown[a]) {                          // lay the row out once
        g_dot_shown[a] = n;
        const int32_t avail = lv_obj_get_content_width(g_tile[a]) - 16 - 56;
        int pitch = n > 0 ? (int)(avail / n) : 11;
        if (pitch > 11) pitch = 11;
        const int size = (pitch - 3) < 4 ? 4 : (pitch - 3);
        for (int d = 0; d < n; d++) {
            lv_obj_set_size(g_dot[a][d], size, size);
            lv_obj_set_style_radius(g_dot[a][d], size, 0);
            lv_obj_align(g_dot[a][d], LV_ALIGN_BOTTOM_LEFT, 8 + d * pitch, -12);
        }
        for (int d = n; d < MAX_DOTS; d++) dot_set(a, d, DOT_HIDDEN, 0);
    }

    // Fixed order — done, skipped, missed, then pending — so a tile's dots
    // never reshuffle as the day fills in.
    for (int d = 0; d < n; d++) {
        const dot_state_t s = d < done               ? DOT_DONE
                            : d < done + skip        ? DOT_SKIP
                            : d < done + skip + miss ? DOT_MISS
                                                     : DOT_PEND;
        dot_set(a, d, s, TINT[a]);
    }
}

void ui_sound_icon_update(void) { popup_refresh(); }

void ui_refresh(void) {
    const time_t now = time(NULL);

    for (int i = 0; i < g_settings.n_actions; i++) {
        dots_refresh(i);

        const time_t next = g_view.next[i];
        if (next > now) {
            lv_label_set_text_fmt(g_cd[i], "%dm", (int)((next - now + 59) / 60));

            // The wash is the countdown: how much of the approach has elapsed.
            // Interval actions fill across their whole cadence; fixed ones
            // across the final hour, since a wash creeping up over the four
            // hours before lunch would be imperceptible anyway.
            const int32_t box = lv_obj_get_content_height(g_tile[i]);
            const time_t span = (g_settings.actions[i].cadence.kind == CADENCE_INTERVAL)
                                ? (time_t)g_settings.actions[i].cadence.every_min * 60
                                : 3600;
            time_t into = span - (next - now);
            if (into < 0) into = 0;
            lv_obj_set_height(g_wash[i], (int32_t)((int64_t)box * into / span));
        } else {
            lv_label_set_text(g_cd[i], "--");
            lv_obj_set_height(g_wash[i], 0);
        }
        lv_obj_align(g_wash[i], LV_ALIGN_BOTTOM_MID, 0, 0);
    }
    if (lv_screen_active() == g_popup) popup_refresh();
}

void ui_show_grid(void) {
    if (lv_screen_active() != g_grid) {
        lv_screen_load_anim(g_grid, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
    }
}

void ui_show_card(void) {
    for (int i = 0; i < ACTIONS_MAX; i++) {
        if (i < g_card.len) {
            const int a = g_card.rows[i].action;
            lv_obj_clear_flag(g_rows[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(g_rows[i], lv_color_hex(TINT[a]), 0);
            lv_obj_set_style_bg_opa(g_rows[i], LV_OPA_20, 0);
            lv_checkbox_set_text(g_checks[i], g_settings.actions[a].name);
            if (g_card.rows[i].checked) lv_obj_add_state(g_checks[i], LV_STATE_CHECKED);
            else                        lv_obj_remove_state(g_checks[i], LV_STATE_CHECKED);
        } else {
            lv_obj_add_flag(g_rows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    // Never take the screen away from a popup the user is mid-decision on.
    if (lv_screen_active() != g_card_scr && lv_screen_active() != g_popup) {
        lv_screen_load_anim(g_card_scr, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
    }
}

/** ~30fps, and only the waterline moves. */
static void wave_cb(lv_timer_t *timer) {
    LV_UNUSED(timer);
    static uint32_t frames;
    static uint64_t total_us;

    if (lv_screen_active() != g_grid) return;      // nothing to animate off-grid

    const int64_t t0 = esp_timer_get_time();
    g_wave_phase += 12;

    for (int i = 0; i < g_settings.n_actions; i++) {
        const int32_t box = lv_obj_get_content_height(g_tile[i]);
        const int32_t h   = lv_obj_get_height(g_wash[i]);
        if (h <= 1 || h >= box) { lv_obj_add_flag(g_wave[i], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_clear_flag(g_wave[i], LV_OBJ_FLAG_HIDDEN);

        const int32_t w = lv_obj_get_content_width(g_tile[i]);
        const int32_t crest = box - h;
        for (int p = 0; p < WAVE_PTS; p++) {
            // Two sines at different rates so the crest never reads as a
            // repeating sawtooth; amplitude under 3px keeps it surface
            // tension rather than a wave machine.
            const int32_t a = lv_trigo_sin((int16_t)(g_wave_phase + p * 14)) * 3 / 32767;
            const int32_t b = lv_trigo_sin((int16_t)(g_wave_phase * 2 + p * 23)) * 2 / 32767;
            g_wave_pts[i][p].x = w * p / (WAVE_PTS - 1);
            g_wave_pts[i][p].y = crest + a + b;
        }
        lv_line_set_points(g_wave[i], g_wave_pts[i], WAVE_PTS);
    }

    total_us += (uint64_t)(esp_timer_get_time() - t0);
    if (++frames % 300 == 0) {
        ESP_LOGI(TAG, "wave: %llu us/frame over %" LV_PRIu32 " frames", total_us / frames, frames);
    }
}

// ------------------------------------------------------------- card wiring

static void host_done(int a, void *ctx)    { LV_UNUSED(ctx); input_done(a); }
static void host_skip(int a, void *ctx)    { LV_UNUSED(ctx); input_skip(a); }
static void host_card(void *ctx)           { LV_UNUSED(ctx); ui_show_card(); }
static void host_grid(void *ctx)           { LV_UNUSED(ctx); ui_show_grid(); }
static void host_stretch(int a, void *ctx) {
    LV_UNUSED(ctx);
    ESP_LOGI(TAG, "stretch due: %s (guided flow not built yet)", g_settings.actions[a].id);
}

static const card_host_t HOST = {
    .log_done = host_done, .log_skip = host_skip,
    .show_card = host_card, .show_grid = host_grid,
    .take_stretch = host_stretch, .ctx = NULL,
};

const card_host_t *ui_card_host(void) { return &HOST; }

void ui_build(void) {
    build_grid();
    build_card();
    build_popup();
    ui_refresh();
    lv_timer_create(wave_cb, 33, NULL);
    lv_screen_load(g_grid);
}
