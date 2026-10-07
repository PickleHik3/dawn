// dawn_file.c - The open note on disk: loading, saving, conflicts, snapshots, renames.
//
// What dawn promises about a note's file, on top of dawn_fsio's atomic writes:
// - It never writes over a change made elsewhere. app.disk_hash fingerprints the bytes dawn last
//   loaded or wrote there; save_session() reads the file first and compares. A file deleted
//   elsewhere is written again, with a notice. A file changed elsewhere is a conflict: the
//   writer's text goes to <stem>.conflict-<time>.md beside the note (once per distinct text),
//   saving pauses, and MODE_CONFLICT (dawn.c) asks: reload theirs (one undo step), overwrite with
//   mine, or keep editing. With no unsaved edits a change elsewhere is simply reloaded
//   (note_watch(), a stat every two seconds, the file read only when its stamp moved).
// - It keeps recoverable snapshots: the first write over an existing file in an editing session,
//   and one every 30 minutes after, first copies the file's bytes as they are on disk into
//   versions/ (skipped when the newest copy there holds the same bytes).
// - A failed write is never ignored (an error notice, app.save_failed), a file holding NUL bytes
//   is never opened (a save would write it back cut short), and no path is ever cut short.
// dawn's own writes (the frontmatter's date, AI edits, a rename) update the fingerprint as they
// land, so they never look like a change from elsewhere. The check needs dawn_fsio's reliable
// errno, so it is off on Windows and in the web build, where saving works as it always did.

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

#include "dawn_file.h"
#include "cJSON.h"
#include "dawn_block.h"
#include "dawn_chat.h"
#include "dawn_date.h"
#include "dawn_embed.h"
#include "dawn_fm.h"
#include "dawn_fsio.h"
#include "dawn_gap.h"
#include "dawn_history.h"
#include "dawn_image.h"
#include "dawn_notepath.h"
#include "dawn_notice.h"
#include "dawn_session.h"
#include "dawn_utils.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

//! Whether dawn_fsio is used here: the open note's file is then compared with what dawn last saw
//! before each save. Windows lacks fsio and the web backend's reads set no errno, so both keep
//! the plain backend calls (and rename-based fallbacks) instead.
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#define DAWN_DISK_CHECK 1
#include <sys/stat.h>
#include <unistd.h>
#else
#define DAWN_DISK_CHECK 0
#include "dawn_embed_index.h" // embed_hash(): the same FNV-1a as fsio_hash
#endif

#define STORE_READ_MAX ((size_t)100 * 1024 * 1024) //!< Largest file read whole, as the backend's
#define SNAPSHOT_EVERY_SEC (30 * 60) //!< A snapshot at most this often while a note is edited
#define NOTE_WATCH_MS 2000 //!< How often note_watch() looks at the file

// #region History Directory

#ifdef _WIN32
#define DAWN_PATH_SEP "\\"
#else
#define DAWN_PATH_SEP "/"
#endif

//! Whether an snprintf result w filled out_size without being cut.
static inline bool fitted(int w, size_t out_size) { return w >= 0 && (size_t)w < out_size; }

//! Build a legacy ~/.dawn path (the pre-XDG default); false (and "") when it does not fit.
static bool build_legacy_dir(char* out, size_t out_size)
{
    const char* home = DAWN_BACKEND(app)->home_dir();
    DAWN_ASSERT(home, "home_dir() returned NULL");
    if (fitted(snprintf(out, out_size, "%s" DAWN_PATH_SEP "%s", home, HISTORY_DIR_NAME), out_size))
        return true;
    out[0] = '\0';
    return false;
}

char* history_dir(void)
{
    static char path[PATH_MAX];

#ifdef _WIN32
    build_legacy_dir(path, sizeof(path));
    return path;
#else
    // Legacy path takes precedence so existing users keep their data. The directory alone does
    // not mark one: older builds put the image cache in it, so it exists on installs that never
    // kept a note there. Its history file does.
    static char legacy[PATH_MAX];
    bool is_legacy = false;
    if (build_legacy_dir(legacy, sizeof(legacy))) {
        char marker[PATH_MAX];
        is_legacy = path_join(marker, sizeof(marker), legacy, ".sessions") && DAWN_BACKEND(app)->file_exists(marker);
        if (!is_legacy)
            is_legacy = path_join(marker, sizeof(marker), legacy, ".history") && DAWN_BACKEND(app)->file_exists(marker);
    }
    if (is_legacy) {
        memcpy(path, legacy, sizeof(path));
        return path;
    }

    const char* xdg_data = getenv("XDG_DATA_HOME");
    int w;
    if (xdg_data && xdg_data[0] == '/') {
        w = snprintf(path, sizeof(path), "%s/%s", xdg_data, APP_NAME);
    } else {
        const char* home = DAWN_BACKEND(app)->home_dir();
        DAWN_ASSERT(home, "home_dir() returned NULL");
        w = snprintf(path, sizeof(path), "%s/.local/share/%s", home, APP_NAME);
    }
    if (!fitted(w, sizeof(path)))
        path[0] = '\0';
    return path;
#endif
}

