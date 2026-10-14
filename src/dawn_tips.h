// dawn_tips.h - One-shot tips: which ones the writer has seen, kept in settings.json

#ifndef DAWN_TIPS_H
#define DAWN_TIPS_H

#include "cJSON.h"
#include <stdbool.h>
#include <stdint.h>

//! A tip dawn gives once, at the moment it applies. Each has a name in settings.json
//! (tips_name()); a name never changes once shipped, and a new tip goes before TIP_COUNT.
typedef enum {
    TIP_FIRST_SELECT, //!< "first_select": a selection first exists on the writing screen
    TIP_FIRST_SAVE, //!< "first_save": the first save of a note with text
    TIP_FIRST_BACK, //!< "first_back": esc first leaves a note for the start screen
    TIP_FIRST_MARKDOWN, //!< "first_markdown": the writer first starts a line with "# ", "- " or "* "
    TIP_COUNT
} TipId;

//! Whether the tip was given already (or read as seen from settings.json)
bool tips_seen(TipId id);

//! Remember the tip as given. Saving it is the caller's job (settings_save()).
void tips_mark(TipId id);

//! Forget every tip seen, as on a first run.
void tips_reset(void);

//! The tip's name in settings.json ("first_select"), or NULL for an id out of range
const char* tips_name(TipId id);

//! The tip a name stands for, or TIP_COUNT when the name is unknown
TipId tips_from_name(const char* name);

//! Mark seen every tip named in array, settings.json's "tips_seen". Names this dawn does not know
//! and entries that are not strings are skipped; anything but an array marks nothing.
void tips_load_json(const cJSON* array);

//! The "tips_seen" array to write: every tip seen, by name in TipId order, then the names in old
//! (the array the file held, or NULL) this dawn does not know, so a newer dawn's tips survive an
//! older one's save. NULL when out of memory.
cJSON* tips_save_json(const cJSON* old);

#endif // DAWN_TIPS_H
