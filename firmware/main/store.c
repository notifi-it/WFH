#include "store.h"

#include <string.h>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_random.h"
#include "sqlite3.h"

static const char *TAG = "store";
static sqlite3 *g_db;
static char g_journal[16] = "unknown";

static const char *SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS events ("
    "  id     TEXT PRIMARY KEY,"
    "  day    TEXT NOT NULL,"
    "  action TEXT NOT NULL,"
    "  kind   TEXT NOT NULL CHECK (kind IN ('done','skip')),"
    "  ts     INTEGER NOT NULL,"
    "  slot   INTEGER NOT NULL,"
    "  UNIQUE (action, slot)"
    ");"
    "CREATE INDEX IF NOT EXISTS events_day ON events (day);"
    "CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT NOT NULL);";

const char *store_day_key(time_t ts) {
    static char buf[11];
    struct tm tm;
    localtime_r(&ts, &tm);
    strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
    return buf;
}

/** Read back a single-value pragma so we can verify it took. */
static void pragma_into(const char *sql, char *out, size_t n) {
    sqlite3_stmt *st;
    out[0] = '\0';

    const int rc = sqlite3_prepare_v2(g_db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK) {
        ESP_LOGE(TAG, "pragma prepare failed (%d): %s -- %s", rc, sqlite3_errmsg(g_db), sql);
        return;
    }
    const int step = sqlite3_step(st);
    if (step == SQLITE_ROW) {
        const unsigned char *v = sqlite3_column_text(st, 0);
        if (v) { strncpy(out, (const char *)v, n - 1); out[n - 1] = '\0'; }
    } else {
        ESP_LOGE(TAG, "pragma step returned %d: %s -- %s", step, sqlite3_errmsg(g_db), sql);
    }
    sqlite3_finalize(st);
}

esp_err_t store_open(void) {
    esp_vfs_littlefs_conf_t fs = {
        .base_path = "/fs", .partition_label = "storage", .format_if_mount_failed = true,
    };
    ESP_ERROR_CHECK(esp_vfs_littlefs_register(&fs));

    size_t total = 0, used = 0;
    esp_littlefs_info(fs.partition_label, &total, &used);
    ESP_LOGI(TAG, "littlefs mounted: %u KB used of %u KB", (unsigned)(used / 1024), (unsigned)(total / 1024));

    sqlite3_initialize();
    if (sqlite3_open("/fs/wfh.db", &g_db) != SQLITE_OK) {
        ESP_LOGE(TAG, "open failed: %s", sqlite3_errmsg(g_db));
        return ESP_FAIL;
    }

    // This port's VFS has no shared-memory methods, and SQLite only allows
    // WAL without them when locking_mode is EXCLUSIVE before the first WAL
    // access. One connection ever, so exclusive costs nothing.
    sqlite3_exec(g_db, "PRAGMA locking_mode=EXCLUSIVE;", NULL, NULL, NULL);
    pragma_into("PRAGMA journal_mode=WAL;", g_journal, sizeof g_journal);
    if (strcasecmp(g_journal, "wal") != 0) ESP_LOGW(TAG, "no WAL, running on %s", g_journal);

    sqlite3_exec(g_db, "PRAGMA synchronous=FULL;", NULL, NULL, NULL);
    sqlite3_exec(g_db, "PRAGMA cache_size=-1024;", NULL, NULL, NULL);   // 1MB, R1 mitigation
    sqlite3_exec(g_db, SCHEMA_SQL, NULL, NULL, NULL);
    return ESP_OK;
}

void store_close(void) {
    if (g_db) { sqlite3_close(g_db); g_db = NULL; }   // checkpoints the WAL
}

const char *store_journal_mode(void) { return g_journal; }

bool store_add_event(const log_event_t *ev, const settings_t *s) {
    static const char *SQL =
        "INSERT OR IGNORE INTO events (id, day, action, kind, ts, slot) VALUES (?1,?2,?3,?4,?5,?6);";

    char uuid[37];
    snprintf(uuid, sizeof uuid, "%08lx-%04lx-4%03lx-%04lx-%08lx%04lx",
             (unsigned long)esp_random(), (unsigned long)(esp_random() & 0xffff),
             (unsigned long)(esp_random() & 0xfff),
             (unsigned long)((esp_random() & 0x3fff) | 0x8000),
             (unsigned long)esp_random(), (unsigned long)(esp_random() & 0xffff));

    sqlite3_stmt *st;
    const int prc = sqlite3_prepare_v2(g_db, SQL, -1, &st, NULL);
    if (prc != SQLITE_OK) {
        ESP_LOGE(TAG, "prepare failed (%d): %s", prc, sqlite3_errmsg(g_db));
        return false;
    }
    sqlite3_bind_text(st, 1, uuid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, store_day_key(ev->ts), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, s->actions[ev->action].id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 4, ev->kind == KIND_DONE ? "done" : "skip", -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 5, ev->ts);
    sqlite3_bind_int64(st, 6, ev->slot);

    const int rc = sqlite3_step(st);
    sqlite3_finalize(st);

    if (rc != SQLITE_DONE) { ESP_LOGE(TAG, "insert: %s", sqlite3_errmsg(g_db)); return false; }
    return sqlite3_changes(g_db) > 0;
}

void store_load_day(const char *day, day_log_t *out, const settings_t *s) {
    memset(out, 0, sizeof *out);

    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(g_db,
            "SELECT action, kind, ts, slot FROM events WHERE day = ?1 ORDER BY ts;",
            -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, day, -1, SQLITE_STATIC);

    while (sqlite3_step(st) == SQLITE_ROW && out->events_len < EVENTS_MAX) {
        const char *id = (const char *)sqlite3_column_text(st, 0);
        int a = -1;
        for (int i = 0; i < s->n_actions; i++) if (strcmp(s->actions[i].id, id) == 0) { a = i; break; }
        if (a < 0) continue;

        log_event_t *e = &out->events[out->events_len++];
        e->action = a;
        e->kind   = strcmp((const char *)sqlite3_column_text(st, 1), "done") == 0 ? KIND_DONE : KIND_SKIP;
        e->ts     = sqlite3_column_int64(st, 2);
        e->slot   = sqlite3_column_int64(st, 3);
    }
    sqlite3_finalize(st);
}

int store_count_events(void) {
    sqlite3_stmt *st;
    int n = 0;
    if (sqlite3_prepare_v2(g_db, "SELECT COUNT(*) FROM events;", -1, &st, NULL) != SQLITE_OK) return -1;
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

void store_prune(int keep_days) {
    char sql[128];
    snprintf(sql, sizeof sql,
             "DELETE FROM events WHERE day < date('now','localtime','-%d days');", keep_days);
    sqlite3_exec(g_db, sql, NULL, NULL, NULL);
}