char* config_dir(void)
{
    static char path[PATH_MAX];
    int w;

#ifdef _WIN32
    const char* appdata = getenv("APPDATA");
    if (appdata && appdata[0]) {
        w = snprintf(path, sizeof(path), "%s\\%s", appdata, APP_NAME);
    } else {
        build_legacy_dir(path, sizeof(path));
        return path;
    }
#else
    const char* xdg_config = getenv("XDG_CONFIG_HOME");
    if (xdg_config && xdg_config[0] == '/') {
        w = snprintf(path, sizeof(path), "%s/%s", xdg_config, APP_NAME);
    } else {
        const char* home = DAWN_BACKEND(app)->home_dir();
        DAWN_ASSERT(home, "home_dir() returned NULL");
        w = snprintf(path, sizeof(path), "%s/.config/%s", home, APP_NAME);
    }
#endif
    if (!fitted(w, sizeof(path)))
        path[0] = '\0';
    return path;
}

// #endregion

// #region Storage Helpers

bool path_join(char* out, size_t out_size, const char* dir, const char* name)
{
    if (!out || out_size == 0)
        return false;
    out[0] = '\0';
    if (!dir || !dir[0] || !name)
        return false;
    if (fitted(snprintf(out, out_size, "%s" DAWN_PATH_SEP "%s", dir, name), out_size))
        return true;
    out[0] = '\0';
    return false;
}

char* store_read(const char* path, size_t* out_len, bool* missing)
{
    size_t len = 0;
#if DAWN_DISK_CHECK
    errno = 0;
    char* data = fsio_read_all(path, &len, STORE_READ_MAX);
    if (missing)
        *missing = !data && errno == ENOENT;
#else
    char* data = DAWN_BACKEND(app)->read_file(path, &len);
    if (missing)
        *missing = !data;
#endif
    if (out_len)
        *out_len = data ? len : 0;
    return data;
}

bool store_move_no_replace(const char* from, const char* to)
{
#if DAWN_DISK_CHECK
    return fsio_move_no_replace(from, to);
#else
    if (DAWN_BACKEND(app)->file_exists(to)) {
        errno = EEXIST;
        return false;
    }
    return rename(from, to) == 0;
#endif
}

