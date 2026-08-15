# STORE_BACKEND picks the storage implementation behind store.h (§15.1 R1).
# Both satisfy the same six functions, which is the whole point.
if(NOT DEFINED STORE_BACKEND)
    set(STORE_BACKEND "files")          # "sqlite" to compare
endif()

if(STORE_BACKEND STREQUAL "sqlite")
    set(STORE_SRC "store.c")
    set(STORE_REQ sqlite3)
else()
    set(STORE_SRC "store_files.c")
    set(STORE_REQ "")
endif()

idf_component_register(
    SRCS "main.c" ${STORE_SRC} "derive.c" "config.c" "card.c"
    INCLUDE_DIRS "."
    REQUIRES ${STORE_REQ} esp_timer joltwallet__littlefs
)
