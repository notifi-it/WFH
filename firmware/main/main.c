// Spike S2 (§15.3): does SQLite on LittleFS hold up at a full retention
// window? R1 is the one risk that could force a design change, so it gets
// measured on real hardware before the build order commits to §3.4.
//
// Pass: p99 insert < 50ms with synchronous=FULL at 15,000 rows.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "derive.h"
#include "store.h"

#define TOTAL_EVENTS    15000
#define EVENTS_PER_DAY     30     // §3.4's own estimate

static int cmp_u32(const void *a, const void *b) {
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static void report(const char *label, uint32_t *us, int n) {
    qsort(us, n, sizeof *us, cmp_u32);
    uint64_t sum = 0;
    for (int i = 0; i < n; i++) sum += us[i];
    printf("  %-12s n=%5d  mean %6.1fms  p50 %6.1fms  p99 %6.1fms  max %7.1fms\n",
           label, n, (double)sum / n / 1000.0, us[n / 2] / 1000.0,
           us[(int)(n * 0.99)] / 1000.0, us[n - 1] / 1000.0);
}

void app_main(void) {
    printf("\n=== S2: storage under load ===\n");

    if (store_open() != ESP_OK) { printf("store_open failed\n"); return; }
    // Evidence for the stack sizing, not decoration: mounting LittleFS is the
    // deepest thing this firmware does, and getting it wrong crashes inside
    // the flash driver where nothing points back here.
    printf("stack headroom after mount: %u bytes\n",
           (unsigned)uxTaskGetStackHighWaterMark(NULL));
    printf("journal_mode = %s   (WAL needs EXCLUSIVE locking on this VFS)\n", store_journal_mode());
    printf("starting row count: %d\n\n", store_count_events());

    const settings_t *s = config_default();
    uint32_t *lat = malloc(TOTAL_EVENTS * sizeof(uint32_t));
    if (!lat) { printf("no memory for timings\n"); return; }

    // Walk backwards from today so `day` spans a realistic retention window
    // rather than piling every row into one key.
    const time_t base = time(NULL) - (time_t)(TOTAL_EVENTS / EVENTS_PER_DAY) * 86400;

    for (int i = 0; i < TOTAL_EVENTS; i++) {
        log_event_t ev = {
            .action = i % s->n_actions,
            .kind   = (i % 9 == 0) ? KIND_SKIP : KIND_DONE,
            .ts     = base + (time_t)(i / EVENTS_PER_DAY) * 86400 + (i % EVENTS_PER_DAY) * 900,
            .slot   = base + (time_t)(i / EVENTS_PER_DAY) * 86400 + (i % EVENTS_PER_DAY) * 900,
        };

        const int64_t t0 = esp_timer_get_time();
        store_add_event(&ev, s);
        lat[i] = (uint32_t)(esp_timer_get_time() - t0);

        if ((i + 1) % 100 == 0) vTaskDelay(1);      // let idle run; no WDT trip

        if ((i + 1) % 1000 == 0) {
            static uint32_t window[1000];           // static: 4KB is too much stack
            memcpy(window, &lat[i - 999], sizeof window);
            char label[16];
            snprintf(label, sizeof label, "@%dk rows", (i + 1) / 1000);
            report(label, window, 1000);
            vTaskDelay(1);                     // let the idle task run; no WDT trips
        }
    }

    printf("\n  --- whole run ---\n");
    report("all inserts", lat, TOTAL_EVENTS);

    // The two other things storage does on the hot path.
    day_log_t day;
    const time_t mid = base + (time_t)(TOTAL_EVENTS / EVENTS_PER_DAY / 2) * 86400;
    int64_t t0 = esp_timer_get_time();
    store_load_day(store_day_key(mid), &day, s);
    printf("\n  load_day     %d events in %.1fms\n", day.events_len,
           (esp_timer_get_time() - t0) / 1000.0);

    day_view_t v;
    t0 = esp_timer_get_time();
    wfh_derive(&day, mid + 43200, s, &v);
    printf("  derive       %.2fms  (runs every tick)\n", (esp_timer_get_time() - t0) / 1000.0);

    t0 = esp_timer_get_time();
    store_prune(400);
    printf("  prune 400d   %.1fms\n", (esp_timer_get_time() - t0) / 1000.0);

    printf("\n  rows now: %d\n", store_count_events());
    store_close();
    printf("\n=== done ===\n");

    free(lat);
    for (;;) vTaskDelay(pdMS_TO_TICKS(10000));
}
