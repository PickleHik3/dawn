// dawn_tips.c - the set of one-shot tips the writer has seen, and its JSON form in settings.json.
//
// dawn gives a few tips once each, at the moment they apply: the first selection and the first
// save, each as a NOTICE_TIP that takes the writing screen's whole status line (dawn.c's
// tip_show(); a tip counts as given only once that line drew it). This file only remembers which
// were given. settings.json holds them as
// "tips_seen": ["first_save", ...], by name, so the order of TipId may change but a shipped name
// never does. Names it does not know (written by a newer dawn) are carried through a save.
// Pure: no app, no backend, no notices; dawn_settings.c reads and writes the array.

#include "dawn_tips.h"
#include <string.h>

//! settings.json names, by TipId; never renamed once shipped
static const char* const TIP_NAMES[TIP_COUNT] = {
    [TIP_FIRST_SELECT] = "first_select",
    [TIP_FIRST_SAVE] = "first_save",
};

static bool g_seen[TIP_COUNT];

static bool tip_valid(TipId id)
{
    return (int32_t)id >= 0 && id < TIP_COUNT;
}

bool tips_seen(TipId id)
{
    return tip_valid(id) && g_seen[id];
}

void tips_mark(TipId id)
{
    if (tip_valid(id))
        g_seen[id] = true;
}

void tips_reset(void)
{
    for (int32_t i = 0; i < TIP_COUNT; i++)
        g_seen[i] = false;
}

const char* tips_name(TipId id)
{
    return tip_valid(id) ? TIP_NAMES[id] : NULL;
}

TipId tips_from_name(const char* name)
{
    if (!name)
        return TIP_COUNT;
    for (int32_t i = 0; i < TIP_COUNT; i++) {
        if (strcmp(TIP_NAMES[i], name) == 0)
            return (TipId)i;
    }
    return TIP_COUNT;
}

void tips_load_json(const cJSON* array)
{
    if (!cJSON_IsArray(array))
        return;
    const cJSON* item;
    cJSON_ArrayForEach(item, array)
    {
        if (cJSON_IsString(item))
            tips_mark(tips_from_name(item->valuestring));
    }
}

//! Whether array already holds the string name
static bool json_has_name(const cJSON* array, const char* name)
{
    const cJSON* item;
    cJSON_ArrayForEach(item, array)
    {
        if (cJSON_IsString(item) && strcmp(item->valuestring, name) == 0)
            return true;
    }
    return false;
}

//! Append the string name to array
//! @return false when out of memory
static bool json_add_name(cJSON* array, const char* name)
{
    cJSON* s = cJSON_CreateString(name);
    if (!s)
        return false;
    if (!cJSON_AddItemToArray(array, s)) {
        cJSON_Delete(s);
        return false;
    }
    return true;
}

cJSON* tips_save_json(const cJSON* old)
{
    cJSON* out = cJSON_CreateArray();
    if (!out)
        return NULL;
    for (int32_t i = 0; i < TIP_COUNT; i++) {
        if (g_seen[i] && !json_add_name(out, TIP_NAMES[i])) {
            cJSON_Delete(out);
            return NULL;
        }
    }
    if (cJSON_IsArray(old)) {
        const cJSON* item;
        cJSON_ArrayForEach(item, old)
        {
            if (!cJSON_IsString(item) || tips_from_name(item->valuestring) != TIP_COUNT
                || json_has_name(out, item->valuestring))
                continue;
            if (!json_add_name(out, item->valuestring)) {
                cJSON_Delete(out);
                return NULL;
            }
        }
    }
    return out;
}
