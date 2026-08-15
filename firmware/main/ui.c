// §7: the grid is the default view; the card takes over when something is
// due. Both are built once and updated in place — see §6 on why rebuilding
// the card every tick is wrong.
#include "ui.h"

#include <stdio.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "day.h"
#include "input.h"

static const char *TAG = "ui";

#define TILE_W  172
#define TILE_H  104
#define GRID_X0   6
#define GRID_Y0  88

// §2's tints, in config order. Kept here rather than in actions.json because
// they are a property of this screen, not of the schedule.
static const uint32_t TINT[] = {
    0x7fd4a8, 0x6ec3e0, 0xb6a3e8, 0xe8b06a, 0xe8926a, 0xe0d16a, 0x8f9aa8,
};

card_t g_card;

static lv_obj_t *g_grid, *g_card_scr;
static lv_obj_t *g_clock, *g_date, *g_sound_icon;
static lv_obj_t *g_tile[ACTIONS_MAX], *g_wash[ACTIONS_MAX];
static lv_obj_t *g_count[ACTIONS_MAX], *g_cd[ACTIONS_MAX];
static lv_obj_t *g_rows[ACTIONS_MAX], *g_checks[ACTIONS_MAX];

// The water surface. Only the crest is animated — the body below it is a
// static gradient — because a fluid simulation is per-pixel work and a
// moving waterline is a polyline. That is the whole trick.
#define WAVE_PTS 25
static lv_obj_t         *g_wave[ACTIONS_MAX];
static lv_point_precise_t g_wave_pts[ACTIONS_MAX][WAVE_PTS];
static uint32_t          g_wave_phase;

// ----------------------------------------------------------------- helpers

static void on_tile_cb(lv_event_t *e) {
    const int a = (int)(intptr_t)lv_event_get_user_data(e);
    input_done(a);                       // §7.1: tapping a tile logs it outright
}

