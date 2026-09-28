// dawn_embed.h - Meaning: an embedding index over every note, kept fresh in the background.
//
// One background worker thread indexes every note in dawn's notes directory (history_dir()) plus
// any extra paths it is given: it cuts each note into pieces (dawn_embed_index.c), asks the
// launcher's embedder for their vectors (libai/ai_embed.c) and keeps one small index file per note
// under $XDG_CACHE_HOME/dawn/embed (or ~/.cache/dawn/embed). After the first pass only pieces
// whose text changed are embedded again. The worker pauses while a chat reply is generating,
// follows Retry-After on 429/503, and does nothing at all until an embedder exists: then every
// call here reports "unavailable" and the UI must show nothing (spec: "until a model exists,
// nothing changes and nothing mentions it").
//
// Everything below is called from dawn's own thread. Search queries are embedded on a short-lived
// thread of their own, so a call never blocks: it answers EMBED_PENDING, and embed_poll() says
// when to ask again.
//
// On builds without the OpenAI-compatible libai (web, Windows, macOS, or USE_LIBAI off) every
// function exists and does nothing: see DAWN_EMBED_LIVE.

#ifndef DAWN_EMBED_H
#define DAWN_EMBED_H

#include "dawn_embed_index.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(USE_LIBAI) && !defined(__APPLE__) && !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#define DAWN_EMBED_LIVE 1
#else
#define DAWN_EMBED_LIVE 0
#endif

#define EMBED_HITS_MAX 64 //!< Most hits one call returns
#define EMBED_SEARCH_MIN_SCORE 0.35f //!< Suggested floor for Ctrl+S's "by meaning" group
#define EMBED_RELATED_MIN_SCORE 0.60f //!< Suggested floor for the chat's "also in:" line

// #region Types

typedef enum {
    EMBED_UNAVAILABLE, //!< No embedder, nothing indexed yet, or the query failed: show nothing
    EMBED_PENDING, //!< The query is being embedded; ask again once embed_poll() returns true
    EMBED_READY, //!< The hits are filled (possibly none)
} EmbedState;

//! One piece that matched, best first.
typedef struct {
    char path[EMBED_PATH_MAX]; //!< The note it is in
    char note_title[EMBED_TITLE_MAX]; //!< That note's title
    char heading[EMBED_HEADING_MAX]; //!< The heading the piece sits under, "" when none
    uint32_t start; //!< Byte range in the note's body (its text without frontmatter, LF endings:
    uint32_t len; //!< what app.text holds once the note is loaded)
    uint64_t text_hash; //!< embed_hash() of the piece when it was indexed; see embed_hit_matches()
    float score; //!< Cosine similarity, -1..1
} EmbedHit;

//! How embed_search() filters and groups.
typedef struct {
    const char* only_path; //!< Only this note's pieces, or NULL for every note
    const char* exclude_path; //!< Never this note (e.g. the open one), or NULL
    float min_score; //!< Drop hits below this (EMBED_SEARCH_MIN_SCORE; -1 keeps everything)
    bool one_per_note; //!< Keep only each note's best piece
} EmbedSearchOpts;

// #endregion

// #region Lifecycle

//! Start the background indexer, once (later calls do nothing). notes_dir is history_dir();
//! extra_paths are notes that live elsewhere (app.history's paths are the natural list), copied.
//! Nothing is sent anywhere until an embedder is found.
void embed_start(const char* notes_dir, const char* const* extra_paths, int32_t extra_count);

//! Stop the worker and any query in flight (their HTTP calls abort within about a second) and
//! free the index. Waits at most about a second; call on exit.
void embed_shutdown(void);

//! The open note's text changed (call after a save, or when the note is switched away from):
//! body is the text without frontmatter, as app.text holds it; title is the note's title or NULL.
//! Copied. The worker waits until the note has been still for a few seconds, then re-embeds only
//! the pieces whose text changed. A later call for the same path replaces an earlier one.
void embed_note_changed(const char* path, const char* title, const char* body, size_t len);

//! Whether meaning features have anything to offer: an embedder exists and at least one note is
//! indexed. When false the UI shows nothing embedding-related.
bool embed_ready(void);

//! Call once per frame: true when a query finished or the index changed since the last call,
//! meaning a caller showing results should ask again (and redraw).
bool embed_poll(void);

// #endregion

// #region Queries

//! Pieces most similar in meaning to query, best first. The first call for a new query text
//! returns EMBED_PENDING and embeds it in the background (typing a new query replaces one not yet
//! sent); once embed_poll() returns true, the same call returns EMBED_READY. Recent query vectors
//! are cached, so asking again with the same text is cheap. opts may be NULL (every note, no
//! floor, every piece). *count gets the number of hits written, at most max (<= EMBED_HITS_MAX).
EmbedState embed_search(const char* query, size_t len, const EmbedSearchOpts* opts, EmbedHit* hits,
    int32_t max, int32_t* count);

//! The pieces of one note ranked by relevance to query, with byte ranges, for the AI snapshot
//! builder: embed_search() restricted to path, with no floor. Same PENDING/READY protocol.
EmbedState embed_relevant(const char* path, const char* query, size_t len, EmbedHit* hits, int32_t max,
    int32_t* count);

//! Other notes that cover the same ground as the note at path (whole-note similarity), best
//! first, scoring at least min_score (EMBED_RELATED_MIN_SCORE). Each hit's range and heading name
//! the piece of that note closest to this one. Needs no query, so it never waits; the answer is
//! cached until the index changes, so calling it every frame is fine. Returns the count.
int32_t embed_related(const char* path, float min_score, EmbedHit* out, int32_t max);

//! Whether the text now at a hit's range is still what was indexed: slice is the current body's
//! bytes [hit->start, hit->start + hit->len). The caller checks the range against the body's
//! length first. A piece that no longer matches is stale (the note was edited since).
bool embed_hit_matches(const EmbedHit* hit, const char* slice, size_t slice_len);

// #endregion

#endif // DAWN_EMBED_H
