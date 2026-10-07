// dawn_notepath.h - Pure naming rules for the files dawn keeps beside a note.
//
// Every name dawn makes for a note's companions is built here, without touching the disk, the app
// global or the backend, so tests/test_store.c can check them alone:
// - a new note is named by the time it was started (2026-10-07_143005.md); when that file exists
//   already the next free one is 2026-10-07_143005-2.md, -3 and so on (notepath_numbered);
// - a copy of the writer's text saved when the note changed elsewhere sits beside the note as
//   <stem>.conflict-<YYYYmmdd-HHMMSS>.md, again with -2, -3 when taken (notepath_conflict);
// - a note's snapshots live in versions/<stem>, plus -<8 hex of the note's path hash> for a note
//   outside dawn's notes directory, so two notes of the same name elsewhere never share one
//   (notepath_version_dir).
// Each builder returns false instead of a cut-short path.

#ifndef DAWN_NOTEPATH_H
#define DAWN_NOTEPATH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NOTEPATH_MAX_NUMBER 99 //!< Highest -N tried before a name counts as taken for good

//! The file name part of path (after its last '/' or '\\'), and through stem_len its length
//! without a final ".md".
const char* notepath_base(const char* path, size_t* stem_len);

//! "<dir><sep><stem>.md" for n <= 1, else "<dir><sep><stem>-<n>.md". dir is dir_len bytes (it
//! need not be NUL-terminated there); an empty dir gives "<stem>.md".
//! @return false when it does not fit out
bool notepath_numbered(char* out, size_t out_size, const char* dir, size_t dir_len, char sep,
    const char* stem, int32_t n);

//! The conflict copy for note_path: "<dir>/<stem>.conflict-<stamp>.md", or "...-<n>.md" for n > 1,
//! where stem is the note's file name without ".md" and stamp is YYYYmmdd-HHMMSS.
//! @return false when it does not fit out or note_path has no file name
bool notepath_conflict(char* out, size_t out_size, const char* note_path, const char* stamp, int32_t n);

//! The versions directory name (not the full path) for note_path: its file name without ".md",
//! with anything unsafe in a directory name made '_' ("note" when nothing is left), and, when
//! in_notes_dir is false, "-" and the low 32 bits of path_hash in 8 lower-case hex digits.
//! @return false when it does not fit out (out_size of 160 always fits)
bool notepath_version_dir(char* out, size_t out_size, const char* note_path, bool in_notes_dir,
    uint64_t path_hash);

//! Whether the n bytes at base are a name dawn gives a new note: YYYY-MM-DD_HHMMSS, optionally
//! followed by -2 .. -99 (notepath_numbered's suffix).
bool notepath_is_stamp_name(const char* base, size_t n);

#endif // DAWN_NOTEPATH_H
