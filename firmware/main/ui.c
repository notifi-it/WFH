// §7. Three screens: the grid (default), the checklist card when several
// things are due at once, and a per-action popup opened by tapping a tile.
// Built once, updated in place — see §6 on why rebuilding every tick is wrong.
//
// v4 "colour block" design — transcribed from design/board-v4c.html,
// design/popup-v4.html and design/card-v4.html. Every colour is the tint
// mixed into PAGE_BG; every screen is rects + alpha + one line per tile.
#include "ui.h"

#include <stdio.h>
#include <time.h>

#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "icons.h"

#include "config.h"
#include "day.h"
#include "derive.h"
#include "feedback.h"
#include "input.h"

#define PANEL_W 368
#define PANEL_H 448

// Tiles fill the panel edge-to-edge; the seam is the black screen showing
// through.
#define TILE_W  183
#define TILE_H  148
#define SEAM      2
_Static_assert(2 * TILE_W + SEAM == PANEL_W, "two tiles and a seam span the panel width");
_Static_assert(3 * TILE_H + 2 * SEAM == PANEL_H, "three tiles and two seams span the panel height");

// Every full-width element on the card, popup, history and flow screens —
// rows, buttons, rules — sits inside a 16px margin.
#define MARGIN    16
#define CONTENT_W (PANEL_W - 2 * MARGIN)

// The popup's two stat columns sit this far either side of centre.
#define POP_COL_DX 84

// The quiet button fill: 8% white over PAGE_BG.
#define BTN_QUIET_OPA 20

// Everything tinted is TINT mixed into this base. 13% for tile and row
// bodies, 34% for the wash pool — the two mixes that define the design.
#define PAGE_BG  0x0a0d11
#define MIX_BODY 33          // 13% of 255
#define MIX_WASH 87          // 34% of 255

#define COL_TEXT   0xe6edf3
#define COL_BLURB  0x92989d  // 62% white over PAGE_BG
#define COL_STATE  0x6d7277  // 45%
#define COL_SKIP   0x6e7681
#define COL_PEND   0x2c343e

static const uint32_t TINT[] = {
    0x7fd4a8, 0x6ec3e0, 0xb6a3e8, 0xe8b06a, 0xe8926a, 0xe0d16a,
};

// A8 alpha art (MingCute, 64px), recoloured to the action's tint at runtime
// and rescaled per screen: 64 on the grid, 88 on the popup, 40 on the card.
// Order matches config/actions.json, which is also what TINT is indexed by.
static const lv_image_dsc_t *ICON[] = {
    &icon_stand, &icon_water, &icon_roll, &icon_snack, &icon_lunch, &icon_stretch,
};

card_t g_card;

static lv_obj_t *g_grid, *g_card_scr, *g_popup;
static lv_obj_t *g_tile[ACTIONS_MAX], *g_wash[ACTIONS_MAX];
static lv_obj_t *g_rows[ACTIONS_MAX], *g_row_icon[ACTIONS_MAX],
                *g_row_name[ACTIONS_MAX], *g_row_sub[ACTIONS_MAX], *g_row_mark[ACTIONS_MAX],
                *g_row_chk[ACTIONS_MAX];
static lv_obj_t *g_confirm, *g_confirm_label;

// One action's day (§7.7): head + a scrolling list of that action's slots.
// Rows are rebuilt on every open — the screen is rare, the list is short.
static lv_obj_t *g_hist, *g_hist_icon, *g_hist_name, *g_hist_sub, *g_hist_list;
static int       g_hist_action = -1;

// The popup is one screen reused for whichever tile was tapped.
static int       g_popup_action = -1;
static lv_obj_t *g_pop_name, *g_pop_blurb, *g_pop_next_lbl, *g_pop_next,
                *g_pop_miss_lbl, *g_pop_miss, *g_pop_miss_sub, *g_pop_rule, *g_pop_done, *g_pop_done_lbl,
                *g_pop_icon;

// §7.2's dot row. Four states, and skipped vs missed stay visually distinct:
// deciding not to eat lunch and forgetting to log lunch are different facts.
#define MAX_DOTS 14
#define DOT_INSET 14
typedef enum { DOT_HIDDEN, DOT_DONE, DOT_SKIP, DOT_MISS, DOT_PEND } dot_state_t;
static lv_obj_t   *g_dot[ACTIONS_MAX][MAX_DOTS];
static dot_state_t g_dot_state[ACTIONS_MAX][MAX_DOTS];
static int         g_dot_shown[ACTIONS_MAX];

// The guided flow (§7.6): one step per screen, a ring counting down, next
// step at zero. Serves the stretch set and any other FLOW_STRETCH action
// (shoulder roll), which gets a single synthesized step from its blurb.
#define FLOW_MAX_STEPS 8
#define FLOW_SINGLE_SECONDS 30
static lv_obj_t *g_flow, *g_flow_pill[FLOW_MAX_STEPS], *g_flow_arc,
                *g_flow_secs, *g_flow_name, *g_flow_cue;
static int                  g_flow_action = -1;
static const stretch_def_t *g_flow_steps;
static int                  g_flow_n, g_flow_i;
static time_t               g_flow_end;      // absolute, §7.6: no decrementing
static bool                 g_flow_running;  // false = waiting on Start
static lv_obj_t            *g_flow_pri;
static stretch_def_t        g_flow_single;

// The waterline. Only the crest is animated — the body below is a solid
// block — because a fluid simulation is per-pixel work and a moving
// surface is geometry.
#define WAVE_PTS 61         // 3px segments; with float points the crest is sub-pixel
static lv_obj_t          *g_wave[ACTIONS_MAX];
static lv_point_precise_t g_wave_pts[ACTIONS_MAX][WAVE_PTS];
static uint32_t           g_wave_phase;

static lv_color_t body_of(int a) { return lv_color_mix(lv_color_hex(TINT[a]), lv_color_hex(PAGE_BG), MIX_BODY); }
static lv_color_t wash_of(int a) { return lv_color_mix(lv_color_hex(TINT[a]), lv_color_hex(PAGE_BG), MIX_WASH); }

// ------------------------------------------------------------------- popup

static void hhmm(char *out, size_t n, time_t t) {
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, n, "%H:%M", &tm);
}

