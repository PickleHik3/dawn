// dawn_settings.c - settings.json in the config directory: theme, timer, Nerd Font boxes.
//
// A save rewrites only the keys dawn sets and keeps every other one the file holds (nerd_font,
// written by hand or by the launcher, among them). A settings.json that does not parse is set
// aside as settings.json.corrupt-<time>, with a notice, and the defaults are used; one that cannot
// be read is not written over. A failed write says so once, until a write gets through again.

#include "dawn_settings.h"
#include "cJSON.h"
#include "dawn_file.h"
#include "dawn_notepath.h"
#include "dawn_notice.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// #region Helpers

//! <config dir>/settings.json, or NULL when that path does not fit.
static const char* settings_path(void)
{
    static char path[PATH_MAX];
    return path_join(path, sizeof(path), config_dir(), "settings.json") ? path : NULL;
}

static void apply_preset_for_minutes(int32_t mins)
{
    for (size_t i = 0; i < NUM_PRESETS; i++) {
        if (TIMER_PRESETS[i] == mins) {
            app.preset_idx = (int32_t)i;
            return;
        }
    }
}

static bool g_read_failing; //!< settings.json could not be read (told once)
static bool g_write_failing; //!< settings.json could not be written (told once)

//! An error notice for the first failure of a streak; a success ends the streak.
static void report(bool ok, bool* failing, const char* what)
{
    if (!ok && !*failing) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s · %s", what, strerror(errno));
        notice_post(NOTICE_ERROR, msg);
    }
    *failing = !ok;
}

//! settings.json as a JSON object. NULL with *writable set: there is none (missing or empty), or
//! it did not parse and was set aside (a notice says so). NULL without: it could not be read, or
//! not set aside, so it must not be written over.
static cJSON* read_settings(bool* writable)
{
    *writable = false;
    const char* path = settings_path();
    if (!path) {
        errno = ENAMETOOLONG;
        report(false, &g_read_failing, "couldn't read settings");
        return NULL;
    }
    size_t len = 0;
    bool missing = false;
    char* json = store_read(path, &len, &missing);
    if (!json) {
        report(missing, &g_read_failing, "couldn't read settings · using defaults");
        *writable = missing;
        return NULL;
    }
    g_read_failing = false;

    bool blank = true;
    for (size_t i = 0; i < len && blank; i++)
        blank = json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r';
    cJSON* root = blank ? NULL : cJSON_ParseWithLength(json, len);
    free(json);
    if (blank || cJSON_IsObject(root)) {
        *writable = true;
        return root;
    }
    cJSON_Delete(root);

    // Damaged: kept under another name, never overwritten.
    char moved[PATH_MAX];
    char msg[128];
    if (store_quarantine(path, moved, sizeof(moved))) {
        snprintf(msg, sizeof(msg), "settings were damaged · kept as %s · using defaults", notepath_base(moved, NULL));
        *writable = true;
    } else {
        snprintf(msg, sizeof(msg), "settings are damaged · using defaults · %s", strerror(errno));
    }
    notice_post(NOTICE_ERROR, msg);
    return NULL;
}

//! Set key in obj to item, replacing any value it had.
static void set_item(cJSON* obj, const char* key, cJSON* item)
{
    if (!item)
        return;
    bool ok = cJSON_GetObjectItemCaseSensitive(obj, key)
        ? cJSON_ReplaceItemInObjectCaseSensitive(obj, key, item)
        : cJSON_AddItemToObject(obj, key, item);
    if (!ok)
        cJSON_Delete(item);
}

// #endregion

// #region Lifecycle

void settings_load(void)
{
    bool writable;
    cJSON* root = read_settings(&writable);
    if (!root)
        return;

    cJSON* theme_j = cJSON_GetObjectItem(root, "theme");
    if (cJSON_IsString(theme_j)) {
        if (strcmp(theme_j->valuestring, "light") == 0)
            app.theme = THEME_LIGHT;
        else if (strcmp(theme_j->valuestring, "dark") == 0)
            app.theme = THEME_DARK;
    }

    cJSON* nerd_j = cJSON_GetObjectItem(root, "nerd_font");
    if (cJSON_IsBool(nerd_j))
        app.nerd_font = cJSON_IsTrue(nerd_j);

    cJSON* timer_j = cJSON_GetObjectItem(root, "timer_mins");
    if (cJSON_IsNumber(timer_j)) {
        int32_t mins = (int32_t)timer_j->valuedouble;
        if (mins < 0)
            mins = 0;
        app.timer_mins = mins;
        apply_preset_for_minutes(mins);
    }

    cJSON_Delete(root);
}

void settings_save(void)
{
    bool writable;
    cJSON* root = read_settings(&writable);
    if (!writable) {
        cJSON_Delete(root);
        return;
    }
    if (!root)
        root = cJSON_CreateObject();
    if (!root)
        return;

    set_item(root, "theme", cJSON_CreateString(app.theme == THEME_DARK ? "dark" : "light"));
    set_item(root, "timer_mins", cJSON_CreateNumber((double)app.timer_mins));

    char* json = cJSON_Print(root);
    cJSON_Delete(root);
    const char* path = settings_path();
    bool ok = false;
    if (!json)
        errno = ENOMEM;
    else
        ok = DAWN_BACKEND(app)->mkdir_p(config_dir()) && DAWN_BACKEND(app)->write_file(path, json, strlen(json));
    free(json);
    report(ok, &g_write_failing, "couldn't save settings");
}

// #endregion
