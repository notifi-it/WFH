idf_component_register(
    SRCS "main.c" "day.c" "input.c" "ui.c" "feedback.c" "audio.c"
         "fonts/geist_14.c" "fonts/geist_18.c" "fonts/geist_22.c"
         "fonts/geist_28.c" "fonts/geist_bold_30.c"
         "store_files.c" "derive.c" "config.c" "card.c"
    INCLUDE_DIRS "." "fonts"
    REQUIRES esp_timer joltwallet__littlefs esp_codec_dev esp_driver_i2s
)