bool store_quarantine(const char* path, char* out, size_t out_size)
{
#if DAWN_DISK_CHECK
    return fsio_quarantine(path, out, out_size);
#else
    time_t now = time(NULL);
    struct tm* utc = gmtime(&now);
    char stamp[32];
    if (!utc || strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%SZ", utc) == 0)
        snprintf(stamp, sizeof(stamp), "%lld", (long long)now);
    char name[PATH_MAX];
    for (int32_t n = 1; n <= NOTEPATH_MAX_NUMBER; n++) {
        int w = n == 1 ? snprintf(name, sizeof(name), "%s.corrupt-%s", path, stamp)
                       : snprintf(name, sizeof(name), "%s.corrupt-%s-%d", path, stamp, (int)n);
        if (!fitted(w, sizeof(name)) || (out_size > 0 && (size_t)w >= out_size)) {
            errno = ENAMETOOLONG;
            return false;
        }
        if (DAWN_BACKEND(app)->file_exists(name))
            continue;
        if (rename(path, name) != 0)
            return false;
        if (out_size > 0)
            memcpy(out, name, (size_t)w + 1);
        return true;
    }
    errno = EEXIST;
    return false;
#endif
}

uint64_t store_hash(const void* data, size_t len)
{
#if DAWN_DISK_CHECK
    return fsio_hash(data, len);
#else
    return embed_hash(data, len);
#endif
}

//! The file's stamp now; ok is false when it is missing (and always off the check's platforms).
static DiskStamp stamp_of(const char* path)
{
    DiskStamp s = { 0 };
#if DAWN_DISK_CHECK
    struct stat st;
    if (path && stat(path, &st) == 0) {
        s.ok = true;
        s.ino = (uint64_t)st.st_ino;
        s.size = (int64_t)st.st_size;
#if defined(__linux__)
        // Nanoseconds where the filesystem keeps them: two saves within a second still differ.
        s.mtime = (int64_t)st.st_mtim.tv_sec * 1000000000 + (int64_t)st.st_mtim.tv_nsec;
#else
        s.mtime = (int64_t)st.st_mtime;
#endif
    }
#else
    (void)path;
#endif
    return s;
}

#if DAWN_DISK_CHECK
static bool same_stamp(DiskStamp a, DiskStamp b)
{
    if (a.ok != b.ok)
        return false;
    return !a.ok || (a.ino == b.ino && a.size == b.size && a.mtime == b.mtime);
}
#endif

// #endregion

// #region User Info

//! Get current user's display name for document metadata
static const char* get_username(void)
{
    return DAWN_BACKEND(app)->username();
}

// #endregion

// #region What Is On Disk

//! The open note's file name, for notices.
static const char* note_name(void) { return app.session_path ? notepath_base(app.session_path, NULL) : "note"; }

//! The conflict is over (resolved, or another note is open): saving resumes.
static void conflict_end(void)
{
    app.save_paused = false;
    app.conflict_prompt = false;
    app.conflict_left_told = false;
    app.conflict_leave_asked = false;
    free(app.conflict_path);
    app.conflict_path = NULL;
    app.conflict_text_hash = 0;
}

//! dawn now knows what the file holds: present with bytes hashing to hash, or absent.
static void disk_expect(bool present, uint64_t hash, DiskStamp stamp)
{
    app.disk_known = true;
    app.disk_present = present;
    app.disk_hash = present ? hash : 0;
    app.disk_stamp = stamp;
}

//! Another note (or none) is open: forget the last one's file, conflict and snapshot clock.
static void disk_forget(void)
{
    conflict_end();
    app.disk_known = false;
    app.disk_present = false;
    app.disk_hash = 0;
    memset(&app.disk_stamp, 0, sizeof(app.disk_stamp));
    app.snapshot_at = 0;
}

void note_disk_begin_absent(void)
{
    disk_forget();
    disk_expect(false, 0, stamp_of(app.session_path));
}

//! The text in the editor, without frontmatter (whose date changes on every save): what tells
//! whether a conflict copy is still current.
static uint64_t buffer_text_hash(void)
{
    char* txt = gap_to_str(&app.text);
    uint64_t h = txt ? store_hash(txt, gap_len(&app.text)) : 0;
    free(txt);
    return h;
}

//! Replace the editor's text with a file's bytes as ONE undo step: the text before stays one
//! Ctrl+Z away, the reloaded text one Ctrl+Y. The frontmatter becomes the file's, the cursor
//! stays where it was as far as the new text allows, and the note counts as saved.
//! @return false when out of memory (nothing changed)
static bool reload_text(const char* data, size_t len)
{
    size_t consumed = 0;
    Frontmatter* fm = fm_parse(data, len, &consumed);
    if (!fm || consumed > len)
        consumed = 0;
    size_t text_len = len - consumed;
    char* text = malloc(text_len + 1);
    if (!text) {
        fm_free(fm);
        return false;
    }
    memcpy(text, data + consumed, text_len);
    text[text_len] = '\0';
    text_len = normalize_line_endings(text, text_len);

    size_t cursor = app.cursor;
    save_undo_state(); // the writer's text
    size_t old_len = gap_len(&app.text);
    if (old_len > 0)
        gap_delete(&app.text, 0, old_len);
    gap_insert_str(&app.text, 0, text, text_len);
    free(text);

    size_t n = gap_len(&app.text);
    if (cursor > n)
        cursor = n;
    while (cursor > 0 && cursor < n && ((unsigned char)gap_at(&app.text, cursor) & 0xC0) == 0x80)
        cursor--;
    app.cursor = cursor;
    app.selecting = false;
    save_undo_state(); // theirs, so the step back has a step forward

    fm_free(app.frontmatter);
    app.frontmatter = fm;
    app.write_fm = fm != NULL;
    if (app.block_cache)
        block_cache_invalidate((BlockCache*)app.block_cache);
    word_count_invalidate();
    app.dirty = false; // save_undo_state() set it; the editor now holds what the file holds
    DAWN_BACKEND(app)->set_title(fm_get_string(app.frontmatter, "title"));
    return true;
}

#if !DAWN_DISK_CHECK
void note_watch(void) { }
#else
void note_watch(void)
{
    static int64_t last_ms;
    if (!app.session_path || !app.disk_known || app.dirty || app.save_paused)
        return;
    int64_t now = DAWN_BACKEND(app)->clock(DAWN_CLOCK_MS);
    if (now >= last_ms && now - last_ms < NOTE_WATCH_MS)
        return;
    last_ms = now;

    DiskStamp st = stamp_of(app.session_path);
    if (same_stamp(st, app.disk_stamp))
        return;
    app.disk_stamp = st; // each change is looked at once, whatever it turns out to be
    if (!st.ok)
        return; // gone: nothing to reload, and the next save writes it again

    size_t len = 0;
    char* data = store_read(app.session_path, &len, NULL);
    if (!data)
        return;
    uint64_t h = store_hash(data, len);
    if (app.disk_present && h == app.disk_hash) {
        free(data); // touched, not changed
        return;
    }
    if (memchr(data, '\0', len)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "%s changed elsewhere · not text now, not reloaded", note_name());
        notice_post(NOTICE_ERROR, msg);
        free(data);
        return;
    }
    if (reload_text(data, len)) {
        disk_expect(true, h, st);
        notice_post(NOTICE_INFO, "reloaded · changed elsewhere");
    }
    free(data);
}
#endif

// #endregion

// #region Session Persistence

//! The note as its file holds it: the frontmatter, when this note carries one, then the text.
static char* session_content(size_t* out_len)
{
    char* txt = gap_to_str(&app.text);
    if (!txt)
        return NULL;
    size_t txt_len = strlen(txt);

    size_t fm_len = 0;
    char* fm_str = app.write_fm ? fm_to_string(app.frontmatter, &fm_len) : NULL;

    char* content = malloc(fm_len + 1 + txt_len + 1);
    if (!content) {
        free(fm_str);
        free(txt);
        return NULL;
    }

    size_t pos = 0;
    if (fm_str) {
        memcpy(content, fm_str, fm_len);
        pos = fm_len;
        // Add blank line only if text doesn't already start with newline
        if (txt_len == 0 || txt[0] != '\n') {
            content[pos++] = '\n';
        }
    }
    memcpy(content + pos, txt, txt_len);
    content[pos + txt_len] = '\0';

    free(fm_str);
    free(txt);
    *out_len = pos + txt_len;
    return content;
}

