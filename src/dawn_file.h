// dawn_file.h - The open note on disk: loading, saving, conflicts, snapshots, renames.

#ifndef DAWN_FILE_H
#define DAWN_FILE_H

#include "dawn_types.h"

// #region History Directory

//! Get path to history/sessions directory
//! Prefers XDG_DATA_HOME/dawn (or ~/.local/share/dawn) on POSIX systems.
//! Falls back to ~/.dawn if that legacy path already exists, for
//! backwards compatibility with installs that predate XDG support.
//! @return static buffer with the resolved path; "" when the path does not fit PATH_MAX
//!         (path_join() then refuses it, so nothing is written to a cut-short path)
char* history_dir(void);

//! Get path to configuration directory
//! Uses XDG_CONFIG_HOME/dawn (or ~/.config/dawn) on POSIX systems,
//! and %APPDATA%\dawn on Windows.
//! @return static buffer with the resolved path; "" when it does not fit PATH_MAX
char* config_dir(void);

// #endregion

// #region Storage Helpers

//! "<dir><sep><name>" into out.
//! @return false when dir is empty or the result does not fit out (out is then "")
bool path_join(char* out, size_t out_size, const char* dir, const char* name);

//! The whole file, NUL-terminated. *missing is set when the file does not exist, as opposed to
//! one that could not be read. dawn_fsio where it is built (errno is then reliable), the backend
//! elsewhere (any failure then counts as missing, the old behaviour).
//! @return malloc'd bytes, or NULL
char* store_read(const char* path, size_t* out_len, bool* missing);

//! Move from to to, never over an existing to (errno EEXIST when it exists).
bool store_move_no_replace(const char* from, const char* to);

//! Set a damaged file aside as <path>.corrupt-<UTC time> (never over anything); the new name goes
//! to out. Used instead of overwriting a file dawn could not parse.
bool store_quarantine(const char* path, char* out, size_t out_size);

//! 64-bit FNV-1a, the fingerprint the open note's file is compared by.
uint64_t store_hash(const void* data, size_t len);

// #endregion

// #region Session Persistence

//! Save current session to disk
//! Writes the note only when it changed since the last successful save (app.dirty), with YAML
//! frontmatter when the note carries one (app.write_fm); always updates the history entry and
//! the companion .chat.json file.
//! Before writing, the file is compared with what dawn last saw there (app.disk_hash):
//! - unchanged: written; the first such write in an editing session, and one every 30 minutes
//!   after, first copies the file as it is on disk into versions/ (a snapshot);
//! - deleted elsewhere: written again, with a notice;
//! - changed elsewhere (a conflict): NOT written. The text goes to a copy beside the note
//!   (<stem>.conflict-<time>.md, refreshed when the text changes), app.save_paused is set and
//!   app.conflict_prompt asks dawn.c to open MODE_CONFLICT.
//! @return false when the note was not written (a failure: app.save_failed; or a conflict:
//!         app.save_paused)
bool save_session(void);

//! Keep a copy of a note's text under <data dir>/versions/<note name>/<UTC time>.md: the buffer
//! before an AI edit (Ctrl+Z reaches only so far), or the file's bytes before dawn writes over
//! them. The newest MAX_NOTE_VERSIONS per note are kept. <note name> carries -<8 hex> of the
//! note's path hash for a note outside the notes directory.
//! @param content the bytes to keep, or NULL for the note as it is in the editor now
//! @param len their length (ignored for NULL)
//! @param out receives the copy's path
//! @param out_size size of out
//! @return false when there is nothing to keep or no path, or the copy could not be written
bool save_note_version(const char* content, size_t len, char* out, size_t out_size);

//! A note was just started with no file behind it yet (a new note, or a name that is not a file):
//! dawn expects none on disk, and any conflict or snapshot state of the previous note is dropped.
void note_disk_begin_absent(void);

//! Look at the open note's file every two seconds (a stat, read and hashed only when its stamp
//! moved). When it changed elsewhere and the note has no unsaved edits, it is reloaded as one
//! undo step with a notice. Call once per frame while the note is on screen.
void note_watch(void);

//! MODE_CONFLICT's "Reload theirs": the file's text replaces the editor's as one undo step (Ctrl+Z
//! brings the writer's back), and saving resumes.
//! @return false when the file could not be read (saving stays paused)
bool note_conflict_reload(void);

//! MODE_CONFLICT's "Overwrite with mine": the editor's text is written over the file, whose bytes
//! are kept in versions/ first, and saving resumes.
//! @return false when the write failed or the file changed yet again
bool note_conflict_overwrite(void);

//! While saving is paused: whether the editor's text differs from what the conflict copy holds,
//! so the next autosave should look at the file again (and refresh the copy, or ask again).
bool note_conflict_text_changed(void);

//! The writer left the note (Esc, the timer) while saving is paused: say where their text is.
void note_conflict_left(void);

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
//! A file holding a NUL byte is not text: it is refused (with an error notice) rather than opened
//! cut short at the NUL, which the next save would have written back.
//! @param path path to the .md file to open
//! @return false when the file could not be read or is not text; the editor is then left as it was
bool load_file_for_editing(const char* path);

//! Whether the last load_file_for_editing() refused the file for holding NUL bytes.
bool load_refused_binary(void);

//! Load content from buffer for editing, parsing frontmatter
//! @param content buffer containing markdown content
//! @param size size of content buffer
void load_buffer_for_editing(const char* content, size_t size);

//! Whether path is inside dawn's own notes directory (history_dir()).
bool note_in_history_dir(const char* path);

//! Rename the open note's file to <its directory>/<stem>.md, or <stem>-2.md, -3.md… when that name
//! is taken: never over an existing file. Its .chat.json moves with it, the history forgets the old
//! name and app.session_path becomes the new one.
//! @return the new path (caller frees), or NULL when it was not renamed
char* note_rename(const char* stem);

//! Rename the open note's file back to exactly path (an undo), unless something is there now.
//! @return false when it was not renamed
bool note_rename_to(const char* path);

//! The open note's file was just renamed to path: move its .chat.json, drop the old history
//! entry, and make path app.session_path.
void note_moved(const char* path);

//! Reveal a file in system file manager
//! @param path path to the file to reveal
void open_in_finder(const char* path);

// #endregion

#endif // DAWN_FILE_H
