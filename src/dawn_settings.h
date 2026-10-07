// dawn_settings.h - Persisted user preferences

#ifndef DAWN_SETTINGS_H
#define DAWN_SETTINGS_H

// #region Lifecycle

//! Load persisted settings into app state.
//! Reads <config_dir>/settings.json if present and applies any
//! recognized keys (theme, timer_mins, nerd_font, meaning_index) on top of existing defaults.
//! Missing or malformed files leave the defaults in place. Applying meaning_index
//! (embed_set_enabled) is the caller's job.
void settings_load(void);

//! Persist the current app preferences.
//! Writes theme, timer_mins and meaning_index to <config_dir>/settings.json, keeping every
//! other key the file holds.
//! Creates the config directory if it does not exist.
void settings_save(void);

// #endregion

#endif // DAWN_SETTINGS_H