//! Whether path is a file inside dir, either as given or with dir's links resolved (note paths
//! are stored resolved; the data dir is not).
static bool path_in_dir(const char* path, const char* dir)
{
    size_t n = strlen(dir);
    if (n == 0)
        return false; // no notes directory (its path did not fit)
    if (strncmp(path, dir, n) == 0 && (path[n] == '/' || path[n] == '\\'))
        return true;
#ifndef _WIN32
    char resolved[PATH_MAX];
    if (realpath(dir, resolved)) {
        n = strlen(resolved);
        return strncmp(path, resolved, n) == 0 && path[n] == '/';
    }
#endif
    return false;
}

bool note_in_history_dir(const char* path) { return path && path_in_dir(path, history_dir()); }

//! The versions directory of the note at note_path: <data dir>/versions/<name>, see
//! notepath_version_dir() for <name>.
static bool version_dir_for(const char* note_path, char* out, size_t out_size)
{
    char name[160];
    char versions[PATH_MAX];
    if (!note_path || !notepath_version_dir(name, sizeof(name), note_path, note_in_history_dir(note_path),
            store_hash(note_path, strlen(note_path))))
        return false;
    return path_join(versions, sizeof(versions), history_dir(), "versions")
        && path_join(out, out_size, versions, name);
}

char* note_rename(const char* stem)
{
    if (!app.session_path || !stem || !stem[0])
        return NULL;
    const char* base = notepath_base(app.session_path, NULL);
    size_t dir_len = base > app.session_path ? (size_t)(base - app.session_path) - 1 : 0;
    char path[PATH_MAX];
    bool moved = false;
    for (int32_t n = 1; n <= NOTEPATH_MAX_NUMBER && !moved; n++) {
        if (!notepath_numbered(path, sizeof(path), app.session_path, dir_len, DAWN_PATH_SEP[0], stem, n))
            return NULL;
        if (strcmp(path, app.session_path) == 0)
            return NULL; // already named so
        moved = store_move_no_replace(app.session_path, path);
        if (!moved && errno != EEXIST)
            return NULL; // not a clash: the rename itself failed
    }
    if (!moved)
        return NULL;
    note_moved(path);
    return dawn_strdup(path);
}

bool note_rename_to(const char* path)
{
    if (!app.session_path || !path || !store_move_no_replace(app.session_path, path))
        return false;
    note_moved(path);
    return true;
}

void note_moved(const char* path)
{
    // The chat that goes with the note moves too, and the history forgets the old name; the next
    // save lists the new one. The file's bytes, inode and time are the same after a rename, so
    // what dawn expects on disk (app.disk_*) still holds.
    char old_chat[PATH_MAX], new_chat[PATH_MAX];
    if (get_chat_path(app.session_path, old_chat, sizeof(old_chat)) && DAWN_BACKEND(app)->file_exists(old_chat)) {
        bool moved = get_chat_path(path, new_chat, sizeof(new_chat));
        if (!moved)
            errno = ENAMETOOLONG;
        else
            moved = store_move_no_replace(old_chat, new_chat);
        if (!moved) {
            char msg[96];
            snprintf(msg, sizeof(msg), "chat not moved with the note · %s", strerror(errno));
            notice_post(NOTICE_ERROR, msg);
        }
    }

    // Its snapshots too. When the new name has a versions directory already, both stay as they are.
    char old_dir[PATH_MAX], new_dir[PATH_MAX];
    if (version_dir_for(app.session_path, old_dir, sizeof(old_dir)) && version_dir_for(path, new_dir, sizeof(new_dir))
        && strcmp(old_dir, new_dir) != 0 && DAWN_BACKEND(app)->file_exists(old_dir))
        store_move_no_replace(old_dir, new_dir);

    hist_remove(app.session_path);
    embed_note_renamed(app.session_path, path);
#if HAS_LIBAI
    session_note_moved(app.session_path, path);
#endif
    free(app.session_path);
    app.session_path = dawn_strdup(path);
}

//! dawn's own notes carry a title, an author and the date of their last edit. A file opened from
//! elsewhere keeps the frontmatter it came with, as it came, and gets none if it had none: dawn is
//! a guest in that file.
static void stamp_frontmatter(void)
{
    if (!app.write_fm || !path_in_dir(app.session_path, history_dir()))
        return;
    Frontmatter* fm = app.frontmatter;
    if (!fm) {
        fm = fm_create();
        app.frontmatter = fm;
    }
    if (!fm_has_key(fm, "title")) {
        fm_set_string(fm, "title", "Untitled");
    }
    if (!fm_has_key(fm, "author")) {
        fm_set_string(fm, "author", get_username());
    }
    DawnTime lt;
    DAWN_BACKEND(app)->localtime(&lt);
    char date_buf[32];
    dawn_format_iso_time(&lt, date_buf, sizeof(date_buf));
    fm_set_string(fm, "date", date_buf);
}

static int compare_names(const void* a, const void* b)
{
    return strcmp(*(char* const*)a, *(char* const*)b);
}

//! Remove all but the newest MAX_NOTE_VERSIONS copies in dir. The names are UTC times, so their
//! order is their age.
static void prune_versions(const char* dir)
{
    char** names = NULL;
    int32_t count = 0;
    if (!DAWN_BACKEND(app)->list_dir(dir, &names, &count))
        return;
    if (count > MAX_NOTE_VERSIONS) {
        qsort(names, (size_t)count, sizeof(char*), compare_names);
        for (int32_t i = 0; i < count - MAX_NOTE_VERSIONS; i++) {
            char path[PATH_MAX];
            if (path_join(path, sizeof(path), dir, names[i]))
                DAWN_BACKEND(app)->rm(path);
        }
    }
    for (int32_t i = 0; i < count; i++)
        free(names[i]);
    free(names);
}

