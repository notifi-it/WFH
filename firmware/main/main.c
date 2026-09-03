// WFH health tracker. Boot, then a 1 Hz tick that asks derive what is due
// and hands it to the card (§5.2). Nothing here writes the log — that is
// day_apply_event's job, reached only through input_done / input_undo.
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_io_expander.h"
#include "bsp/touch.h"
#include "esp_lvgl_port.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "audio.h"
#include "card.h"
#include "config.h"
#include "day.h"
#include "derive.h"
#include "feedback.h"
#include "store.h"
#include "ui.h"
#include "wifi_time.h"

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
    const uint32_t stride_b = snap->header.stride;

    // One framed line per row, so a byte lost in transit costs that row and
    // nothing after it — the host resyncs on the next "R<y>:" frame. The
    // whole-frame stream this replaces smeared every row past the first drop.
    char *line = malloc((stride_b + 2) / 3 * 4 + 1);
    if (!line) { ESP_LOGE(TAG, "no heap for dump line"); free(mem); return; }

    printf("\n<<<FB %d %d %d>>>\n", (int)snap->header.w, (int)snap->header.h,
           (int)stride_b);
    for (int32_t y = 0; y < snap->header.h; y++) {
        const uint8_t *p = snap->data + (size_t)y * stride_b;
        char *q = line;
        for (uint32_t i = 0; i < stride_b; i += 3) {
            const uint32_t v = (uint32_t)p[i] << 16 |
                               (uint32_t)(i + 1 < stride_b ? p[i+1] : 0) << 8 |
                               (uint32_t)(i + 2 < stride_b ? p[i+2] : 0);
            *q++ = B64[v >> 18 & 63];
            *q++ = B64[v >> 12 & 63];
            *q++ = i + 1 < stride_b ? B64[v >> 6 & 63] : '=';
            *q++ = i + 2 < stride_b ? B64[v & 63] : '=';
        }
        *q = '\0';
        printf("R%d:%s\n", (int)y, line);
    }
    printf("<<<END>>>\n");
    fflush(stdout);
    free(line);
    free(mem);
}

/** Dump on demand — 'd' over the console — instead of once after boot.
 *  The old boot-time dump meant every capture began with a reset, which
 *  destroyed exactly the transient states worth capturing. */
