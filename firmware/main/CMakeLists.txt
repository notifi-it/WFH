idf_component_register(
    SRCS "main.c" "day.c" "input.c" "ui.c" "feedback.c" "audio.c"
         "store_files.c" "derive.c" "config.c" "card.c"
    INCLUDE_DIRS "." "fonts"
    REQUIRES esp_timer joltwallet__littlefs esp_codec_dev esp_driver_i2s
)