//! The fingerprint of the newest copy in a versions directory; false when there is none.
static bool newest_version_hash(const char* dir, uint64_t* out)
{
    char** names = NULL;
    int32_t count = 0;
    if (!DAWN_BACKEND(app)->list_dir(dir, &names, &count))
        return false;
    const char* newest = NULL;
    for (int32_t i = 0; i < count; i++)
        if (!newest || strcmp(names[i], newest) > 0)
            newest = names[i];
    bool found = false;
    char path[PATH_MAX];
    if (newest && path_join(path, sizeof(path), dir, newest)) {
        size_t len = 0;
        char* data = store_read(path, &len, NULL);
        if (data) {
            *out = store_hash(data, len);
            found = true;
            free(data);
        }
    }
    for (int32_t i = 0; i < count; i++)
        free(names[i]);
    free(names);
    return found;
}

bool save_note_version(const char* content, size_t len, char* out, size_t out_size)
{
    if (!app.session_path || !out || out_size == 0)
        return false;
    out[0] = '\0';
    char* own = NULL;
    if (!content) {
        if (gap_len(&app.text) == 0)
            return false;
        own = session_content(&len);
        if (!own)
            return false;
        content = own;
    } else if (len == 0) {
        return false;
    }

    char dir[PATH_MAX];
    bool ok = version_dir_for(app.session_path, dir, sizeof(dir)) && DAWN_BACKEND(app)->mkdir_p(dir);
    if (ok) {
        // Named by the UTC time; a second copy within the same second gets a suffix, not the first's place.
        time_t now = (time_t)DAWN_BACKEND(app)->clock(DAWN_CLOCK_SEC);
        struct tm* utc = gmtime(&now);
        char stamp[32];
        if (!utc || strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%SZ", utc) == 0)
            snprintf(stamp, sizeof(stamp), "%lld", (long long)now);
        ok = false;
        for (int32_t i = 0; i <= NOTEPATH_MAX_NUMBER; i++) {
            char name[64];
            if (i == 0)
                snprintf(name, sizeof(name), "%s.md", stamp);
            else
                snprintf(name, sizeof(name), "%s-%d.md", stamp, (int)i);
            if (!path_join(out, out_size, dir, name))
                break;
            if (!DAWN_BACKEND(app)->file_exists(out)) {
                ok = true;
                break;
            }
        }
        ok = ok && DAWN_BACKEND(app)->write_file(out, content, len);
    }
    free(own);
    if (!ok) {
        out[0] = '\0';
        return false;
    }
    prune_versions(dir);
    return true;
}

//! Before dawn writes over the file: copy its bytes as they are on disk now into versions/, when
//! this editing session has not yet or not in the last 30 minutes, and the newest copy there is
//! not these bytes already. Silent: a snapshot that fails does not stop the save.
static void snapshot_before_write(const char* disk, size_t len)
{
    int64_t now = DAWN_BACKEND(app)->clock(DAWN_CLOCK_SEC);
    if (app.snapshot_at != 0 && now >= app.snapshot_at && now - app.snapshot_at < SNAPSHOT_EVERY_SEC)
        return;
    app.snapshot_at = now;
    if (len == 0)
        return;
    char dir[PATH_MAX];
    uint64_t newest;
    if (version_dir_for(app.session_path, dir, sizeof(dir)) && newest_version_hash(dir, &newest)
        && newest == store_hash(disk, len))
        return;
    char out[PATH_MAX];
    save_note_version(disk, len, out, sizeof(out));
}

#if DAWN_DISK_CHECK
static bool write_all(int fd, const char* data, size_t len)
{
    while (len > 0) {
        ssize_t w = write(fd, data, len);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        data += w;
        len -= (size_t)w;
    }
    return true;
}
#endif

//! Write content to a new file beside the note, <stem>.conflict-<local YYYYmmdd-HHMMSS>.md (-2,
//! -3 when taken), never over an existing file. Its path goes to out.
static bool conflict_copy_create(const char* content, size_t len, char* out, size_t out_size)
{
    DawnTime lt;
    DAWN_BACKEND(app)->localtime(&lt);
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d", (int)lt.year, (int)lt.mon + 1, (int)lt.mday,
        (int)lt.hour, (int)lt.min, (int)lt.sec);
#if DAWN_DISK_CHECK
    // The bytes reach the disk under a temp name first and then take the first free name in one
    // no-replace move: a crash leaves no half-written copy, and nothing is ever replaced.
    char tmp[PATH_MAX];
    int fd = fsio_create_temp(app.session_path, tmp, sizeof(tmp));
    if (fd < 0)
        return false;
    bool ok = write_all(fd, content, len) && fsync(fd) == 0;
    int saved = errno;
    if (close(fd) != 0 && ok) {
        ok = false;
        saved = errno;
    }
    if (ok) {
        ok = false;
        for (int32_t n = 1; n <= NOTEPATH_MAX_NUMBER; n++) {
            if (!notepath_conflict(out, out_size, app.session_path, stamp, n)) {
                saved = ENAMETOOLONG;
                break;
            }
            if (fsio_move_no_replace(tmp, out)) {
                ok = true;
                break;
            }
            saved = errno;
            if (saved != EEXIST)
                break;
        }
    }
    if (!ok) {
        unlink(tmp);
        errno = saved;
    }
    return ok;
#else
    for (int32_t n = 1; n <= NOTEPATH_MAX_NUMBER; n++) {
        if (!notepath_conflict(out, out_size, app.session_path, stamp, n)) {
            errno = ENAMETOOLONG;
            return false;
        }
        if (!DAWN_BACKEND(app)->file_exists(out))
            return DAWN_BACKEND(app)->write_file(out, content, len);
    }
    errno = EEXIST;
    return false;
#endif
}

