// dawn_file.c

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

#include "dawn_file.h"
#include "cJSON.h"
#include "dawn_block.h"
#include "dawn_chat.h"
#include "dawn_date.h"
#include "dawn_fm.h"
#include "dawn_gap.h"
#include "dawn_history.h"
#include "dawn_image.h"
#include "dawn_utils.h"
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// #region History Directory

#ifdef _WIN32
#define DAWN_PATH_SEP "\\"
#else
#define DAWN_PATH_SEP "/"
#endif

//! Build a legacy ~/.dawn path (the pre-XDG default).
static void build_legacy_dir(char* out, size_t out_size)
{
    const char* home = DAWN_BACKEND(app)->home_dir();
    DAWN_ASSERT(home, "home_dir() returned NULL");
    snprintf(out, out_size, "%s" DAWN_PATH_SEP "%s", home, HISTORY_DIR_NAME);
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
    build_legacy_dir(legacy, sizeof(legacy));
    char marker[PATH_MAX];
    snprintf(marker, sizeof(marker), "%s/.sessions", legacy);
    bool is_legacy = DAWN_BACKEND(app)->file_exists(marker);
    if (!is_legacy) {
        snprintf(marker, sizeof(marker), "%s/.history", legacy);
        is_legacy = DAWN_BACKEND(app)->file_exists(marker);
    }
    if (is_legacy) {
        memcpy(path, legacy, sizeof(path));
        return path;
    }

    const char* xdg_data = getenv("XDG_DATA_HOME");
    if (xdg_data && xdg_data[0] == '/') {
        snprintf(path, sizeof(path), "%s/%s", xdg_data, APP_NAME);
    } else {
        const char* home = DAWN_BACKEND(app)->home_dir();
        DAWN_ASSERT(home, "home_dir() returned NULL");
        snprintf(path, sizeof(path), "%s/.local/share/%s", home, APP_NAME);
    }
    return path;
#endif
}

char* config_dir(void)
{
    static char path[PATH_MAX];

#ifdef _WIN32
    const char* appdata = getenv("APPDATA");
    if (appdata && appdata[0]) {
        snprintf(path, sizeof(path), "%s\\%s", appdata, APP_NAME);
    } else {
        build_legacy_dir(path, sizeof(path));
    }
#else
    const char* xdg_config = getenv("XDG_CONFIG_HOME");
    if (xdg_config && xdg_config[0] == '/') {
        snprintf(path, sizeof(path), "%s/%s", xdg_config, APP_NAME);
    } else {
        const char* home = DAWN_BACKEND(app)->home_dir();
        DAWN_ASSERT(home, "home_dir() returned NULL");
        snprintf(path, sizeof(path), "%s/.config/%s", home, APP_NAME);
    }
#endif
    return path;
}

// #endregion

// #region User Info

//! Get current user's display name for document metadata
static const char* get_username(void)
{
    return DAWN_BACKEND(app)->username();
}

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

