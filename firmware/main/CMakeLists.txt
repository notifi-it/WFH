idf_component_register(
    SRCS "main.c" "store.c" "derive.c" "config.c" "card.c"
    INCLUDE_DIRS "."
    REQUIRES sqlite3 esp_timer joltwallet__littlefs
)