//! The file changed elsewhere while the note has unsaved edits: keep the writer's text (content,
//! what the save would have written) in this conflict's copy, pause saving and ask. The copy is
//! written once per distinct text: a save that finds the text as the copy holds it writes nothing.
//! Only a new conflict asks: once the writer chose to keep editing, autosaves just keep the copy
//! current, and esc asks again (dawn.c).
static void conflict_found(const char* content, size_t len)
{
    if (!app.save_paused)
        app.conflict_prompt = true;
    app.save_paused = true;

    uint64_t text_hash = buffer_text_hash();
    if (app.conflict_path && app.conflict_text_hash == text_hash)
        return;
    // Recorded even when the write below fails: it is tried again once the text changes, not on
    // every save.
    app.conflict_text_hash = text_hash;

    char msg[160];
    // Refreshed in place while it is still there; one that was moved or deleted is made anew.
    if (app.conflict_path && DAWN_BACKEND(app)->file_exists(app.conflict_path)) {
        if (!DAWN_BACKEND(app)->write_file(app.conflict_path, content, len)) {
            snprintf(msg, sizeof(msg), "couldn't update the copy of your text · %s", strerror(errno));
            notice_post(NOTICE_ERROR, msg);
        }
        return;
    }
    char path[PATH_MAX];
    if (!conflict_copy_create(content, len, path, sizeof(path))) {
        snprintf(msg, sizeof(msg), "couldn't keep a copy of your text · %s", strerror(errno));
        notice_post(NOTICE_ERROR, msg);
        return;
    }
    free(app.conflict_path);
    app.conflict_path = dawn_strdup(path);
    snprintf(msg, sizeof(msg), "changed elsewhere · your text is in %s", notepath_base(path, NULL));
    notice_post(NOTICE_INFO, msg);
}

//! Write the open note's .chat.json; an error notice once per streak of failures.
static void save_chat(void)
{
    static bool failing;
    char chat_path[PATH_MAX];
    bool ok = get_chat_path(app.session_path, chat_path, sizeof(chat_path));
    if (!ok) {
        errno = ENAMETOOLONG;
    } else {
        cJSON* root = cJSON_CreateArray();
        for (int32_t i = 0; root && i < app.chat_count; i++) {
            ChatMessage* m = &app.chat_msgs[i];
            cJSON* msg = cJSON_CreateObject();
            cJSON_AddStringToObject(msg, "role", m->is_user ? "user" : "assistant");
            cJSON_AddStringToObject(msg, "content", m->text);
            cJSON_AddItemToArray(root, msg);
        }
        char* json = root ? cJSON_Print(root) : NULL;
        cJSON_Delete(root);
        if (!json)
            errno = ENOMEM;
        ok = json && DAWN_BACKEND(app)->write_file(chat_path, json, strlen(json));
        free(json);
    }
    if (!ok && !failing) {
        char msg[96];
        snprintf(msg, sizeof(msg), "couldn't save the chat · %s", strerror(errno));
        notice_post(NOTICE_ERROR, msg);
    }
    failing = !ok;
}

//! A save that did not reach the disk: say why (errno) and keep "not saved" on the status bar.
static bool save_failed(const char* what)
{
    char msg[96];
    snprintf(msg, sizeof(msg), "%s · %s", what, strerror(errno));
    notice_post(NOTICE_ERROR, msg);
    app.save_failed = true;
    return false;
}

