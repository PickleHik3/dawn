// dawn_file.h

#ifndef DAWN_FILE_H
#define DAWN_FILE_H

#include "dawn_types.h"

// #region History Directory

//! Get path to history/sessions directory
//! Prefers XDG_DATA_HOME/dawn (or ~/.local/share/dawn) on POSIX systems.
//! Falls back to ~/.dawn if that legacy path already exists, for
//! backwards compatibility with installs that predate XDG support.
//! @return static buffer with the resolved path
char* history_dir(void);

//! Get path to configuration directory
//! Uses XDG_CONFIG_HOME/dawn (or ~/.config/dawn) on POSIX systems,
//! and %APPDATA%\dawn on Windows.
//! @return static buffer with the resolved path
char* config_dir(void);

// #endregion

// #region Session Persistence

//! Save current session to disk
//! Writes the note only when it changed since the last successful save (app.dirty), with YAML
//! frontmatter when the note carries one (app.write_fm); always updates the history entry and
//! the companion .chat.json file.
//! @return false when the note could not be written; app.save_failed then stays set until it is
bool save_session(void);

//! Keep a copy of the note as it is now, under <data dir>/versions/<note name>/<UTC time>.md,
//! for the AI's edits: Ctrl+Z reaches them only so far. The newest MAX_NOTE_VERSIONS per note
//! are kept.
//! @param out receives the copy's path
//! @param out_size size of out
//! @return false when the note is empty or has no path, or the copy could not be written
bool save_note_version(char* out, size_t out_size);

//! Drop the undo history and start it again from the current text (dawn.c)
void undo_reset(void);

//! Load list of past sessions from history directory
//! Populates app.history array, sorted newest first
void load_history(void);

//! Load AI chat history for a session
//! @param session_path path to the .md session file
void load_chat_history(const char* session_path);

// #endregion

// #region File Operations

//! path made absolute: its directory resolved through realpath, its file name kept as given.
//! History entries hold this form, so they open from any working directory.
//! @param path path to a file, which need not exist yet
//! @return newly allocated path, or NULL when the directory it names does not exist
char* note_path_for(const char* path);

//! Load a file for editing, parsing frontmatter
//! @param path path to the .md file to open
//! @return false when the file could not be read; the editor is then left as it was
bool load_file_for_editing(const char* path);

//! Load content from buffer for editing, parsing frontmatter
//! @param content buffer containing markdown content
//! @param size size of content buffer
void load_buffer_for_editing(const char* content, size_t size);

//! Reveal a file in system file manager
//! @param path path to the file to reveal
void open_in_finder(const char* path);

// #endregion

#endif // DAWN_FILE_H