static void on_row_toggle(lv_event_t *e) {
    card_toggle(&g_card, (int)(intptr_t)lv_event_get_user_data(e));
    ui_show_card();
}
static void on_row_skip(lv_event_t *e) {
    card_skip_row(&g_card, (int)(intptr_t)lv_event_get_user_data(e), ui_card_host());
    ui_show_card();
}
static void on_confirm(lv_event_t *e)  { LV_UNUSED(e); card_confirm(&g_card, ui_card_host()); ui_show_card(); }
static void on_delay(lv_event_t *e)    { LV_UNUSED(e); card_delay_all(&g_card, &g_day, time(NULL), ui_card_host()); }
static void on_dismiss(lv_event_t *e)  { LV_UNUSED(e); card_dismiss(&g_card, ui_card_host()); }
static void on_sound_icon(lv_event_t *e) { LV_UNUSED(e); input_toggle_sound(); }

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

    lv_obj_t *hdr = lv_obj_create(g_grid);
    lv_obj_set_size(hdr, 368, 80);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0x161b22), 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    g_clock = lv_label_create(hdr);
    lv_obj_set_style_text_font(g_clock, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(g_clock, lv_color_hex(0xe6edf3), 0);
    lv_obj_align(g_clock, LV_ALIGN_LEFT_MID, 4, -8);

    g_date = lv_label_create(hdr);
    lv_obj_set_style_text_color(g_date, lv_color_hex(0x8b949e), 0);
    lv_obj_align(g_date, LV_ALIGN_LEFT_MID, 4, 14);

    g_sound_icon = lv_label_create(hdr);
    lv_obj_set_style_text_font(g_sound_icon, &lv_font_montserrat_24, 0);
    lv_obj_align(g_sound_icon, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_obj_add_flag(g_sound_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(g_sound_icon, 16);       // 24px glyph, 56px target
    lv_obj_add_event_cb(g_sound_icon, on_sound_icon, LV_EVENT_CLICKED, NULL);

    for (int i = 0; i < g_settings.n_actions; i++) {
        const bool last = (i == g_settings.n_actions - 1);
        // §7.1: shutdown spans the final row — it is the action that ends the
        // day, so the layout reads as a full stop.
        const int w = last ? (TILE_W * 2 + 12) : TILE_W;
        const int x = last ? GRID_X0 : (i % 2) * (TILE_W + 12) + GRID_X0;
        const int y = GRID_Y0 + (i / 2) * (TILE_H + 8);

        lv_obj_t *t = lv_obj_create(g_grid);
        lv_obj_set_size(t, w, TILE_H);
        lv_obj_set_pos(t, x, y);
        lv_obj_set_style_bg_color(t, lv_color_hex(0x161b22), 0);
        lv_obj_set_style_border_color(t, lv_color_hex(TINT[i]), 0);
        lv_obj_set_style_border_width(t, 2, 0);
        lv_obj_set_style_radius(t, 14, 0);
        lv_obj_set_style_pad_all(t, 0, 0);
        // No clip_corner. Clipping children to the rounded corner forces LVGL
        // to allocate a mask layer and blend the tile per-pixel on every
        // redraw — affordable at the 1 Hz tick, ruinous at 30fps, and it was
        // what pinned the LVGL task hard enough to trip the watchdog. The
        // cost is square corners on the wash inside a rounded border.
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

        g_count[i] = lv_label_create(t);
        lv_obj_set_style_text_color(g_count[i], lv_color_hex(0x8b949e), 0);
        lv_obj_align(g_count[i], LV_ALIGN_BOTTOM_LEFT, 8, -8);

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
        lv_obj_set_size(row, 336, 56);
        lv_obj_set_pos(row, 16, 72 + i * 62);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_pad_all(row, 8, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *chk = lv_checkbox_create(row);
        lv_obj_align(chk, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_add_event_cb(chk, on_row_toggle, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *skip = lv_button_create(row);
        lv_obj_set_size(skip, 72, 40);
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

// ----------------------------------------------------------------- updates

void ui_sound_icon_update(void) {
    if (!g_sound_icon) return;
    lv_label_set_text(g_sound_icon, g_settings_sound ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE);
    // Muted is dimmed as well as different: the shape carries the meaning,
    // the weight confirms it. Colour alone would not survive the tints.
    lv_obj_set_style_text_color(g_sound_icon,
        lv_color_hex(g_settings_sound ? 0xE6EDF3 : 0x5A636D), 0);
}

void ui_refresh(void) {
    const time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    char buf[32];
    strftime(buf, sizeof buf, "%H:%M", &tm);
    lv_label_set_text(g_clock, buf);
    strftime(buf, sizeof buf, "%a %d %b", &tm);
    lv_label_set_text(g_date, buf);

    for (int i = 0; i < g_settings.n_actions; i++) {
        const int done = g_view.counts[i];
        lv_label_set_text_fmt(g_count[i], "%d of %d", done, g_settings.actions[i].target);

        const time_t next = g_view.next[i];
        if (next > now) {
            const int mins = (int)((next - now + 59) / 60);
            lv_label_set_text_fmt(g_cd[i], "%dm", mins);

            // The wash is the countdown: the fraction of the approach already
            // elapsed. Interval actions fill across their whole cadence; fixed
            // ones fill across the final hour, because a wash creeping up over
            // the four hours before lunch would be imperceptible anyway, and
            // the last hour is when it carries information.
            const int32_t box = lv_obj_get_content_height(g_tile[i]);
            const time_t span = (g_settings.actions[i].cadence.kind == CADENCE_INTERVAL)
                                ? (time_t)g_settings.actions[i].cadence.every_min * 60
                                : 3600;
            const time_t into = span - (next - now) > 0 ? span - (next - now) : 0;
            lv_obj_set_height(g_wash[i], (int32_t)((int64_t)box * into / span));
        } else {
            lv_label_set_text(g_cd[i], "--");
            lv_obj_set_height(g_wash[i], 0);
        }
        lv_obj_align(g_wash[i], LV_ALIGN_BOTTOM_MID, 0, 0);
    }
    ui_sound_icon_update();
}

void ui_show_grid(void) {
    if (lv_screen_active() != g_grid) lv_screen_load_anim(g_grid, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
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
    if (lv_screen_active() != g_card_scr) {
        lv_screen_load_anim(g_card_scr, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
    }
}

/** ~30fps, and only the waterline moves. Measures its own cost so the
 *  "can the panel do a water effect" question has a number attached. */
static void wave_cb(lv_timer_t *timer) {
    LV_UNUSED(timer);
    static uint32_t frames;
    static uint64_t total_us;
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
            const int32_t x = w * p / (WAVE_PTS - 1);
            // Two sines at different rates so the crest never looks like a
            // repeating sawtooth; amplitude stays under 3px so it reads as
            // surface tension rather than a wave machine.
            const int32_t a = lv_trigo_sin((int16_t)(g_wave_phase + p * 14)) * 3 / 32767;
            const int32_t b = lv_trigo_sin((int16_t)(g_wave_phase * 2 + p * 23)) * 2 / 32767;
            g_wave_pts[i][p].x = x;
            g_wave_pts[i][p].y = crest + a + b;
        }
        lv_line_set_points(g_wave[i], g_wave_pts[i], WAVE_PTS);
    }

    total_us += (uint64_t)(esp_timer_get_time() - t0);
    if (++frames % 150 == 0) {
        ESP_LOGI(TAG, "wave: %llu us/frame over %" LV_PRIu32 " frames", total_us / frames, frames);
    }
}

// ------------------------------------------------------------- card wiring

static void host_done(int a, void *ctx)    { LV_UNUSED(ctx); input_done(a); }
static void host_skip(int a, void *ctx)    { LV_UNUSED(ctx); input_skip(a); }
static void host_card(void *ctx)           { LV_UNUSED(ctx); ui_show_card(); }
static void host_grid(void *ctx)           { LV_UNUSED(ctx); ui_show_grid(); }
static void host_stretch(int a, void *ctx) { LV_UNUSED(ctx); ESP_LOGI(TAG, "stretch due: %s (flow not built yet)", g_settings.actions[a].id); }

static const card_host_t HOST = {
    .log_done = host_done, .log_skip = host_skip,
    .show_card = host_card, .show_grid = host_grid,
    .take_stretch = host_stretch, .ctx = NULL,
};

const card_host_t *ui_card_host(void) { return &HOST; }

void ui_build(void) {
    build_grid();
    build_card();
    ui_refresh();
    lv_timer_create(wave_cb, 33, NULL);        // ~30fps waterline
    lv_screen_load(g_grid);
}
