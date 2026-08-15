// WFH health tracker. Boot, then a 1 Hz tick that asks derive what is due
// and hands it to the card (§5.2). Nothing here writes the log — that is
// day_apply_event's job, reached only through input_done / input_skip.
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_io_expander.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "audio.h"
#include "card.h"
#include "config.h"
#include "day.h"
#include "derive.h"
#include "feedback.h"
#include "input.h"
#include "store.h"
#include "ui.h"



static const char *TAG = "wfh";

#define ESCALATE_AFTER_S 120        // §9: re-cue once, louder, then stay quiet

// Framebuffer dump. A display can be wrong in two very different places —
// LVGL drew it wrong, or the panel received it wrong — and from the outside
// both look identical. Snapshotting what LVGL rendered splits those cases
// without anyone photographing a screen.
#ifdef CONFIG_WFH_DUMP_FRAMEBUFFER
static void dump_screen(void) {
    // LVGL's own pool is far smaller than a frame, so the buffer comes from
    // PSRAM and lv_snapshot renders into it rather than allocating its own.
    const int32_t w = lv_display_get_horizontal_resolution(NULL);
    const int32_t h = lv_display_get_vertical_resolution(NULL);
    const uint32_t stride = lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_RGB565);
    uint8_t *mem = heap_caps_malloc(stride * h, MALLOC_CAP_SPIRAM);
    if (!mem) { ESP_LOGE(TAG, "no PSRAM for snapshot"); return; }

    // Pre-fill with magenta: anything LVGL does not paint stays magenta, so
    // "the renderer left this alone" and "the renderer drew something odd"
    // stop looking the same in the capture.
    for (size_t i = 0; i < (size_t)stride * h; i += 2) { mem[i] = 0x1F; mem[i+1] = 0xF8; }

    lv_draw_buf_t buf;
    lv_draw_buf_init(&buf, w, h, LV_COLOR_FORMAT_RGB565, stride, mem, stride * h);

    lv_result_t ok = LV_RESULT_INVALID;
    if (bsp_display_lock(2000)) {
        ok = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &buf);
        bsp_display_unlock();
    }
    if (ok != LV_RESULT_OK) { ESP_LOGE(TAG, "snapshot failed"); free(mem); return; }
    lv_draw_buf_t *snap = &buf;

    static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const uint8_t *p = snap->data;
    const size_t n = (size_t)snap->header.stride * snap->header.h;

    printf("\n<<<FB %d %d %d>>>\n", (int)snap->header.w, (int)snap->header.h,
           (int)snap->header.stride);
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t)p[i] << 16 |
                           (uint32_t)(i + 1 < n ? p[i+1] : 0) << 8 |
                           (uint32_t)(i + 2 < n ? p[i+2] : 0);
        char q[4] = { B64[v >> 18 & 63], B64[v >> 12 & 63],
                      i + 1 < n ? B64[v >> 6 & 63] : '=', i + 2 < n ? B64[v & 63] : '=' };
        fwrite(q, 1, 4, stdout);
        if ((i / 3) % 16 == 15) putchar('\n');
    }
    printf("\n<<<END>>>\n");
    fflush(stdout);
    free(mem);
}
#endif

/** The V1 BSP creates the I/O expander and never drives it, so the panel has
 *  no VCI and both controllers sit in reset. Nothing reports this — every SPI
 *  write into an unpowered panel returns ESP_OK. See §15.3.
 *    EXIO0 = LCD_RESET, EXIO1 = DSI_PWR_EN, EXIO2 = TP_RESET */
static esp_err_t panel_power_up(void) {
    esp_io_expander_handle_t exp = bsp_io_expander_init();
    if (!exp) return ESP_FAIL;

    const uint32_t pins = IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_1 | IO_EXPANDER_PIN_NUM_2;
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(exp, pins, IO_EXPANDER_OUTPUT), TAG, "expander dir");

    esp_io_expander_set_level(exp, IO_EXPANDER_PIN_NUM_1, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_io_expander_set_level(exp, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_2, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_io_expander_set_level(exp, IO_EXPANDER_PIN_NUM_0 | IO_EXPANDER_PIN_NUM_2, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    return ESP_OK;
}

/** No NTP yet (§11 step 10). Until then a board with a cold RTC would write
 *  events under a 1970 day key, so seed it from the build clock and say so
 *  loudly — a wrong date is the one failure here that corrupts data (§12). */
static void clock_bootstrap(void) {
    setenv("TZ", g_settings.tz, 1);
    tzset();

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    if (tm.tm_year < 124) {                     // before 2024 => never set
        struct tm built = { 0 };
        strptime(__DATE__ " " __TIME__, "%b %d %Y %H:%M:%S", &built);
        built.tm_isdst = -1;
        const struct timeval tv = { .tv_sec = mktime(&built) };
        settimeofday(&tv, NULL);
        ESP_LOGW(TAG, "RTC was unset — seeded from build time. Dates are approximate until NTP.");
    }

    now = time(NULL);
    localtime_r(&now, &tm);
    char buf[40];
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S %Z", &tm);
    ESP_LOGI(TAG, "clock: %s", buf);
}

static void tick_task(void *arg) {
    LV_UNUSED(arg);
    char day_key[11] = { 0 };

    for (;;) {
        const time_t now = time(NULL);

        // Rollover is handled here rather than on boot: the board runs for
        // weeks, so midnight is the common case, not an edge case (§11.2).
        const char *today = store_day_key(now);
        if (strcmp(today, day_key) != 0) {
            strncpy(day_key, today, sizeof day_key - 1);
            day_reload(now);
            ESP_LOGI(TAG, "day is now %s", day_key);
        } else {
            wfh_derive(&g_day, now, &g_settings, &g_view);
        }

        if (bsp_display_lock(50)) {
            const int was = g_card.len;
            card_sync(&g_card, g_view.due, g_view.n_due, &g_settings, now, ui_card_host());

            if (g_card.len > 0 && was == 0) feedback_play(CUE_BLOOM);

            // §9: escalate once rather than nagging in a loop.
            if (g_card.len > 0 && !g_card.escalated && now - g_card.raised_at >= ESCALATE_AFTER_S) {
                g_card.escalated = true;
                feedback_play(CUE_BLOOM);
            }

            ui_refresh();
            bsp_display_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main(void) {
    printf("\n=== WFH tracker ===\n");

    ESP_ERROR_CHECK(bsp_i2c_init());
    ESP_ERROR_CHECK(panel_power_up());

    lv_display_t *disp = bsp_display_start();
    if (!disp) { ESP_LOGE(TAG, "display start failed"); return; }
    ESP_ERROR_CHECK(bsp_display_brightness_set(85));

    g_settings = *config_default();
    clock_bootstrap();

    ESP_ERROR_CHECK(store_open());
    day_reload(time(NULL));

    if (bsp_display_lock(0)) { ui_build(); bsp_display_unlock(); }

    if (audio_init(70) != ESP_OK) ESP_LOGE(TAG, "audio init failed — running silent");

    ESP_LOGI(TAG, "%d actions, %d events today, %d due now",
             g_settings.n_actions, g_day.events_len, g_view.n_due);

    xTaskCreate(tick_task, "tick", 6144, NULL, 5, NULL);

#ifdef CONFIG_WFH_DUMP_FRAMEBUFFER
    vTaskDelay(pdMS_TO_TICKS(2500));
    dump_screen();
#endif
}
