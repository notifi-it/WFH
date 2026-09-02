// One append-only file per day. Appending is the one thing LittleFS is
// actually good at — it is copy-on-write, so rewriting the middle of a file
// costs block copies that get worse as the file grows.
//
// Line format is TSV: plain text, greppable, curl-able, writable with one
// fprintf and parseable with one sscanf, so there is no JSON parser in the
// firmware.
//
//   <ts>\t<slot>\t<action>\t<kind>\t<uuid>\n
//
// The uuid is a per-line identity for external tooling. The firmware writes
// it and never parses it.
#include "store.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_random.h"

#include "derive.h"

static const char *TAG = "store";
#define DAY_DIR "/fs/d"

const char *store_day_key(time_t ts) {
    static char buf[11];
    struct tm tm;
    localtime_r(&ts, &tm);
    strftime(buf, sizeof buf, "%Y-%m-%d", &tm);
    return buf;
}

static void day_path(const char *day, char *out, size_t n) {
    snprintf(out, n, DAY_DIR "/%s", day);
}

esp_err_t store_open(void) {
    esp_vfs_littlefs_conf_t fs = {
        .base_path = "/fs", .partition_label = "storage", .format_if_mount_failed = true,
    };
    ESP_ERROR_CHECK(esp_vfs_littlefs_register(&fs));

    size_t total = 0, used = 0;
    esp_littlefs_info(fs.partition_label, &total, &used);
    ESP_LOGI(TAG, "littlefs mounted: %u KB used of %u KB", (unsigned)(used / 1024), (unsigned)(total / 1024));

    mkdir(DAY_DIR, 0755);
    return ESP_OK;
}

void store_load_day(const char *day, day_log_t *out, const settings_t *s) {
    memset(out, 0, sizeof *out);

    char path[64];
    day_path(day, path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[160];
    while (fgets(line, sizeof line, f) && out->events_len < EVENTS_MAX) {
        long long ts = 0, slot = 0;
        char action[24] = {0}, kind[8] = {0};
        if (sscanf(line, "%lld\t%lld\t%23[^\t]\t%7[^\t]", &ts, &slot, action, kind) != 4) continue;

        int a = -1;
        for (int i = 0; i < s->n_actions; i++) if (strcmp(s->actions[i].id, action) == 0) { a = i; break; }
        if (a < 0) continue;

        event_kind_t k;
        if (!wfh_kind_parse(kind, &k)) continue;

        log_event_t *e = &out->events[out->events_len++];
        e->action = a;
        e->kind   = k;
        e->ts     = (time_t)ts;
        e->slot   = (time_t)slot;
    }
    fclose(f);
}

// The open day, held between writes. Measured, not assumed: opening a file
// costs a linear scan of the directory, so with a 400-day retention window
// an open-per-write makes every tap pay for every day ever recorded. The
// board writes ~30 events into the same day, so the handle and the day's
// events stay resident and a rollover is the only thing that pays.
// This is also exactly §4's in-RAM g_day, arrived at from the other side.
static char      g_open_day[11];
static FILE     *g_fp;
static day_log_t g_cache;

static void close_day(void) {
    if (g_fp) { fclose(g_fp); g_fp = NULL; }
    g_open_day[0] = '\0';
}

static bool open_day(const char *day, const settings_t *s) {
    if (g_fp && strcmp(day, g_open_day) == 0) return true;
    close_day();

    store_load_day(day, &g_cache, s);          // one read per day, not per write

    char path[64];
    day_path(day, path, sizeof path);
    g_fp = fopen(path, "a");
    if (!g_fp) { ESP_LOGE(TAG, "open %s failed", path); return false; }

    strncpy(g_open_day, day, sizeof g_open_day - 1);
    return true;
}

bool store_add_event(const log_event_t *ev, const settings_t *s) {
    const char *day = store_day_key(ev->ts);
    if (!open_day(day, s)) return false;

    // Idempotence per (action, slot): the latest event is the truth (§3.4),
    // so a repeat of whatever is already latest is a duplicate and is not
    // written. A different kind — an undo after a done, a done after an
    // undo — is a real change and appends.
    const log_event_t *latest = wfh_latest_event(&g_cache, ev->action, ev->slot);
    if (latest ? latest->kind == ev->kind : ev->kind == KIND_UNDO) return false;

    fprintf(g_fp, "%lld\t%lld\t%s\t%s\t%08lx%08lx\n",
            (long long)ev->ts, (long long)ev->slot,
            s->actions[ev->action].id, wfh_kind_name(ev->kind),
            (unsigned long)esp_random(), (unsigned long)esp_random());

    fflush(g_fp);
    fsync(fileno(g_fp));                       // the line is on flash before the tap is acknowledged

    if (g_cache.events_len < EVENTS_MAX) g_cache.events[g_cache.events_len++] = *ev;
    return true;
}

void store_prune(int keep_days) {
    const char *cutoff = store_day_key(time(NULL) - (time_t)keep_days * 86400);

    DIR *d = opendir(DAY_DIR);
    if (!d) return;

    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        if (strcmp(e->d_name, cutoff) < 0) {      // ISO dates sort lexically
            char path[64];
            day_path(e->d_name, path, sizeof path);
            unlink(path);
        }
    }
    closedir(d);
}
