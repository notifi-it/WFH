// Spikes S1 + S3 (§15.3): panel, touch, PMIC rails, and the sound path.
//
// Deliberately not lv_demo_widgets(): this draws the §7 layout at real size
// on the real panel, so what gets judged is whether the design reads at
// 368x448 — the thing the design/ loop can only approximate. Tapping a tile
// fires the §9 cue that tile would fire in the product.
#include <stdio.h>

#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_io_expander.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "audio.h"

static const char *TAG = "s1";

#define V2_TOUCH_ADDR 0x15        // CST820; absent on a V1 (FT3168) board

static lv_obj_t *g_readout;
static int       g_taps;

typedef struct { const char *name; uint32_t tint; const char *dots; } tile_t;

static const tile_t TILES[] = {
    { "Stand break",   0x7fd4a8, "4 of 10" },
    { "Water",         0x6ec3e0, "3 of 8"  },
    { "Shoulder roll", 0xb6a3e8, "2 of 8"  },
    { "Snack",         0xe8b06a, "1 of 2"  },
    { "Lunch",         0xe8926a, "0 of 1"  },
    { "Stretches",     0xe0d16a, "1 of 2"  },
};

static void on_tile(lv_event_t *e) {
    const tile_t *t = lv_event_get_user_data(e);

    lv_indev_t *indev = lv_indev_active();
    lv_point_t p = { 0, 0 };
    if (indev) lv_indev_get_point(indev, &p);

    g_taps++;
    ESP_LOGI(TAG, "tap #%d on '%s' at (%d, %d)", g_taps, t->name, (int)p.x, (int)p.y);
    lv_label_set_text_fmt(g_readout, "%s  (%d,%d)  #%d", t->name, (int)p.x, (int)p.y, g_taps);

    audio_cue(g_taps % 2 ? CUE_SUCCESS : CUE_BLOOM);
}

static void build_screen(void) {
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0d1117), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Header — 80px, per §7.
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, 368, 80);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0x161b22), 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *clock = lv_label_create(hdr);
    lv_label_set_text(clock, "14:32");
    lv_obj_set_style_text_font(clock, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(clock, lv_color_hex(0xe6edf3), 0);
    lv_obj_align(clock, LV_ALIGN_LEFT_MID, 4, -8);

    lv_obj_t *date = lv_label_create(hdr);
    lv_label_set_text(date, "Thu 13 Aug");
    lv_obj_set_style_text_color(date, lv_color_hex(0x8b949e), 0);
    lv_obj_align(date, LV_ALIGN_LEFT_MID, 4, 14);

    lv_obj_t *snd = lv_label_create(hdr);
    lv_label_set_text(snd, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_font(snd, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(snd, lv_color_hex(0xe6edf3), 0);
    lv_obj_align(snd, LV_ALIGN_RIGHT_MID, -12, 0);

    // 2-column grid in the 368x368 square below the header.
    for (int i = 0; i < (int)(sizeof TILES / sizeof TILES[0]); i++) {
        lv_obj_t *tile = lv_obj_create(scr);
        lv_obj_set_size(tile, 172, 104);
        lv_obj_set_pos(tile, (i % 2) * 184 + 6, 88 + (i / 2) * 112);
        lv_obj_set_style_bg_color(tile, lv_color_hex(0x161b22), 0);
        lv_obj_set_style_border_color(tile, lv_color_hex(TILES[i].tint), 0);
        lv_obj_set_style_border_width(tile, 2, 0);
        lv_obj_set_style_radius(tile, 14, 0);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(tile, on_tile, LV_EVENT_CLICKED, (void *)&TILES[i]);

        lv_obj_t *name = lv_label_create(tile);
        lv_label_set_text(name, TILES[i].name);
        lv_obj_set_style_text_color(name, lv_color_hex(TILES[i].tint), 0);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);

        lv_obj_t *dots = lv_label_create(tile);
        lv_label_set_text(dots, TILES[i].dots);
        lv_obj_set_style_text_color(dots, lv_color_hex(0x8b949e), 0);
        lv_obj_align(dots, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }

    g_readout = lv_label_create(scr);
    lv_label_set_text(g_readout, "tap a tile");
    lv_obj_set_style_text_color(g_readout, lv_color_hex(0x8b949e), 0);
    lv_obj_align(g_readout, LV_ALIGN_BOTTOM_MID, 0, -6);
}

/** Who is actually on the bus. Worth printing before anything depends on it:
 *  "touch not found" and "touch held in reset" produce identical errors from
 *  the driver, and only a scan tells them apart. */
static void i2c_scan(const char *when) {
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    printf("  i2c scan (%s):", when);
    for (uint8_t a = 0x08; a < 0x78; a++) {
        if (i2c_master_probe(bsp_i2c_get_handle(), a, 50) == ESP_OK) printf(" 0x%02X", a);
    }
    printf("\n");
    esp_log_level_set("i2c.master", ESP_LOG_INFO);
}

void app_main(void) {
    printf("\n=== S1/S3: panel, touch, sound ===\n");

    ESP_ERROR_CHECK(bsp_i2c_init());
    i2c_scan("before expander");

    // TP_RESET hangs off the TCA9554 (EXIO2 on the schematic), so the touch
    // controller is held in reset until something drives it high. The BSP
    // does not do this before probing, which is why FT5x06 init failed.
    esp_io_expander_handle_t exp = bsp_io_expander_init();
    if (exp) {
        esp_io_expander_set_dir(exp, IO_EXPANDER_PIN_NUM_2, IO_EXPANDER_OUTPUT);
        esp_io_expander_set_level(exp, IO_EXPANDER_PIN_NUM_2, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        esp_io_expander_set_level(exp, IO_EXPANDER_PIN_NUM_2, 1);
        vTaskDelay(pdMS_TO_TICKS(120));            // FT3168 needs time to come up
        i2c_scan("after touch reset");
    } else {
        ESP_LOGE(TAG, "io expander init failed");
    }

    lv_display_t *disp = bsp_display_start();
    if (!disp) { ESP_LOGE(TAG, "display start failed — PMIC rails or panel driver"); return; }
    ESP_ERROR_CHECK(bsp_display_brightness_set(85));
    ESP_LOGI(TAG, "panel up: %dx%d", (int)lv_display_get_horizontal_resolution(disp),
             (int)lv_display_get_vertical_resolution(disp));

    // Independent confirmation of the revision: V2 answers on the CST820
    // address, V1 does not. Cross-checks the strings read from the factory
    // firmware, using a completely different signal.
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    const bool v2 = i2c_master_probe(bsp_i2c_get_handle(), V2_TOUCH_ADDR, 100) == ESP_OK;
    esp_log_level_set("i2c.master", ESP_LOG_INFO);
    ESP_LOGI(TAG, "touch probe says board is %s", v2 ? "V2 (CO5300/CST820)" : "V1 (SH8601/FT3168)");

    if (bsp_display_lock(0)) { build_screen(); bsp_display_unlock(); }

    if (audio_init(70) == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(300));
        ESP_LOGI(TAG, "playing bloom (prompt), success (logged), ready (stretch step)");
        audio_cue(CUE_BLOOM);   vTaskDelay(pdMS_TO_TICKS(500));
        audio_cue(CUE_SUCCESS); vTaskDelay(pdMS_TO_TICKS(500));
        audio_cue(CUE_READY);
    } else {
        ESP_LOGE(TAG, "audio init failed");
    }

    printf("=== ready: tap tiles, each logs coords and plays a cue ===\n");
    for (;;) vTaskDelay(pdMS_TO_TICKS(5000));
}
