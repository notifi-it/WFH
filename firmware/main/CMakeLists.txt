idf_component_register(
    SRCS "main.c" "icons/icons.c" "day.c" "input.c" "ui.c" "feedback.c" "audio.c"
         "store_files.c" "derive.c" "config.c" "card.c" "wifi_time.c"
    INCLUDE_DIRS "." "icons"
    REQUIRES esp_timer joltwallet__littlefs esp_codec_dev esp_driver_i2s espressif__esp_lcd_touch espressif__esp_lvgl_port
             esp_wifi esp_netif esp_event nvs_flash
)
