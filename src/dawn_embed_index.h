// dawn_embed_index.h - The pure half of the meaning index: chunking, hashing, the on-disk index
// file and cosine ranking.
//
// Nothing here touches dawn's globals, the backend, the network or a thread, so it builds on every
// platform and is what tests/test_embed.c exercises. dawn_embed.c (the background indexer) is the
// only other user.
//
// Byte ranges everywhere are offsets into a note's *body*: the file's text with its frontmatter
// removed and its line endings normalized to LF, which is exactly what app.text holds once dawn
// has loaded the note (load_content() in dawn_file.c does the same two steps).

#ifndef DAWN_EMBED_INDEX_H
#define DAWN_EMBED_INDEX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// #region Limits

#define EMBED_INDEX_VERSION 1 //!< Bumped whenever the file layout changes; older files are rebuilt
#define EMBED_MAX_DIMS 4096 //!< Largest vector a file may hold
#define EMBED_MAX_PIECES 4096 //!< Pieces per note; the rest of a larger note is not indexed
#define EMBED_MODEL_MAX 128 //!< Model id, bytes including the NUL
#define EMBED_REVISION_MAX 64 //!< `_revision`, bytes including the NUL
#define EMBED_PATH_MAX 1024 //!< Note path, bytes including the NUL; longer paths are not indexed
#define EMBED_TITLE_MAX 160 //!< Note title, bytes including the NUL
#define EMBED_HEADING_MAX 160 //!< A piece's heading, bytes including the NUL
#define EMBED_FILE_MAX (32u << 20) //!< Largest index file read back; anything bigger is discarded
#define EMBED_NOTE_MAX (8u << 20) //!< Largest note indexed, in bytes

#define EMBED_TARGET_TOKENS 400 //!< Pieces grow up to about this many tokens
#define EMBED_MAX_TOKENS 480 //!< A paragraph above this is split at sentence boundaries

// #endregion

// #region Text

//! FNV-1a, 64-bit: the hash of a piece's text, of a whole body and of a note path.
uint64_t embed_hash(const void* data, size_t len);

//! Where the body starts in a note file's text: past a `---` frontmatter block and the one newline
//! after it, or 0 when there is none. Same delimiter rules as fm_parse() (dawn_fm.c), without
//! checking the YAML itself.
size_t embed_body_offset(const char* text, size_t len);

//! CRLF and lone CR to LF, in place, as normalize_line_endings() does. Returns the new length.
size_t embed_normalize_newlines(char* buf, size_t len);

//! The note's title: the frontmatter's `title:` when text (the whole file) has one, else the
//! body's first `# ` heading, else the file name without its directory and `.md`. Always
//! NUL-terminates out, cut at a UTF-8 boundary.
void embed_note_title(const char* text, size_t len, const char* path, char* out, size_t out_size);

//! Roughly how many tokens text costs: chars/3.6 for mostly-Latin text, chars/2.5 when it is
//! mostly Arabic script (the same estimate as dawn_ai_tokens.c). 0 for empty text.
int32_t embed_estimate_tokens(const char* text, size_t len);

// #endregion

// #region Chunking

//! One piece of a note: a byte range of the body and what it is filed under.
typedef struct {
    uint32_t start; //!< Offset of the piece in the body
    uint32_t len; //!< Its length in bytes
    uint64_t text_hash; //!< embed_hash() of those bytes
    char heading[EMBED_HEADING_MAX]; //!< The nearest heading above it, "" when there is none
} EmbedChunk;

//! Split a body into pieces, markdown-aware: a heading always starts a new piece (and is part of
//! it); small paragraphs merge until about EMBED_TARGET_TOKENS; a paragraph longer than
//! EMBED_MAX_TOKENS is split at sentence ends (and, failing that, at a space); a fenced code block
//! is never split, however long. Pieces are trimmed of surrounding blank space, and a piece that
//! holds nothing but a heading is dropped. token_scale multiplies embed_estimate_tokens() (1.0
//! when uncalibrated; the model's real count over the estimate otherwise). Returns the number of
//! pieces written, at most max.
int32_t embed_chunk(const char* body, size_t len, float token_scale, EmbedChunk* out, int32_t max);

// #endregion

// #region Index file

//! One note's index, as kept on disk and in memory.
typedef struct {
    char model[EMBED_MODEL_MAX]; //!< Embedder id the vectors came from
    char revision[EMBED_REVISION_MAX]; //!< Its `_revision`; a different one means rebuild
    int32_t dims; //!< Values per vector
    char path[EMBED_PATH_MAX]; //!< The note's path, as dawn names it
    char title[EMBED_TITLE_MAX]; //!< The note's title when it was indexed
    uint64_t body_hash; //!< embed_hash() of the whole body
    uint32_t body_len; //!< Length of the body; every piece lies inside it
    int64_t mtime; //!< The file's modification time when indexed (0 for live text)
    uint64_t size; //!< The file's size when indexed
    int32_t count; //!< Pieces
    EmbedChunk* chunks; //!< count pieces (malloc'd)
    float* vectors; //!< count * dims values, piece by piece, unit length (malloc'd)
} EmbedIndex;

//! Free an index's arrays and zero it.
void embed_index_free(EmbedIndex* idx);

//! Serialize an index: a versioned little-endian header, the pieces with their vectors, and a
//! trailing checksum. *out is malloc'd; caller frees. Returns false when the index is out of
//! bounds (dims, count, string lengths) or memory runs out.
bool embed_index_encode(const EmbedIndex* idx, uint8_t** out, size_t* out_len);

//! Parse what embed_index_encode() wrote. Every length is checked against the limits above and
//! against the bytes actually present, and the checksum must match; anything else (truncation,
//! corruption, another version) returns false and leaves *out zeroed.
bool embed_index_decode(const uint8_t* data, size_t len, EmbedIndex* out);

//! Write an index to file_path atomically: a sibling temp file `<file_path>.<pid>-<n>.tmp`,
//! unique to this process and call, then a rename over the old one. Not synced: it is a cache.
bool embed_index_write(const char* file_path, const EmbedIndex* idx);

//! Read an index back. False when the file is missing, too big or fails embed_index_decode().
bool embed_index_read(const char* file_path, EmbedIndex* out);

//! The index file's name for a note: 16 hex digits of embed_hash(note_path), then ".idx".
void embed_index_file_name(const char* note_path, char* out, size_t out_size);

// #endregion

// #region Ranking

//! Dot product; the cosine for unit-length vectors, which is what the index holds.
float embed_dot(const float* a, const float* b, int32_t dims);

//! Scale v to unit length (left alone when it is all zeros).
void embed_normalize(float* v, int32_t dims);

//! One candidate in a top-k list.
typedef struct {
    int32_t note; //!< Caller's note index
    int32_t chunk; //!< Caller's piece index
    float score;
} EmbedScored;

//! Offer a candidate to a top-k list kept sorted by score, best first. *count is the list's
//! current length (start at 0) and k its capacity; a candidate no better than the k-th is dropped.
void embed_topk_push(EmbedScored* top, int32_t* count, int32_t k, EmbedScored cand);

// #endregion

#endif // DAWN_EMBED_INDEX_H