static void dump_task(void *arg) {
    LV_UNUSED(arg);
    for (;;) {
        const int c = getchar();
        if (c == 'd') dump_screen();
        else vTaskDelay(pdMS_TO_TICKS(100));
    }
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

/** Display bring-up, done here instead of bsp_display_start() for two
 *  reasons that both corrupt the panel silently:
 *
 *  1. The BSP registers this SPI panel with the LVGL port as an *RGB*
 *     display. For RGB panels the port reports the buffer free the moment
 *     the transfer is queued, which is right for a memory-mapped
 *     framebuffer and wrong for a DMA still streaming from the buffer: LVGL
 *     renders the next chunk over the top and the panel receives a mix.
 *     Registered as a plain SPI display, the port frees the buffer from
 *     the driver's transfer-done callback instead.
 *  2. The BSP allocates the draw buffer with default caps, which at >16KB
 *     means PSRAM. The SPI driver cannot DMA from PSRAM and bounces every
 *     ~32KB chunk through a temporary internal buffer; once WiFi has taken
 *     its share of internal RAM those bounces fail intermittently
 *     ("setup_dma_priv_buffer: Failed to allocate priv TX buffer"), the
 *     chunk is dropped and the panel keeps its old rows — two frames
 *     interleaved in ~44-row bands, with LVGL's own snapshot clean.
 *     A DMA-capable internal buffer needs no bounce and no allocation.
 *
 *  draw_buffer_reserve() takes that internal RAM before WiFi initialises;
 *  display_start() installs it as LVGL's buffer over the port's own token
 *  one, so the buffer always has room regardless of what the radio left
 *  behind. */
#define DRAW_BUF_PX    (BSP_LCD_H_RES * CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT)
#define DRAW_BUF_BYTES (DRAW_BUF_PX * 2)
static void *g_draw_buf_hold;

static void draw_buffer_reserve(void) {
    g_draw_buf_hold = heap_caps_aligned_alloc(64, DRAW_BUF_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!g_draw_buf_hold) ESP_LOGE(TAG, "no internal DMA RAM for a %u-byte draw buffer", DRAW_BUF_BYTES);
}

static lv_display_t *display_start(void) {
    esp_lcd_panel_handle_t    panel = NULL;
    esp_lcd_panel_io_handle_t io    = NULL;
    const bsp_display_config_t dc = { .max_transfer_sz = DRAW_BUF_BYTES };
    if (bsp_display_new(&dc, &panel, &io) != ESP_OK) return NULL;

    const lvgl_port_cfg_t pc = ESP_LVGL_PORT_INIT_CONFIG();
    if (lvgl_port_init(&pc) != ESP_OK) return NULL;

    // The port allocates a token buffer of its own; the reserved one is
    // installed over it below. Freeing the reservation for the port to
    // re-take was tried and is not reliable: the port allocates its context
    // struct first, and that can land in the hole.
    const lvgl_port_display_cfg_t dcfg = {
        .io_handle     = io,
        .panel_handle  = panel,
        .buffer_size   = BSP_LCD_H_RES * 2,
        .double_buffer = false,
        .hres          = BSP_LCD_H_RES,
        .vres          = BSP_LCD_V_RES,
        .color_format  = LV_COLOR_FORMAT_RGB565,
        .flags = { .buff_dma = true, .swap_bytes = true },
    };
    lv_display_t *disp = lvgl_port_add_disp(&dcfg);
    if (!disp) return NULL;
    if (g_draw_buf_hold) {
        lv_display_set_buffers(disp, g_draw_buf_hold, NULL, DRAW_BUF_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);
    } else {
        ESP_LOGE(TAG, "rendering through the port's %u-byte token buffer — expect a slow, tearing panel",
                 (unsigned)(BSP_LCD_H_RES * 2 * 2));
    }

    esp_lcd_touch_handle_t tp = NULL;
    if (bsp_touch_new(NULL, &tp) != ESP_OK) return NULL;
    const lvgl_port_touch_cfg_t tc = { .disp = disp, .handle = tp };
    if (!lvgl_port_add_touch(&tc)) return NULL;

    const lv_draw_buf_t *db = lv_display_get_buf_active(disp);
    ESP_LOGI(TAG, "draw buf %p (%s), %u bytes; free internal %u",
             db ? (void *)db->data : NULL,
             db && esp_ptr_external_ram(db->data) ? "PSRAM" : "internal DMA",
             db ? (unsigned)db->data_size : 0,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return disp;
}

/** Pre-NTP seed: SNTP (wifi_time.c) needs the network up before it can fix
 *  the clock, and a board with a cold RTC would meanwhile write events under
 *  a 1970 day key. Seed from the build clock and say so loudly — a wrong
 *  date is the one failure here that corrupts data (§12). */
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
        now = time(NULL);
        localtime_r(&now, &tm);
    }

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
            store_prune(400);                   // retention runs at rollover, once a day
            ESP_LOGI(TAG, "day is now %s", day_key);
        } else {
            wfh_derive(&g_day, now, &g_settings, &g_view);
        }

        // The LVGL task is not on the task watchdog, so a hang inside it
        // (it holds the display mutex forever) would otherwise freeze the
        // board until someone unplugs it. Fifteen seconds without the lock
        // is not contention, it is that; reboot into a working board.
        static int lock_misses;
        if (!bsp_display_lock(50)) {
            if (++lock_misses >= 15) {
                ESP_LOGE(TAG, "display lock unavailable for %ds — LVGL task is stuck, restarting", lock_misses);
                esp_restart();
            }
        } else {
            lock_misses = 0;

            // Out of hours — from the last window closing (work end + grace)
            // to work start — the grid gives way to the tally (§7.8). Acted
            // on at the transition only, so a tap that peeks at the grid is
            // not undone a second later.
            struct tm tm;
            localtime_r(&now, &tm);
            const bool off = now >= wfh_at_time(&tm, g_settings.work_end) + g_settings.grace_min * 60
                          || now <  wfh_at_time(&tm, g_settings.work_start);
            static int off_was = -1;
            if (off != off_was) {
                off_was = off;
                if (off) ui_show_dayend(); else ui_show_grid();
            }

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
    draw_buffer_reserve();                  // before WiFi: it takes what is left

    g_settings = *config_default();
    clock_bootstrap();

    // Clock before panel, always in this order: the WiFi radio and the QSPI
    // panel corrupt each other on the shared memory bus, so the radio runs
    // to completion — and is stopped — while the panel is still unpowered.
    // Costs a few dark seconds at boot; the alternative was a
    // panel that tore into white bands every time the radio keyed up.
    wifi_time_sync(g_settings.wifi_ssid, g_settings.wifi_pass, 60);

    ESP_ERROR_CHECK(panel_power_up());

    lv_display_t *disp = display_start();
    if (!disp) { ESP_LOGE(TAG, "display start failed"); return; }
    ESP_ERROR_CHECK(bsp_display_brightness_set(85));

    ESP_ERROR_CHECK(store_open());
    day_reload(time(NULL));

    if (bsp_display_lock(0)) { ui_build(); bsp_display_unlock(); }

    if (audio_init(70) != ESP_OK) ESP_LOGE(TAG, "audio init failed — running silent");

    ESP_LOGI(TAG, "%d actions, %d events today, %d due now",
             g_settings.n_actions, g_day.events_len, g_view.n_due);

    xTaskCreate(tick_task, "tick", 6144, NULL, 5, NULL);

#ifdef CONFIG_WFH_DUMP_FRAMEBUFFER
    xTaskCreate(dump_task, "dump", 8192, NULL, 3, NULL);
#endif
}