bool save_session(void)
{
    if (!app.session_path)
        return true;

    if (app.dirty) {
        // dawn's own notes carry a title, an author and the date of their last edit. A file opened
        // from elsewhere keeps the frontmatter it came with, as it came, and gets none if it had
        // none: dawn is a guest in that file.
        if (app.write_fm && path_in_dir(app.session_path, history_dir())) {
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

        size_t len = 0;
        char* content = session_content(&len);
        bool ok = content && DAWN_BACKEND(app)->write_file(app.session_path, content, len);
        free(content);
        if (!ok) {
            app.save_failed = true;
            return false;
        }
        app.dirty = false;
    }
    app.save_failed = false;

    // A note that was never written (opened by a name that is not a file yet, or dawn's own and
    // still empty) has nothing to list in the history.
    if (!DAWN_BACKEND(app)->file_exists(app.session_path))
        return true;

    // Update history
    hist_upsert(app.session_path, fm_get_string(app.frontmatter, "title"), app.cursor);

    // Save AI chat to companion .chat.json file
    if (app.chat_count > 0) {
        char chat_path[520];
        get_chat_path(app.session_path, chat_path, sizeof(chat_path));

        cJSON* root = cJSON_CreateArray();
        for (int32_t i = 0; i < app.chat_count; i++) {
            ChatMessage* m = &app.chat_msgs[i];
            cJSON* msg = cJSON_CreateObject();
            cJSON_AddStringToObject(msg, "role", m->is_user ? "user" : "assistant");
            cJSON_AddStringToObject(msg, "content", m->text);
            cJSON_AddItemToArray(root, msg);
        }

        char* json = cJSON_Print(root);
        cJSON_Delete(root);

        if (json) {
            DAWN_BACKEND(app)->write_file(chat_path, json, strlen(json));
            free(json);
        }
    }
    return true;
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
            snprintf(path, sizeof(path), "%s" DAWN_PATH_SEP "%s", dir, names[i]);
            DAWN_BACKEND(app)->rm(path);
        }
    }
    for (int32_t i = 0; i < count; i++)
        free(names[i]);
    free(names);
}

bool save_note_version(char* out, size_t out_size)
{
    if (!app.session_path || gap_len(&app.text) == 0)
        return false;

    // The note's file name, without its .md and with anything unsafe in a directory name replaced.
    const char* base = strrchr(app.session_path, '/');
    const char* base_win = strrchr(app.session_path, '\\');
    if (base_win && (!base || base_win > base))
        base = base_win;
    base = base ? base + 1 : app.session_path;
    char name[128];
    size_t n = 0;
    for (const char* p = base; *p && n < sizeof(name) - 1; p++) {
        unsigned char c = (unsigned char)*p;
        name[n++] = (isalnum(c) || c == '-' || c == '_' || c == '.' || c >= 0x80) ? (char)c : '_';
    }
    if (n > 3 && strncmp(name + n - 3, ".md", 3) == 0)
        n -= 3;
    name[n] = '\0';
    if (n == 0)
        snprintf(name, sizeof(name), "note");

    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s" DAWN_PATH_SEP "versions" DAWN_PATH_SEP "%s", history_dir(), name);
    if (!DAWN_BACKEND(app)->mkdir_p(dir))
        return false;

    // Named by the UTC time; a second copy within the same second gets a suffix, not the first's place.
    time_t now = (time_t)DAWN_BACKEND(app)->clock(DAWN_CLOCK_SEC);
    struct tm* utc = gmtime(&now);
    char stamp[32];
    if (!utc || strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%SZ", utc) == 0)
        snprintf(stamp, sizeof(stamp), "%lld", (long long)now);
    for (int32_t i = 0;; i++) {
        if (i == 0)
            snprintf(out, out_size, "%s" DAWN_PATH_SEP "%s.md", dir, stamp);
        else
            snprintf(out, out_size, "%s" DAWN_PATH_SEP "%s-%d.md", dir, stamp, i);
        if (!DAWN_BACKEND(app)->file_exists(out))
            break;
        if (i >= 99)
            return false;
    }

    size_t len = 0;
    char* content = session_content(&len);
    bool ok = content && DAWN_BACKEND(app)->write_file(out, content, len);
    free(content);
    if (ok)
        prune_versions(dir);
    return ok;
}

void load_history(void)
{
    hist_load();
}

void load_chat_history(const char* session_path)
{
    char chat_path[520];
    get_chat_path(session_path, chat_path, sizeof(chat_path));

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
    // Only a file that came with frontmatter gets it back on save; a plain file stays plain.
    app.write_fm = fm != NULL;
    app.cursor = 0;
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

bool load_file_for_editing(const char* path)
{
    char* abs = note_path_for(path);
    const char* open_path = abs ? abs : path;

    size_t size;
    char* content = DAWN_BACKEND(app)->read_file(open_path, &size);
    if (!content) {
        free(abs);
        return false;
    }

    load_content(content, size, open_path);
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