static void popup_refresh(void) {
    const int a = g_popup_action;
    if (a < 0 || !g_pop_name) return;

    lv_obj_set_style_bg_color(g_popup, body_of(a), 0);
    lv_image_set_src(g_pop_icon, ICON[a]);
    lv_obj_set_style_image_recolor(g_pop_icon, lv_color_hex(TINT[a]), 0);
    lv_label_set_text(g_pop_name, g_settings.actions[a].name);
    lv_label_set_text(g_pop_blurb, g_settings.actions[a].blurb);
    lv_obj_set_style_bg_color(g_pop_done, lv_color_hex(TINT[a]), 0);
    lv_label_set_text(g_pop_done_lbl, g_settings.actions[a].flow == FLOW_STRETCH ? "Start" : "Done");
    lv_obj_set_style_text_color(g_pop_next, lv_color_hex(TINT[a]), 0);

    // open_slot, not the due list: a snoozed slot is still the one being
    // asked about, and a popup that only says "next 15:30" while 10:45 sits
    // unanswered reads as though 10:45 never happened.
    const time_t now = time(NULL), next = g_view.next[a], slot = g_view.open_slot[a];
    const time_t snoozed = g_day.snoozed_until[a] > now ? g_day.snoozed_until[a] : 0;
    char at[8];

    // The next column sits centred on its own, or right of the hairline
    // when there is a missed slot to show on the left.
    const int32_t col = slot ? POP_COL_DX : 0;
    lv_obj_align(g_pop_next_lbl, LV_ALIGN_TOP_MID, col, 240);
    lv_obj_align(g_pop_next,     LV_ALIGN_TOP_MID, col, 258);
    lv_label_set_text(g_pop_next_lbl, "next");
    if (next > now) {
        hhmm(at, sizeof at, next);
        lv_label_set_text(g_pop_next, at);
    } else {
        lv_label_set_text(g_pop_next, "none");
    }

    if (slot) {
        hhmm(at, sizeof at, slot);
        lv_label_set_text(g_pop_miss, at);
        lv_obj_align(g_pop_miss_lbl, LV_ALIGN_TOP_MID, -POP_COL_DX, 240);
        lv_obj_align(g_pop_miss,     LV_ALIGN_TOP_MID, -POP_COL_DX, 258);
        lv_obj_align(g_pop_miss_sub, LV_ALIGN_TOP_MID, -POP_COL_DX, 300);
        if (snoozed) {
            hhmm(at, sizeof at, snoozed);
            lv_label_set_text_fmt(g_pop_miss_sub, "snoozed to %s", at);
        } else {
            lv_label_set_text(g_pop_miss_sub, "");
        }
        lv_obj_remove_flag(g_pop_miss_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(g_pop_miss,     LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(g_pop_miss_sub, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(g_pop_rule,     LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_pop_miss_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_pop_miss,     LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_pop_miss_sub, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_pop_rule,     LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_show_popup(int action) {
    g_popup_action = action;
    popup_refresh();
    lv_screen_load_anim(g_popup, LV_SCR_LOAD_ANIM_FADE_IN, 150, 0, false);
}

static void on_tile_cb(lv_event_t *e) {
    ui_show_popup((int)(intptr_t)lv_event_get_user_data(e));
}

/** Done logs a tap action. A guided action (stretches, shoulder roll) has
 *  nothing to log until its timer has run, so its button reads Start and
 *  opens the flow instead — the flow's last step logs the done (§7.6). */
static void on_pop_done(lv_event_t *e) {
    LV_UNUSED(e);
    const int a = g_popup_action;
    if (a < 0) return;
    if (g_settings.actions[a].flow == FLOW_STRETCH) { ui_show_stretch(a); return; }
    input_done(a);
    ui_show_grid();
}
static void on_pop_close(lv_event_t *e)  { LV_UNUSED(e); ui_show_grid(); }
static void on_pop_history(lv_event_t *e) { LV_UNUSED(e); if (g_popup_action >= 0) ui_show_history(g_popup_action); }


// --------------------------------------------------------------- card hooks

static void on_row_toggle(lv_event_t *e) {
    card_toggle(&g_card, (int)(intptr_t)lv_event_get_user_data(e));
    ui_show_card();
}
static void on_confirm(lv_event_t *e) {
    LV_UNUSED(e);
    card_confirm(&g_card, &g_day, time(NULL), ui_card_host());
}
static void on_delay(lv_event_t *e)   { LV_UNUSED(e); card_delay_all(&g_card, &g_day, time(NULL), ui_card_host()); }

/** §7.3a: one X, built one way, everywhere it appears. */
static void x_button(lv_obj_t *parent, lv_event_cb_t on_close) {
    lv_obj_t *x = lv_button_create(parent);
    // 64px square flush in the corner, plus 12px of slop beyond its edges:
    // on a 1.8" panel a 44px target was missed more often than hit.
    lv_obj_set_size(x, 64, 64);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_ext_click_area(x, 12);
    lv_obj_set_style_bg_opa(x, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(x, 0, 0);
    lv_obj_t *l = lv_label_create(x);
    // LVGL's bundled Montserrat carries the LV_SYMBOL_* glyphs; the design's
    // Geist does not, so symbol labels always use the bundled font.
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_label_set_text(l, LV_SYMBOL_CLOSE);
    lv_obj_center(l);
    lv_obj_add_event_cb(x, on_close, LV_EVENT_CLICKED, NULL);
}

/** A flat rectangular button with no theme baggage: no border, no shadow. */
static lv_obj_t *flat_button(lv_obj_t *parent, int w, int h, int radius) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, radius, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    return b;
}

// -------------------------------------------------------------------- grid

static void build_grid(void) {
    g_grid = lv_obj_create(NULL);
    lv_obj_set_style_text_font(g_grid, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(g_grid, lv_color_hex(0x000000), 0);   // the seams
    lv_obj_clear_flag(g_grid, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < g_settings.n_actions; i++) {
        const int x = (i % 2) * (TILE_W + SEAM);
        const int y = (i / 2) * (TILE_H + SEAM);

        lv_obj_t *t = lv_obj_create(g_grid);
        lv_obj_set_size(t, TILE_W, TILE_H);
        lv_obj_set_pos(t, x, y);
        lv_obj_set_style_bg_color(t, body_of(i), 0);
        lv_obj_set_style_border_width(t, 0, 0);
        lv_obj_set_style_radius(t, 0, 0);
        lv_obj_set_style_pad_all(t, 0, 0);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(t, on_tile_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        g_tile[i] = t;

        // The wash is a flat block now — the deeper tint IS the pool; no
        // gradient. Its height is the countdown (see ui_refresh).
        lv_obj_t *wash = lv_obj_create(t);
        lv_obj_set_width(wash, LV_PCT(100));
        lv_obj_set_height(wash, 0);
        lv_obj_set_style_border_width(wash, 0, 0);
        lv_obj_set_style_radius(wash, 0, 0);
        lv_obj_set_style_pad_all(wash, 0, 0);
        lv_obj_set_style_bg_color(wash, wash_of(i), 0);
        lv_obj_set_style_bg_opa(wash, LV_OPA_COVER, 0);
        lv_obj_clear_flag(wash, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(wash, LV_ALIGN_BOTTOM_MID, 0, 0);
        g_wash[i] = wash;

        lv_obj_t *wave = lv_line_create(t);
        lv_obj_set_style_line_color(wave, lv_color_hex(TINT[i]), 0);
        lv_obj_set_style_line_width(wave, 2, 0);
        lv_obj_set_style_line_rounded(wave, true, 0);
        lv_obj_set_style_line_opa(wave, LV_OPA_90, 0);
        lv_obj_clear_flag(wave, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        g_wave[i] = wave;

        // The icon owns the tile: 64px, centred, drawn after the wash and
        // wave so the waterline passes behind it.
        lv_obj_t *icon = lv_image_create(t);
        lv_image_set_src(icon, ICON[i]);
        lv_obj_set_style_image_recolor(icon, lv_color_hex(TINT[i]), 0);
        lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
        lv_obj_align(icon, LV_ALIGN_CENTER, 0, -5);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);

        for (int d = 0; d < MAX_DOTS; d++) {
            lv_obj_t *dot = lv_obj_create(t);
            lv_obj_set_style_border_width(dot, 0, 0);
            lv_obj_set_style_pad_all(dot, 0, 0);
            lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
            g_dot[i][d] = dot;
        }
    }
}

// -------------------------------------------------------------------- card

static void build_card(void) {
    g_card_scr = lv_obj_create(NULL);
    lv_obj_set_style_text_font(g_card_scr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(g_card_scr, lv_color_hex(PAGE_BG), 0);
    lv_obj_clear_flag(g_card_scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(g_card_scr);
    lv_label_set_text(title, "Due now");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 22);

    // One row per action, not ACTIONS_MAX: the card can never hold more than
    // the schedule has. The whole row is the checkbox — filled tint block
    // means "will be logged", quiet block means "won't".
    for (int i = 0; i < g_settings.n_actions; i++) {
        lv_obj_t *row = lv_obj_create(g_card_scr);
        lv_obj_set_size(row, CONTENT_W, 62);
        lv_obj_set_pos(row, MARGIN, 72 + i * 72);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 16, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(row, on_row_toggle, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *icon = lv_image_create(row);
        lv_image_set_scale(icon, 160);                    // 64px art at 40px
        lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 6, 0);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *name = lv_label_create(row);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_16, 0);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 72, -10);

        lv_obj_t *sub = lv_label_create(row);
        lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
        lv_obj_align(sub, LV_ALIGN_LEFT_MID, 72, 10);

        lv_obj_t *mark = lv_obj_create(row);
        lv_obj_set_size(mark, 26, 26);
        lv_obj_set_style_radius(mark, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_all(mark, 0, 0);
        lv_obj_align(mark, LV_ALIGN_RIGHT_MID, -18, 0);
        lv_obj_clear_flag(mark, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *chk = lv_label_create(mark);
        lv_obj_set_style_text_font(chk, &lv_font_montserrat_14, 0);
        lv_label_set_text(chk, LV_SYMBOL_OK);
        lv_obj_center(chk);

        g_rows[i] = row;  g_row_icon[i] = icon;  g_row_name[i] = name;
        g_row_sub[i] = sub;  g_row_mark[i] = mark;  g_row_chk[i] = chk;
    }

    lv_obj_t *confirm = flat_button(g_card_scr, CONTENT_W, 66, 16);
    lv_obj_align(confirm, LV_ALIGN_BOTTOM_MID, 0, -88);
    lv_obj_set_style_bg_color(confirm, lv_color_hex(COL_TEXT), 0);
    // Nothing ticked, nothing to log: the button says so by going quiet.
    lv_obj_set_style_bg_opa(confirm, 31, LV_STATE_DISABLED);               // 12%
    lv_obj_set_style_bg_color(confirm, lv_color_hex(COL_TEXT), LV_STATE_DISABLED);
    g_confirm = confirm;
    g_confirm_label = lv_label_create(confirm);
    lv_obj_set_style_text_font(g_confirm_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(g_confirm_label, lv_color_hex(PAGE_BG), 0);
    lv_label_set_text(g_confirm_label, "Log 0 done");
    lv_obj_center(g_confirm_label);
    lv_obj_add_event_cb(confirm, on_confirm, LV_EVENT_CLICKED, NULL);

    lv_obj_t *delay = flat_button(g_card_scr, CONTENT_W, 56, 14);
    lv_obj_align(delay, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_bg_color(delay, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(delay, BTN_QUIET_OPA, 0);
    lv_obj_t *dl = lv_label_create(delay);
    lv_obj_set_style_text_color(dl, lv_color_hex(0xd9dee3), 0);
    lv_label_set_text(dl, "+15m for all");
    lv_obj_center(dl);
    lv_obj_add_event_cb(delay, on_delay, LV_EVENT_CLICKED, NULL);

    x_button(g_card_scr, on_delay);          // X = not now, same promise as +15m
}

// ----------------------------------------------------------------- history

static void on_hist_close(lv_event_t *e) { LV_UNUSED(e); if (g_hist_action >= 0) ui_show_popup(g_hist_action); }

/** The rebuild is deferred: this runs inside the Undo button's own click
 *  event, and lv_obj_clean() would delete that button while LVGL is still
 *  walking it — a use-after-free that surfaced as the whole UI freezing on
 *  the next tap. lv_async_call lands after the event has fully unwound. */
static void hist_rebuild_async(void *arg) {
    LV_UNUSED(arg);
    if (g_hist_action >= 0 && lv_screen_active() == g_hist) ui_show_history(g_hist_action);
}

static void on_hist_undo(lv_event_t *e) {
    const int k = (int)(intptr_t)lv_event_get_user_data(e);
    const int a = g_hist_action;
    if (a < 0 || k < 0 || k >= g_view.n_slots[a]) return;
    input_undo(a, g_view.slot_time[a][k]);      // reloads + rederives g_view
    lv_async_call(hist_rebuild_async, NULL);    // rebuild once the event is done
}

static void build_history(void) {
    g_hist = lv_obj_create(NULL);
    lv_obj_set_style_text_font(g_hist, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(g_hist, lv_color_hex(PAGE_BG), 0);
    lv_obj_clear_flag(g_hist, LV_OBJ_FLAG_SCROLLABLE);

    g_hist_icon = lv_image_create(g_hist);
    lv_image_set_scale(g_hist_icon, 144);                 // 64px art at 36px
    lv_obj_set_style_image_recolor_opa(g_hist_icon, LV_OPA_COVER, 0);
    lv_obj_align(g_hist_icon, LV_ALIGN_TOP_LEFT, 16, 20);

    g_hist_name = lv_label_create(g_hist);
    lv_obj_set_style_text_font(g_hist_name, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(g_hist_name, lv_color_hex(COL_TEXT), 0);
    lv_obj_align(g_hist_name, LV_ALIGN_TOP_LEFT, 64, 16);

    g_hist_sub = lv_label_create(g_hist);
    lv_obj_set_style_text_color(g_hist_sub, lv_color_hex(COL_STATE), 0);
    lv_obj_align(g_hist_sub, LV_ALIGN_TOP_LEFT, 64, 50);

    // The feed scrolls; the head does not.
    g_hist_list = lv_obj_create(g_hist);
    lv_obj_set_pos(g_hist_list, 0, 80);
    lv_obj_set_size(g_hist_list, PANEL_W, PANEL_H - 80);
    lv_obj_set_style_bg_opa(g_hist_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_hist_list, 0, 0);
    lv_obj_set_style_radius(g_hist_list, 0, 0);
    lv_obj_set_style_pad_all(g_hist_list, 0, 0);
    lv_obj_set_style_pad_left(g_hist_list, 16, 0);
    lv_obj_set_style_pad_right(g_hist_list, 16, 0);
    lv_obj_set_style_pad_bottom(g_hist_list, 16, 0);
    lv_obj_set_flex_flow(g_hist_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(g_hist_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_hist_list, LV_SCROLLBAR_MODE_OFF);

    x_button(g_hist, on_hist_close);
}

static lv_obj_t *hist_row(void) {
    lv_obj_t *row = lv_obj_create(g_hist_list);
    lv_obj_set_size(row, CONTENT_W, 52);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_border_opa(row, 15, 0);                 // 6%
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return row;
}

/** The mark is the tile dot's language at 22px: filled tint tick = done,
 *  grey cross = skipped, hollow grey = missed, hollow tint = open, and a
 *  small dark dot for upcoming. */
static void hist_mark(lv_obj_t *row, slot_state_t st, int a) {
    lv_obj_t *m = lv_obj_create(row);
    lv_obj_set_style_radius(m, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(m, 0, 0);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    if (st == SLOT_PEND) {
        lv_obj_set_size(m, 10, 10);
        lv_obj_set_style_border_width(m, 0, 0);
        lv_obj_set_style_bg_color(m, lv_color_hex(COL_PEND), 0);
        lv_obj_align(m, LV_ALIGN_RIGHT_MID, -6, 0);
        return;
    }
    lv_obj_set_size(m, 22, 22);
    lv_obj_align(m, LV_ALIGN_RIGHT_MID, 0, 0);
    if (st == SLOT_DONE || st == SLOT_SKIP) {
        lv_obj_set_style_border_width(m, 0, 0);
        lv_obj_set_style_bg_color(m, st == SLOT_DONE ? lv_color_hex(TINT[a]) : lv_color_hex(COL_SKIP), 0);
        lv_obj_t *g = lv_label_create(m);
        lv_obj_set_style_text_font(g, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(g, lv_color_hex(PAGE_BG), 0);
        lv_label_set_text(g, st == SLOT_DONE ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE);
        lv_obj_center(g);
    } else {
        lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(m, 2, 0);
        lv_obj_set_style_border_color(m, st == SLOT_DUE ? lv_color_hex(TINT[a]) : lv_color_hex(COL_TEXT), 0);
        lv_obj_set_style_border_opa(m, st == SLOT_DUE ? LV_OPA_COVER : 77, 0);   // 30%
    }
}

static void hist_slot_row(int a, int k, time_t slot, slot_state_t st, time_t ts) {
    lv_obj_t *row = hist_row();
    if (st == SLOT_PEND) lv_obj_set_style_opa(row, 102, 0); // upcoming: 40%

    char at[8], txt[40];
    hhmm(at, sizeof at, slot);
    lv_obj_t *t = lv_label_create(row);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(t, st == SLOT_DUE ? lv_color_hex(TINT[a])
                                 : st == SLOT_MISS ? lv_color_hex(0x7f8489) : lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(t, at);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);

    const time_t now = time(NULL);
    const time_t snoozed = st == SLOT_DUE && g_day.snoozed_until[a] > now ? g_day.snoozed_until[a] : 0;
    if (st == SLOT_DONE || st == SLOT_SKIP) {
        hhmm(at, sizeof at, ts);
        snprintf(txt, sizeof txt, "%s at %s", st == SLOT_DONE ? "done" : "skipped", at);
    } else if (snoozed) {
        hhmm(at, sizeof at, snoozed);
        snprintf(txt, sizeof txt, "open " LV_SYMBOL_BULLET " snoozed to %s", at);
    } else {
        snprintf(txt, sizeof txt, "%s", st == SLOT_DUE ? "open" : st == SLOT_MISS ? "missed" : "upcoming");
    }
    lv_obj_t *l = lv_label_create(row);
    lv_obj_set_style_text_color(l, lv_color_hex(0x7f8489), 0);   // 55%
    lv_label_set_text(l, txt);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 68, 0);

    if (st == SLOT_DONE || st == SLOT_SKIP) {
        lv_obj_t *u = flat_button(row, 68, 34, 10);
        lv_obj_align(u, LV_ALIGN_RIGHT_MID, -34, 0);
        lv_obj_set_style_bg_color(u, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_bg_opa(u, BTN_QUIET_OPA, 0);
        lv_obj_set_ext_click_area(u, 8);
        lv_obj_t *ul = lv_label_create(u);
        lv_obj_set_style_text_color(ul, lv_color_hex(0xd9dee3), 0);
        lv_label_set_text(ul, "Undo");
        lv_obj_center(ul);
        lv_obj_add_event_cb(u, on_hist_undo, LV_EVENT_CLICKED, (void *)(intptr_t)k);
    }
    hist_mark(row, st, a);
}

static void hist_now_rule(void) {
    lv_obj_t *row = lv_obj_create(g_hist_list);
    lv_obj_set_size(row, CONTENT_W, 28);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    char at[8], txt[16];
    hhmm(at, sizeof at, time(NULL));
    snprintf(txt, sizeof txt, "NOW %s", at);
    lv_obj_t *l = lv_label_create(row);
    lv_obj_set_style_text_color(l, lv_color_hex(0x8a9096), 0);   // 60%
    lv_label_set_text(l, txt);
    lv_obj_center(l);

    for (int side = 0; side < 2; side++) {
        lv_obj_t *line = lv_obj_create(row);
        lv_obj_set_size(line, 118, 1);
        lv_obj_set_style_border_width(line, 0, 0);
        lv_obj_set_style_radius(line, 0, 0);
        lv_obj_set_style_bg_color(line, lv_color_hex(COL_TEXT), 0);
        lv_obj_set_style_bg_opa(line, 46, 0);                 // 18%
        lv_obj_align(line, side ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, 0, 0);
    }
}

void ui_show_history(int a) {
    g_hist_action = a;
    const action_def_t *def = &g_settings.actions[a];

    lv_image_set_src(g_hist_icon, ICON[a]);
    lv_obj_set_style_image_recolor(g_hist_icon, lv_color_hex(TINT[a]), 0);
    lv_label_set_text(g_hist_name, def->name);
    lv_label_set_text_fmt(g_hist_sub, "%d of %d today", g_view.counts[a], def->target);

    lv_obj_clean(g_hist_list);
    for (int k = 0; k < g_view.n_slots[a]; k++) {
        hist_slot_row(a, k, g_view.slot_time[a][k], (slot_state_t)g_view.slots[a][k], g_view.slot_ts[a][k]);
    }
    hist_now_rule();
    // Upcoming: walk on from the next slot. Bounded like derive's walk.
    int shown = g_view.n_slots[a];
    for (time_t t = g_view.next[a]; t && shown < SLOTS_MAX; t = wfh_slot_after(def, t, &g_settings), shown++) {
        hist_slot_row(a, -1, t, SLOT_PEND, 0);
    }
    lv_obj_scroll_to_y(g_hist_list, 0, LV_ANIM_OFF);

    if (lv_screen_active() != g_hist) lv_screen_load_anim(g_hist, LV_SCR_LOAD_ANIM_FADE_IN, 150, 0, false);
}

// ------------------------------------------------------------------- popup

static void build_popup(void) {
    g_popup = lv_obj_create(NULL);
    lv_obj_set_style_text_font(g_popup, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(g_popup, lv_color_hex(PAGE_BG), 0);
    lv_obj_clear_flag(g_popup, LV_OBJ_FLAG_SCROLLABLE);

    g_pop_icon = lv_image_create(g_popup);
    lv_image_set_scale(g_pop_icon, 352);                  // 64px art at 88px
    lv_obj_set_style_image_recolor_opa(g_pop_icon, LV_OPA_COVER, 0);
    lv_obj_align(g_pop_icon, LV_ALIGN_TOP_MID, 0, 80);

    g_pop_name = lv_label_create(g_popup);
    lv_obj_set_style_text_font(g_pop_name, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(g_pop_name, lv_color_hex(COL_TEXT), 0);
    lv_obj_align(g_pop_name, LV_ALIGN_TOP_MID, 0, 168);

    g_pop_blurb = lv_label_create(g_popup);
    lv_obj_set_width(g_pop_blurb, 320);
    lv_label_set_long_mode(g_pop_blurb, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(g_pop_blurb, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(g_pop_blurb, lv_color_hex(COL_BLURB), 0);
    lv_obj_align(g_pop_blurb, LV_ALIGN_TOP_MID, 0, 210);

    // Two facts side by side: the slot let pass (muted, only when there
    // is one) and the next one coming (tinted), split by a hairline —
    // transcribed from design/popup-v4.html's .stats.
    g_pop_miss_lbl = lv_label_create(g_popup);
    lv_obj_set_style_text_color(g_pop_miss_lbl, lv_color_hex(COL_STATE), 0);
    lv_label_set_text(g_pop_miss_lbl, "missed");
    g_pop_miss = lv_label_create(g_popup);
    lv_obj_set_style_text_font(g_pop_miss, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(g_pop_miss, lv_color_hex(COL_BLURB), 0);
    g_pop_miss_sub = lv_label_create(g_popup);
    lv_obj_set_style_text_color(g_pop_miss_sub, lv_color_hex(COL_STATE), 0);

    g_pop_rule = lv_obj_create(g_popup);
    lv_obj_set_size(g_pop_rule, 1, 72);
    lv_obj_set_style_border_width(g_pop_rule, 0, 0);
    lv_obj_set_style_radius(g_pop_rule, 0, 0);
    lv_obj_set_style_bg_color(g_pop_rule, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_bg_opa(g_pop_rule, 31, 0);                     // 12%
    lv_obj_align(g_pop_rule, LV_ALIGN_TOP_MID, 0, 240);

    g_pop_next_lbl = lv_label_create(g_popup);
    lv_obj_set_style_text_color(g_pop_next_lbl, lv_color_hex(COL_STATE), 0);
    g_pop_next = lv_label_create(g_popup);
    lv_obj_set_style_text_font(g_pop_next, &lv_font_montserrat_36, 0);

    g_pop_done = flat_button(g_popup, CONTENT_W, 66, 16);
    lv_obj_align(g_pop_done, LV_ALIGN_BOTTOM_MID, 0, -24);
    g_pop_done_lbl = lv_label_create(g_pop_done);
    lv_obj_set_style_text_font(g_pop_done_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(g_pop_done_lbl, lv_color_hex(PAGE_BG), 0);
    lv_label_set_text(g_pop_done_lbl, "Done");
    lv_obj_center(g_pop_done_lbl);
    lv_obj_add_event_cb(g_pop_done, on_pop_done, LV_EVENT_CLICKED, NULL);

    // Top-left: this action's day (§7.7).
    lv_obj_t *hist = lv_button_create(g_popup);
    lv_obj_set_size(hist, 64, 64);
    lv_obj_align(hist, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_ext_click_area(hist, 12);
    lv_obj_set_style_bg_opa(hist, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(hist, 0, 0);
    lv_obj_t *hl = lv_label_create(hist);
    lv_obj_set_style_text_font(hl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(hl, lv_color_hex(COL_TEXT), 0);
    lv_label_set_text(hl, LV_SYMBOL_LIST);
    lv_obj_center(hl);
    lv_obj_add_event_cb(hist, on_pop_history, LV_EVENT_CLICKED, NULL);

    x_button(g_popup, on_pop_close);
}

// ------------------------------------------------------------- guided flow

static void flow_show_step(void) {
    const stretch_def_t *st = &g_flow_steps[g_flow_i];
    const int a = g_flow_action;

    for (int i = 0; i < FLOW_MAX_STEPS; i++) {
        if (i >= g_flow_n) { lv_obj_add_flag(g_flow_pill[i], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_clear_flag(g_flow_pill[i], LV_OBJ_FLAG_HIDDEN);
        if (i < g_flow_i) {                                   // past: faded tint
            lv_obj_set_style_bg_color(g_flow_pill[i], lv_color_hex(TINT[a]), 0);
            lv_obj_set_style_bg_opa(g_flow_pill[i], 115, 0);  // 45%
        } else if (i == g_flow_i) {                           // current: full tint
            lv_obj_set_style_bg_color(g_flow_pill[i], lv_color_hex(TINT[a]), 0);
            lv_obj_set_style_bg_opa(g_flow_pill[i], LV_OPA_COVER, 0);
        } else {                                              // future: faint white
            lv_obj_set_style_bg_color(g_flow_pill[i], lv_color_hex(0xffffff), 0);
            lv_obj_set_style_bg_opa(g_flow_pill[i], 36, 0);   // 14%
        }
    }

    lv_label_set_text(g_flow_name, st->name);
    lv_label_set_text(g_flow_cue, st->cue);
    lv_label_set_text_fmt(g_flow_secs, "%d", st->seconds);

    lv_arc_set_range(g_flow_arc, 0, st->seconds);
    lv_arc_set_value(g_flow_arc, st->seconds);

    // Every step waits for Start — you need a moment to get into position.
    // There is no Done: the timer finishing IS done, for steps and the set.
    g_flow_running = false;
    lv_obj_clear_flag(g_flow_pri, LV_OBJ_FLAG_HIDDEN);
}

static void flow_advance(void) {
    if (g_flow_i + 1 < g_flow_n) {
        g_flow_i++;
        feedback_play(CUE_READY);       // §9: step complete — single soft note
        flow_show_step();
    } else {
        // The timer running out on the last step logs the one done.
        const int a = g_flow_action;
        g_flow_action = -1;
        input_done(a);
        ui_show_grid();
    }
}

static void flow_tick(lv_timer_t *timer) {
    LV_UNUSED(timer);
    if (lv_screen_active() != g_flow || g_flow_action < 0 || !g_flow_running) return;

    const time_t remaining = g_flow_end - time(NULL);
    if (remaining <= 0) { flow_advance(); return; }
    lv_label_set_text_fmt(g_flow_secs, "%d", (int)remaining);
    lv_arc_set_value(g_flow_arc, (int32_t)remaining);
}

static void on_flow_start(lv_event_t *e) {
    LV_UNUSED(e);
    if (g_flow_running) return;
    g_flow_running = true;
    g_flow_end = time(NULL) + g_flow_steps[g_flow_i].seconds;
    lv_obj_add_flag(g_flow_pri, LV_OBJ_FLAG_HIDDEN);
}
static void on_flow_close(lv_event_t *e) {
    LV_UNUSED(e);
    // X is "not now", not a skip: nothing is logged, but without a snooze
    // card_sync would re-raise this screen on the very next tick.
    const int a = g_flow_action;
    g_flow_action = -1;
    if (a >= 0) g_day.snoozed_until[a] = time(NULL) + CARD_SNOOZE_S;
    ui_show_grid();
}

static void build_flow(void) {
    g_flow = lv_obj_create(NULL);
    lv_obj_set_style_text_font(g_flow, &lv_font_montserrat_14, 0);
    lv_obj_set_style_bg_color(g_flow, lv_color_hex(PAGE_BG), 0);
    lv_obj_clear_flag(g_flow, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < FLOW_MAX_STEPS; i++) {
        lv_obj_t *p = lv_obj_create(g_flow);
        lv_obj_set_size(p, 18, 4);
        lv_obj_set_pos(p, 16 + i * 23, 20);
        lv_obj_set_style_radius(p, 4, 0);
        lv_obj_set_style_border_width(p, 0, 0);
        lv_obj_set_style_pad_all(p, 0, 0);
        lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
        g_flow_pill[i] = p;
    }

    g_flow_arc = lv_arc_create(g_flow);
    lv_obj_set_size(g_flow_arc, 160, 160);
    lv_obj_align(g_flow_arc, LV_ALIGN_TOP_MID, 0, 36);
    lv_arc_set_rotation(g_flow_arc, 270);
    lv_arc_set_bg_angles(g_flow_arc, 0, 360);
    lv_obj_set_style_arc_width(g_flow_arc, 7, LV_PART_MAIN);
    lv_obj_set_style_arc_color(g_flow_arc, lv_color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(g_flow_arc, 26, LV_PART_MAIN);        // 10%
    lv_obj_set_style_arc_width(g_flow_arc, 7, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(g_flow_arc, true, LV_PART_INDICATOR);
    lv_obj_remove_style(g_flow_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(g_flow_arc, LV_OBJ_FLAG_CLICKABLE);

    g_flow_secs = lv_label_create(g_flow);
    lv_obj_set_style_text_font(g_flow_secs, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(g_flow_secs, lv_color_hex(COL_TEXT), 0);
    lv_obj_align(g_flow_secs, LV_ALIGN_TOP_MID, 0, 88);

    g_flow_name = lv_label_create(g_flow);
    lv_obj_set_style_text_font(g_flow_name, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(g_flow_name, lv_color_hex(COL_TEXT), 0);
    lv_obj_align(g_flow_name, LV_ALIGN_TOP_MID, 0, 218);

    g_flow_cue = lv_label_create(g_flow);
    lv_obj_set_width(g_flow_cue, 304);
    lv_label_set_long_mode(g_flow_cue, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(g_flow_cue, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(g_flow_cue, lv_color_hex(COL_BLURB), 0);
    lv_obj_align(g_flow_cue, LV_ALIGN_TOP_MID, 0, 252);

    g_flow_pri = flat_button(g_flow, CONTENT_W, 66, 16);
    lv_obj_align(g_flow_pri, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_t *pri_lbl = lv_label_create(g_flow_pri);
    lv_obj_set_style_text_font(pri_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(pri_lbl, lv_color_hex(PAGE_BG), 0);
    lv_label_set_text(pri_lbl, "Start");
    lv_obj_center(pri_lbl);
    lv_obj_add_event_cb(g_flow_pri, on_flow_start, LV_EVENT_CLICKED, NULL);

    // Start and the X are the only controls: mid-flow you are either doing
    // the thing or backing out, and the X snoozes (on_flow_close).
    x_button(g_flow, on_flow_close);
    lv_timer_create(flow_tick, 250, NULL);
}

void ui_show_stretch(int action) {
    // card_sync re-fires take_stretch every tick while the action is due;
    // re-entering would reset the timer mid-hold.
    if (lv_screen_active() == g_flow && g_flow_action == action) return;

    g_flow_action = action;
    if (g_settings.actions[action].flow == FLOW_STRETCH &&
        config_action_index(&g_settings, "stretch") == action) {
        g_flow_steps = config_stretches(&g_flow_n);
        if (g_flow_n > FLOW_MAX_STEPS) g_flow_n = FLOW_MAX_STEPS;
    } else {
        g_flow_single = (stretch_def_t){ .name = g_settings.actions[action].name,
                                         .seconds = FLOW_SINGLE_SECONDS,
                                         .cue = g_settings.actions[action].blurb };
        g_flow_steps = &g_flow_single;
        g_flow_n = 1;
    }
    g_flow_i = 0;

    lv_obj_set_style_bg_color(g_flow, body_of(action), 0);
    lv_obj_set_style_bg_color(g_flow_pri, lv_color_hex(TINT[action]), 0);
    lv_obj_set_style_arc_color(g_flow_arc, lv_color_hex(TINT[action]), LV_PART_INDICATOR);
    flow_show_step();

    lv_screen_load_anim(g_flow, LV_SCR_LOAD_ANIM_FADE_IN, 150, 0, false);
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
        lv_obj_set_style_bg_color(o, lv_color_hex(COL_SKIP), 0);
        lv_obj_set_style_border_width(o, 0, 0);
        break;
    case DOT_MISS:                                     // hollow ring
        lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(o, lv_color_hex(COL_SKIP), 0);
        lv_obj_set_style_border_width(o, 1, 0);
        break;
    default:                                           // pending: dark solid
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(o, lv_color_hex(COL_PEND), 0);
        lv_obj_set_style_border_width(o, 0, 0);
        break;
    }
}

static void dots_refresh(int a) {
    // One dot per slot the day actually has: the ones walked so far plus
    // the ones still to come, never fewer than the target. Sized from the
    // target alone, a cadence that outruns its target (water: 11 slots to a
    // target of 8) showed a full row with slots still ahead.
    const action_def_t *def = &g_settings.actions[a];
    int n = g_view.n_slots[a];
    for (time_t t = g_view.next[a]; t && n < MAX_DOTS; t = wfh_slot_after(def, t, &g_settings)) n++;
    if (n < def->target) n = def->target;
    if (n > MAX_DOTS) n = MAX_DOTS;

    if (n != g_dot_shown[a]) {                          // lay the row out once
        g_dot_shown[a] = n;
        // Measured from constants, not from lv_obj_get_content_width(): the
        // first refresh happens before LVGL has run layout, so the query
        // returns nothing useful, the pitch comes out garbage and the dots
        // pile on top of each other. Worse, the row is only re-laid-out when
        // the count changes, so it never corrects itself.
        const int32_t avail = TILE_W - 2 * DOT_INSET;
        int pitch = n > 0 ? (int)(avail / n) : 13;      // design: 9px + 4 gap
        if (pitch > 13) pitch = 13;
        const int size = (pitch - 4) < 6 ? 6 : (pitch - 4);
        const int x0 = (TILE_W - ((n - 1) * pitch + size)) / 2;   // row centred in the tile
        for (int d = 0; d < n; d++) {
            lv_obj_set_size(g_dot[a][d], size, size);
            lv_obj_set_style_radius(g_dot[a][d], size, 0);
            lv_obj_align(g_dot[a][d], LV_ALIGN_BOTTOM_LEFT, x0 + d * pitch, -DOT_INSET);
        }
        for (int d = n; d < MAX_DOTS; d++) dot_set(a, d, DOT_HIDDEN, 0);
    }

    // Slot order, left to right: the dot that fills is the slot that was
    // answered, and a slot let slide stays hollow in front of it. A due
    // slot — past, unanswered, window still open — is hollow as well: the
    // card and popup already call it missed, and it fills when answered.
    for (int d = 0; d < n; d++) {
        dot_state_t s = DOT_PEND;
        if (d < g_view.n_slots[a]) {
            switch (g_view.slots[a][d]) {
                case SLOT_DONE: s = DOT_DONE; break;
                case SLOT_SKIP: s = DOT_SKIP; break;
                case SLOT_MISS:
                case SLOT_DUE:  s = DOT_MISS; break;
                default:        s = DOT_PEND; break;
            }
        }
        dot_set(a, d, s, TINT[a]);
    }
}


void ui_refresh(void) {
    const time_t now = time(NULL);

    for (int i = 0; i < g_settings.n_actions; i++) {
        dots_refresh(i);

        // The wash is the tile's only countdown: its height says how close
        // the next prompt is. The time itself is on the popup.
        const time_t next = g_view.next[i];
        if (next > now) {
            // The wash is the countdown: how much of the approach has elapsed.
            // Interval actions fill across their whole cadence; fixed ones
            // across the final hour, since a wash creeping up over the four
            // hours before lunch would be imperceptible anyway.
            const time_t span = (g_settings.actions[i].cadence.kind == CADENCE_INTERVAL)
                                ? (time_t)g_settings.actions[i].cadence.every_min * 60
                                : 3600;
            time_t into = span - (next - now);
            if (into < 0) into = 0;
            lv_obj_set_height(g_wash[i], (int32_t)((int64_t)TILE_H * into / span));
        } else {
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
    if (g_card.len == 0) return;        // confirm/dismiss land on the grid

    int checked = 0;
    // Three rows fit at full height; a fuller card compresses them rather
    // than sliding row four under the Confirm button (found on-device: the
    // fourth row's top edge peeked out from behind "Log 0 done").
    const int row_h  = g_card.len <= 3 ? 62 : 46;
    const int pitch  = row_h + 10;
    const int iscale = g_card.len <= 3 ? 160 : 120;   // 40px / 30px icons
    for (int i = 0; i < g_settings.n_actions; i++) {
        if (i < g_card.len) {
            const int a = g_card.rows[i].action;
            const bool on = g_card.rows[i].checked;
            if (on) checked++;

            lv_obj_set_size(g_rows[i], CONTENT_W, row_h);
            lv_obj_set_pos(g_rows[i], MARGIN, 72 + i * pitch);
            lv_image_set_scale(g_row_icon[i], iscale);
            lv_obj_clear_flag(g_rows[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(g_rows[i], on ? lv_color_hex(TINT[a]) : body_of(a), 0);

            lv_image_set_src(g_row_icon[i], ICON[a]);
            lv_obj_set_style_image_recolor(g_row_icon[i],
                on ? lv_color_hex(PAGE_BG) : lv_color_hex(TINT[a]), 0);

            lv_label_set_text(g_row_name[i], g_settings.actions[a].name);
            lv_obj_set_style_text_color(g_row_name[i],
                lv_color_hex(on ? PAGE_BG : COL_TEXT), 0);

            // The slot the user let pass is the whole explanation of why
            // the board is asking.
            char at[8] = "";
            const time_t due = day_due_slot(a);
            if (due) hhmm(at, sizeof at, due);
            lv_label_set_text_fmt(g_row_sub[i], "missed %s", at);
            lv_obj_set_style_text_color(g_row_sub[i], lv_color_hex(on ? PAGE_BG : COL_TEXT), 0);
            lv_obj_set_style_text_opa(g_row_sub[i], on ? 166 : 128, 0);   // 65% / 50%

            if (on) {
                lv_obj_set_style_bg_color(g_row_mark[i], lv_color_hex(PAGE_BG), 0);
                lv_obj_set_style_bg_opa(g_row_mark[i], 46, 0);        // 18%
                lv_obj_set_style_border_width(g_row_mark[i], 0, 0);
                lv_obj_clear_flag(g_row_chk[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_text_color(g_row_chk[i], lv_color_hex(PAGE_BG), 0);
            } else {
                lv_obj_set_style_bg_opa(g_row_mark[i], LV_OPA_TRANSP, 0);
                lv_obj_set_style_border_width(g_row_mark[i], 2, 0);
                lv_obj_set_style_border_color(g_row_mark[i], lv_color_hex(TINT[a]), 0);
                lv_obj_set_style_border_opa(g_row_mark[i], 140, 0);   // 55%
                lv_obj_add_flag(g_row_chk[i], LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_obj_add_flag(g_rows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_label_set_text_fmt(g_confirm_label, "Log %d done", checked);
    if (checked) lv_obj_remove_state(g_confirm, LV_STATE_DISABLED);
    else         lv_obj_add_state(g_confirm, LV_STATE_DISABLED);
    lv_obj_set_style_text_color(g_confirm_label, lv_color_hex(checked ? PAGE_BG : COL_STATE), 0);

    if (lv_screen_active() != g_card_scr) {
        lv_screen_load_anim(g_card_scr, LV_SCR_LOAD_ANIM_FADE_IN, 150, 0, false);
    }
}

/** ~30fps, and only the waterline moves. */
static void wave_cb(lv_timer_t *timer) {
    LV_UNUSED(timer);
    if (lv_screen_active() != g_grid) return;      // nothing to animate off-grid

    g_wave_phase += 12;

    for (int i = 0; i < g_settings.n_actions; i++) {
        const int32_t h = lv_obj_get_height(g_wash[i]);
        if (h <= 1 || h >= TILE_H) { lv_obj_add_flag(g_wave[i], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_clear_flag(g_wave[i], LV_OBJ_FLAG_HIDDEN);

        const int32_t crest = TILE_H - h;
        for (int p = 0; p < WAVE_PTS; p++) {
            // Two sines at different rates so the crest never reads as a
            // repeating sawtooth; amplitude under 3px keeps it surface
            // tension rather than a wave machine.
            const float a = lv_trigo_sin((int16_t)(g_wave_phase + p * 6)) * 3.0f / 32767.0f;
            const float b = lv_trigo_sin((int16_t)(g_wave_phase * 2 + p * 10)) * 2.0f / 32767.0f;
            g_wave_pts[i][p].x = (lv_value_precise_t)TILE_W * p / (WAVE_PTS - 1);
            g_wave_pts[i][p].y = (lv_value_precise_t)crest + a + b;
        }
        lv_line_set_points(g_wave[i], g_wave_pts[i], WAVE_PTS);
    }
}

// ------------------------------------------------------------- card wiring

static void host_done(int a, void *ctx)    { LV_UNUSED(ctx); input_done(a); }
static void host_card(void *ctx)           { LV_UNUSED(ctx); ui_show_card(); }
static void host_grid(void *ctx)           { LV_UNUSED(ctx); ui_show_grid(); }
static void host_stretch(int a, void *ctx) { LV_UNUSED(ctx); ui_show_stretch(a); }

static const card_host_t HOST = {
    .log_done = host_done,
    .show_card = host_card, .show_grid = host_grid,
    .take_stretch = host_stretch, .ctx = NULL,
};

const card_host_t *ui_card_host(void) { return &HOST; }

void ui_build(void) {
    build_grid();
    build_card();
    build_popup();
    build_history();
    build_flow();
    ui_refresh();
    lv_timer_create(wave_cb, 33, NULL);
    lv_screen_load(g_grid);
}