bool save_session(void)
{
    if (!app.session_path)
        return true;

    if (app.dirty) {
        stamp_frontmatter();

        size_t len = 0;
        char* content = session_content(&len);
        if (!content) {
            errno = ENOMEM;
            return save_failed("couldn't save");
        }

        // What is on disk now, against what dawn last saw there.
        char* disk = NULL;
        size_t disk_len = 0;
        bool gone = false;
#if DAWN_DISK_CHECK
        if (app.disk_known) {
            bool missing = false;
            disk = store_read(app.session_path, &disk_len, &missing);
            if (!disk && !missing) {
                int e = errno;
                free(content);
                errno = e;
                return save_failed("couldn't read the file before saving");
            }
            gone = !disk && app.disk_present;
            if (disk && (!app.disk_present || store_hash(disk, disk_len) != app.disk_hash)) {
                conflict_found(content, len);
                free(disk);
                free(content);
                return false;
            }
        }
#endif
        if (disk)
            snapshot_before_write(disk, disk_len);
        free(disk);

        // errno is best-effort here (write_file crosses several syscalls), but it's usually still
        // set by whichever one actually failed, and a rough reason beats none.
        if (!DAWN_BACKEND(app)->write_file(app.session_path, content, len)) {
            int e = errno;
            free(content);
            errno = e;
            return save_failed("couldn't save");
        }
        disk_expect(true, store_hash(content, len), stamp_of(app.session_path));
        free(content);
        app.dirty = false;
        if (gone) {
            char msg[128];
            snprintf(msg, sizeof(msg), "%s was deleted elsewhere · saved again", note_name());
            notice_post(NOTICE_INFO, msg);
        }
        if (app.save_paused) {
            // The file is back to what dawn expected (the change elsewhere was undone there).
            conflict_end();
            notice_post(NOTICE_INFO, "saving resumed");
        }

        // The meaning index re-embeds the pieces that changed once the note has been still a while.
        char* body = gap_to_str(&app.text);
        if (body) {
            embed_note_changed(app.session_path, fm_get_string(app.frontmatter, "title"), body,
                gap_len(&app.text));
            free(body);
        }
    }
    app.save_failed = false;

    // A note that was never written (opened by a name that is not a file yet, or dawn's own and
    // still empty) has nothing to list in the history.
    if (!DAWN_BACKEND(app)->file_exists(app.session_path))
        return true;

    // Update history
    hist_upsert(app.session_path, fm_get_string(app.frontmatter, "title"), app.cursor);

    // Save AI chat to companion .chat.json file
    if (app.chat_count > 0)
        save_chat();
    return true;
}

bool note_conflict_reload(void)
{
    if (!app.session_path)
        return false;
    DiskStamp st = stamp_of(app.session_path);
    size_t len = 0;
    bool missing = false;
    char* data = store_read(app.session_path, &len, &missing);
    char msg[128];
    if (!data) {
        if (!missing) {
            snprintf(msg, sizeof(msg), "couldn't read %s · %s", note_name(), strerror(errno));
            notice_post(NOTICE_ERROR, msg);
            return false;
        }
        // Deleted since: there is nothing of theirs to load, so the next save writes the note again.
        conflict_end();
        snprintf(msg, sizeof(msg), "%s is gone now · yours will be saved", note_name());
        notice_post(NOTICE_INFO, msg);
        return true;
    }
    if (memchr(data, '\0', len)) {
        free(data);
        snprintf(msg, sizeof(msg), "%s is not a text file now · not reloaded", note_name());
        notice_post(NOTICE_ERROR, msg);
        return false;
    }
    if (!reload_text(data, len)) {
        free(data);
        return false;
    }
    disk_expect(true, store_hash(data, len), st);
    free(data);
    conflict_end();
    notice_post(NOTICE_INFO, "reloaded theirs · ctrl+z brings yours back");
    return true;
}

bool note_conflict_overwrite(void)
{
    if (!app.session_path)
        return false;
    DiskStamp st = stamp_of(app.session_path);
    size_t len = 0;
    bool missing = false;
    char* data = store_read(app.session_path, &len, &missing);
    if (!data && !missing) {
        char msg[128];
        snprintf(msg, sizeof(msg), "couldn't read %s · %s", note_name(), strerror(errno));
        notice_post(NOTICE_ERROR, msg);
        return false;
    }
    // Theirs becomes what dawn expects there, so the save goes through, and it is copied into
    // versions/ first whatever the 30-minute clock says. Changed yet again in the meantime, the
    // save finds a new conflict instead.
    disk_expect(data != NULL, data ? store_hash(data, len) : 0, st);
    free(data);
    conflict_end();
    app.snapshot_at = 0;
    app.dirty = true;
    if (!save_session())
        return false;
    notice_post(NOTICE_INFO, "saved yours · theirs is kept in versions");
    return true;
}

bool note_conflict_text_changed(void) { return buffer_text_hash() != app.conflict_text_hash; }

void note_conflict_left(void)
{
    char msg[160];
    if (app.conflict_path)
        snprintf(msg, sizeof(msg), "not saved · changed elsewhere · yours is in %s", notepath_base(app.conflict_path, NULL));
    else
        snprintf(msg, sizeof(msg), "not saved · %s changed elsewhere", note_name());
    notice_post(NOTICE_ERROR, msg);
}

void load_history(void)
{
    hist_load();
}

void load_chat_history(const char* session_path)
{
    char chat_path[PATH_MAX];
    if (!get_chat_path(session_path, chat_path, sizeof(chat_path)))
        return;

    size_t size;
    char* json_str = DAWN_BACKEND(app)->read_file(chat_path, &size);
    if (!json_str)
        return;

    cJSON* root = cJSON_Parse(json_str);
    free(json_str);
    if (!root)
        return;

    if (cJSON_IsArray(root)) {
        cJSON* msg;
        cJSON_ArrayForEach(msg, root)
        {
            cJSON* role = cJSON_GetObjectItem(msg, "role");
            cJSON* content = cJSON_GetObjectItem(msg, "content");
            if (cJSON_IsString(role) && cJSON_IsString(content)) {
                bool is_user = strcmp(role->valuestring, "user") == 0;
                chat_add(content->valuestring, is_user);
            }
        }
    }

    cJSON_Delete(root);
}

// #endregion

// #region File Operations

