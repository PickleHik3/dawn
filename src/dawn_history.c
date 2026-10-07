// dawn_history.c - Session history management
//
// The list of notes lives in <notes dir>/.sessions as a CRDT (dawn_crdt.c), merged with what is on
// disk before every write so two dawns never drop each other's entries. A .sessions that exists
// but does not parse is set aside as .sessions.corrupt-<time> (with a notice) before anything is
// written in its place, and one that cannot be read at all is not written over. A failed write
// says so once, until a write gets through again.

#include "dawn_history.h"
#include "cJSON.h"
#include "dawn_crdt.h"
#include "dawn_date.h"
#include "dawn_file.h"
#include "dawn_notepath.h"
#include "dawn_notice.h"
#include "dawn_types.h"
#include "dawn_utils.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static CrdtState* hist_state = NULL;

// #region Helpers

//! <notes dir>/.sessions, or NULL when that path does not fit.
static const char* sessions_file_path(void)
{
    static char path[PATH_MAX];
    return path_join(path, sizeof(path), history_dir(), ".sessions") ? path : NULL;
}

//! <notes dir>/.history (the format before the CRDT), or NULL when that path does not fit.
static const char* legacy_history_path(void)
{
    static char path[PATH_MAX];
    return path_join(path, sizeof(path), history_dir(), ".history") ? path : NULL;
}

//! A notice for the first failure of a streak (*failing false), none for the rest; a success ends
//! the streak.
static void report(bool ok, bool* failing, const char* what)
{
    if (!ok && !*failing) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s · %s", what, strerror(errno));
        notice_post(NOTICE_ERROR, msg);
    }
    *failing = !ok;
}

static bool g_read_failing; //!< .sessions could not be read (told once)
static bool g_write_failing; //!< .sessions could not be written (told once)

//! Write state to .sessions (the notes directory made first).
static bool write_sessions(CrdtState* state)
{
    const char* path = sessions_file_path();
    char* json = crdt_serialize(state);
    bool ok = false;
    if (!path)
        errno = ENAMETOOLONG;
    else if (!json)
        errno = ENOMEM;
    else
        ok = DAWN_BACKEND(app)->mkdir_p(history_dir()) && DAWN_BACKEND(app)->write_file(path, json, strlen(json));
    free(json);
    report(ok, &g_write_failing, "couldn't save the history");
    return ok;
}

static bool all_blank(const char* s, size_t len)
{
    for (size_t i = 0; i < len; i++)
        if (s[i] != ' ' && s[i] != '\t' && s[i] != '\n' && s[i] != '\r')
            return false;
    return true;
}

static char* normalize_path(const char* path)
{
    if (!path)
        return NULL;
    char* result = dawn_strdup(path);
#ifdef _WIN32
    for (char* p = result; *p; p++) {
        if (*p == '/')
            *p = '\\';
    }
#else
    for (char* p = result; *p; p++) {
        if (*p == '\\')
            *p = '/';
    }
#endif
    return result;
}

static void format_date(int64_t timestamp, char* buf, size_t len)
{
    if (timestamp == 0) {
        snprintf(buf, len, "Unknown");
        return;
    }

    DawnTime lt;
    DAWN_BACKEND(app)->localtime_from(&lt, timestamp / 1000);
    dawn_format_human_time(&lt, buf, len);
}

static void rebuild_history_array(void)
{
    hist_free();

    if (!hist_state)
        return;

    int32_t count;
    CrdtEntry** live = crdt_get_live(hist_state, &count);
    if (!live || count == 0) {
        free(live);
        return;
    }

    app.history = malloc(sizeof(HistoryEntry) * (size_t)count);
    app.hist_count = 0;

    for (int32_t i = 0; i < count; i++) {
        CrdtEntry* e = live[i];

        if (!DAWN_BACKEND(app)->file_exists(e->key))
            continue;

        HistoryEntry* entry = &app.history[app.hist_count];
        entry->path = dawn_strdup(e->key);
        entry->title = e->value ? dawn_strdup(e->value) : NULL;

        char date_buf[64];
        format_date(e->timestamp, date_buf, sizeof(date_buf));
        entry->date_str = dawn_strdup(date_buf);

        int64_t cursor_val = 0;
        if (crdt_meta_get_int(e, "cursor", &cursor_val))
            entry->cursor = (size_t)cursor_val;
        else
            entry->cursor = 0;

        app.hist_count++;
    }

    free(live);
}

static void normalize_crdt_keys(CrdtState* state)
{
    if (!state)
        return;
    for (int32_t i = 0; i < state->entry_count; i++) {
        char* norm = normalize_path(state->entries[i].key);
        free(state->entries[i].key);
        state->entries[i].key = norm;
    }
    for (int32_t i = 0; i < state->tombstone_count; i++) {
        char* norm = normalize_path(state->tombstones[i].key);
        free(state->tombstones[i].key);
        state->tombstones[i].key = norm;
    }
}

