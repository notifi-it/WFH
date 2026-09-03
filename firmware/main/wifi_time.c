// WiFi exists on this board for exactly one datagram's worth of purpose:
// SNTP, so events are logged under the real date (§12: a wrong date is the
// one failure that corrupts data). No server, no client, no cloud.
//
// Runs before the panel powers up and returns with the radio fully off —
// the radio and the QSPI panel corrupt each other on the shared memory bus,
// so the two are never alive at the same time. See wifi_time.h.
#include "wifi_time.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_time";

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)data;
    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        // Retry inside the sync window; the window's own timeout bounds it.
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
    }
}

static void on_sync(struct timeval *tv) {
    (void)tv;
    ESP_LOGI(TAG, "SNTP synced — clock is now real");
}

bool wifi_time_sync(const char *ssid, const char *pass, int timeout_s) {
    if (!ssid || !ssid[0]) {
        ESP_LOGW(TAG, "no wifi ssid configured — clock stays on build-time seed");
        return false;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(esp_netif_init());
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err);
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL));

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, pass ? pass : "", sizeof wc.sta.password);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Three servers, cycled by lwIP on each retry: one lost UDP packet to a
    // single server used to cost the whole window on this weak link, and a
    // board that misses its sync runs the day on the build-time seed.
    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        3, ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.google.com", "time.cloudflare.com"));
    sc.sync_cb = on_sync;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sc));
    ESP_LOGI(TAG, "wifi up, waiting for SNTP (max %ds, screen stays dark)…", timeout_s);

    bool synced = false;
    for (int waited = 0; waited < timeout_s; waited += 5) {
        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(5000)) == ESP_OK) { synced = true; break; }
        ESP_LOGW(TAG, "still waiting for SNTP (%ds)…", waited + 5);
    }

    // Stop the radio but keep the driver's memory: esp_wifi_deinit() frees
    // it, the panel bring-up then reuses it, and the driver's last DMA/task
    // activity lands on top of whatever moved in — measured as LVGL's tick
    // mutex being allocated at the WiFi task's old address and the kernel
    // spinning on it forever. ~50KB of internal RAM stays parked; nothing
    // else on this board needs it. The pause lets the driver's teardown
    // settle before the panel takes the bus.
    esp_netif_sntp_deinit();
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi);
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "wifi off — %s", synced ? "clock synced, radio no longer needed"
                                          : "SNTP never landed, clock stays on the seed");
    return synced;
}