//! Load content into editor, parsing frontmatter
//! @param content buffer containing markdown content (will be modified)
//! @param size size of content buffer
//! @param path optional file path (NULL for stdin)
static void load_content(char* content, size_t size, const char* path)
{
    // Whatever dawn knew about the last note's file (and a conflict over it) was about that note.
    disk_forget();

    // Free old frontmatter
    fm_free(app.frontmatter);
    app.frontmatter = NULL;

    // Parse frontmatter
    const char* text_start = content;
    size_t consumed = 0;
    Frontmatter* fm = fm_parse(content, size, &consumed);
    if (fm) {
        app.frontmatter = fm;
        text_start = content + consumed;
    }

    // Initialize gap buffer with content (normalize CRLF -> LF)
    gap_free(&app.text);
    gap_init(&app.text, 4096);
    size_t text_len = strlen(text_start);
    if (text_len > 0) {
        // Need mutable copy for normalize
        char* mutable_text = dawn_strdup(text_start);
        text_len = normalize_line_endings(mutable_text, text_len);
        gap_insert_str(&app.text, 0, mutable_text, text_len);
        free(mutable_text);
    }

    // Clear image cache when switching documents
    image_clear_all();

    // Reset editor state
    free(app.session_path);
    app.session_path = path ? dawn_strdup(path) : NULL;
    image_set_base_dir_for_note(app.session_path);
    // Only a file that came with frontmatter gets it back on save; a plain file stays plain.
    app.write_fm = fm != NULL;
    app.cursor = 0;
    if (fm) {
        // Past the frontmatter's blank line(s): offset 0 would draw as a bar on an empty row above the title
        size_t blank = 0;
        while (blank < gap_len(&app.text) && gap_at(&app.text, blank) == '\n')
            blank++;
        if (blank < gap_len(&app.text))
            app.cursor = blank;
    }
    app.scroll_y = 0;
    app.selecting = false;
    undo_reset();
    app.dirty = false;
    app.save_failed = false;
    app.timer_done = false;
    app.timer_on = false;
    app.mode = MODE_WRITING;
    app.ai_open = false;
    app.ai_focused = false;
    app.ai_input_len = 0;
    app.ai_input_cursor = 0;
    app.chat_scroll = 0;
    chat_clear();

    // Load associated chat history (only for files)
    if (path) {
        load_chat_history(path);
    }

#if HAS_LIBAI
    if (app.ai_ready && !app.ai_session) {
        ai_init_session();
    }
#endif

    const char* title = fm_get_string(app.frontmatter, "title");
    DAWN_BACKEND(app)->set_title(title);
}

char* note_path_for(const char* path)
{
    if (!path || !path[0])
        return NULL;
#ifdef _WIN32
    return dawn_strdup(path);
#else
    const char* slash = strrchr(path, '/');
    const char* name = path;
    char dir[PATH_MAX];
    if (slash) {
        size_t n = (size_t)(slash - path);
        if (n == 0)
            n = 1; // "/name": the root
        if (n >= sizeof(dir))
            return NULL;
        memcpy(dir, path, n);
        dir[n] = '\0';
        name = slash + 1;
    } else {
        snprintf(dir, sizeof(dir), ".");
    }

    char resolved[PATH_MAX];
    if (!name[0] || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        // The path names a directory; resolve the whole of it and let the open fail on its own.
        if (!realpath(path, resolved))
            return NULL;
        return dawn_strdup(resolved);
    }
    if (!realpath(dir, resolved))
        return NULL;

    size_t n = strlen(resolved) + 1 + strlen(name) + 1;
    char* out = malloc(n);
    if (out)
        snprintf(out, n, "%s%s%s", resolved, strcmp(resolved, "/") == 0 ? "" : "/", name);
    return out;
#endif
}

static bool g_refused_binary;

bool load_refused_binary(void) { return g_refused_binary; }

bool load_file_for_editing(const char* path)
{
    g_refused_binary = false;
    char* abs = note_path_for(path);
    const char* open_path = abs ? abs : path;

    // Stamped before the read: a change landing in between shows up as a moved stamp later.
    DiskStamp stamp = stamp_of(open_path);
    size_t size;
    char* content = DAWN_BACKEND(app)->read_file(open_path, &size);
    if (!content) {
        free(abs);
        return false;
    }

    // Text stops at a NUL byte, so the note would open cut short there and the next save would
    // write the cut text over the file. Not text, not opened.
    if (memchr(content, '\0', size)) {
        g_refused_binary = true;
        char msg[128];
        snprintf(msg, sizeof(msg), "%s is not a text file · not opened", notepath_base(open_path, NULL));
        notice_post(NOTICE_ERROR, msg);
        free(content);
        free(abs);
        return false;
    }

    uint64_t hash = store_hash(content, size);
    load_content(content, size, open_path);
    disk_expect(true, hash, stamp);
    free(content);
    free(abs);
    return true;
}

void load_buffer_for_editing(const char* content, size_t size)
{
    if (!content || size == 0)
        return;

    // Make a mutable copy
    char* buf = malloc(size + 1);
    if (!buf)
        return;
    memcpy(buf, content, size);
    buf[size] = '\0';

    load_content(buf, size, NULL);
    free(buf);
}

void open_in_finder(const char* path)
{
    DAWN_BACKEND(app)->reveal(path);
}

// #endregion