//! .sessions as it is on disk. NULL with *writable set: there is none yet (missing or empty), or
//! it did not parse and was set aside, so writing a new one loses nothing. NULL without: it could
//! not be read, or not set aside, so it must not be written over.
static CrdtState* load_disk_state(bool* writable)
{
    *writable = false;
    const char* path = sessions_file_path();
    if (!path) {
        errno = ENAMETOOLONG;
        report(false, &g_read_failing, "couldn't read the history");
        return NULL;
    }
    size_t len = 0;
    bool missing = false;
    char* content = store_read(path, &len, &missing);
    if (!content) {
        report(missing, &g_read_failing, "couldn't read the history");
        *writable = missing;
        return NULL;
    }
    g_read_failing = false;

    bool blank = all_blank(content, len);
    CrdtState* state = blank ? NULL : crdt_parse(content, len);
    free(content);
    if (state || blank) {
        normalize_crdt_keys(state);
        *writable = true;
        return state;
    }

    // Damaged: kept under another name for whoever wants to look, never overwritten.
    char moved[PATH_MAX];
    char msg[128];
    if (store_quarantine(path, moved, sizeof(moved))) {
        snprintf(msg, sizeof(msg), "history file was damaged · kept as %s", notepath_base(moved, NULL));
        *writable = true;
    } else {
        snprintf(msg, sizeof(msg), "history file is damaged · couldn't set it aside: %s", strerror(errno));
    }
    notice_post(NOTICE_ERROR, msg);
    return NULL;
}

static CrdtState* migrate_v1_to_crdt(const char* json, size_t len)
{
    cJSON* root = cJSON_ParseWithLength(json, len);
    if (!root || !cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return NULL;
    }

    CrdtState* state = crdt_create();

    cJSON* item;
    cJSON_ArrayForEach(item, root)
    {
        cJSON* path_j = cJSON_GetObjectItem(item, "path");
        cJSON* title_j = cJSON_GetObjectItem(item, "title");
        cJSON* modified_j = cJSON_GetObjectItem(item, "modified");

        if (!path_j || !cJSON_IsString(path_j))
            continue;

        char* norm_path = normalize_path(path_j->valuestring);

        if (!DAWN_BACKEND(app)->file_exists(norm_path)) {
            free(norm_path);
            continue;
        }

        const char* title = (title_j && cJSON_IsString(title_j)) ? title_j->valuestring : NULL;

        crdt_upsert(state, norm_path, title);

        if (modified_j && cJSON_IsNumber(modified_j)) {
            CrdtEntry* e = crdt_find(state, norm_path);
            if (e)
                e->timestamp = (int64_t)(modified_j->valuedouble * 1000);
        }
        free(norm_path);
    }

    cJSON_Delete(root);
    return state;
}

// #endregion

// #region Lifecycle

void hist_load(void)
{
    if (hist_state) {
        crdt_free(hist_state);
        hist_state = NULL;
    }
    hist_free();

    // The old .history becomes .sessions (merged into one already there), and goes only once the
    // new file is written: a failed write leaves it to try again next time.
    const char* legacy = legacy_history_path();
    if (legacy && DAWN_BACKEND(app)->file_exists(legacy)) {
        size_t len;
        char* content = DAWN_BACKEND(app)->read_file(legacy, &len);
        CrdtState* migrated = content ? migrate_v1_to_crdt(content, len) : NULL;
        free(content);
        if (migrated) {
            bool writable;
            CrdtState* existing = load_disk_state(&writable);
            if (existing) {
                CrdtState* merged = crdt_merge(migrated, existing);
                crdt_free(existing);
                if (merged) {
                    crdt_free(migrated);
                    migrated = merged;
                }
            }
            if (writable && write_sessions(migrated))
                remove(legacy);
            crdt_free(migrated);
        }
    }

    bool writable;
    hist_state = load_disk_state(&writable);
    if (hist_state)
        rebuild_history_array();
}

void hist_save(void)
{
    if (!hist_state)
        hist_state = crdt_create();

    bool writable;
    CrdtState* disk_state = load_disk_state(&writable);

    if (disk_state) {
        CrdtState* merged = crdt_merge(hist_state, disk_state);
        crdt_free(disk_state);
        if (merged) {
            crdt_free(hist_state);
            hist_state = merged;
        }
    }

    // Not written over a file that could not be read: its entries would be lost.
    if (writable)
        write_sessions(hist_state);

    rebuild_history_array();
}

void hist_free(void)
{
    if (app.history) {
        for (int32_t i = 0; i < app.hist_count; i++) {
            free(app.history[i].path);
            free(app.history[i].title);
            free(app.history[i].date_str);
        }
        free(app.history);
        app.history = NULL;
    }
    app.hist_count = 0;
    app.hist_sel = 0;
}

void hist_shutdown(void)
{
    hist_free();
    if (hist_state) {
        crdt_free(hist_state);
        hist_state = NULL;
    }
}

// #endregion

// #region Operations

bool hist_upsert(const char* path, const char* title, size_t cursor)
{
    if (!path)
        return false;

    if (!hist_state)
        hist_load();
    if (!hist_state)
        hist_state = crdt_create();

    char* norm_path = normalize_path(path);
    crdt_upsert(hist_state, norm_path, title);

    CrdtEntry* entry = crdt_find(hist_state, norm_path);
    if (entry)
        crdt_meta_set_int(entry, "cursor", (int64_t)cursor);

    free(norm_path);

    hist_save();
    return true;
}

bool hist_remove(const char* path)
{
    if (!path || !hist_state)
        return false;

    char* norm_path = normalize_path(path);
    CrdtEntry* entry = crdt_find(hist_state, norm_path);
    if (!entry) {
        free(norm_path);
        return false;
    }

    crdt_remove(hist_state, norm_path);
    free(norm_path);

    hist_save();
    return true;
}

HistEntry* hist_find(const char* path)
{
    if (!path)
        return NULL;

    char* norm_path = normalize_path(path);
    for (int32_t i = 0; i < app.hist_count; i++) {
        if (strcmp(app.history[i].path, norm_path) == 0) {
            free(norm_path);
            return (HistEntry*)&app.history[i];
        }
    }
    free(norm_path);
    return NULL;
}

// #endregion
