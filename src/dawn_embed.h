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
// The embedder is the /v1/models entry with "text_embeddings" (EmbeddingGemma 2 first, then
// EmbeddingGemma). Pieces are kept inside its context window. When /v1/embeddings turns it away
// (404, or vectors of another length) the worker asks /v1/models again at once, backing off up to
// 5 minutes, and rebuilds only if the embedder really changed. A permanent refusal (401,
// embedding_tokenizer_missing, invalid_dimensions, capability_not_supported) stops indexing until
// the next discovery that succeeds.
//
// What the worker is doing is published for a settings screen: embed_status() (phase, progress,
// model, error), embed_set_enabled() to turn the whole thing off and on, embed_rebuild() to start
// over. None of it draws anything.
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
#define EMBED_ERROR_MAX 64 //!< EmbedStatus.error, bytes including the NUL

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

//! What the background indexer is doing (embed_status()).
typedef enum {
    EMBED_PHASE_OFF, //!< Disabled by settings (embed_set_enabled(false)) or not started
    EMBED_PHASE_DISCOVERING, //!< Asking /v1/models
    EMBED_PHASE_NO_EMBEDDER, //!< No text_embeddings model is installed
    EMBED_PHASE_INDEXING, //!< notes_done of notes_total
    EMBED_PHASE_PAUSED, //!< Waiting for a chat reply to finish
    EMBED_PHASE_WAITING, //!< Retry-After or backoff (or the endpoint not answering); see retry_at_ms
    EMBED_PHASE_IDLE, //!< The index is current
    EMBED_PHASE_FAILED, //!< A permanent error; error holds its code
} EmbedPhase;

//! A snapshot of the indexer for a status line or settings screen.
typedef struct {
    EmbedPhase phase;
    int32_t notes_total; //!< Notes in this pass (0 when not indexing)
    int32_t notes_done; //!< Of those, how many are through
    int32_t notes_indexed; //!< Notes in the store
    char current_title[EMBED_TITLE_MAX]; //!< The note being embedded, "" otherwise
    char model[EMBED_MODEL_MAX]; //!< The embedder's id, "" when there is none
    char error[EMBED_ERROR_MAX]; //!< The error code for EMBED_PHASE_FAILED (e.g. "unauthorized",
                                 //!< "embedding_tokenizer_missing"), "" otherwise
    int64_t retry_at_ms; //!< For EMBED_PHASE_WAITING: when the next try is due, in the backend's
                         //!< DAWN_CLOCK_MS time (CLOCK_MONOTONIC milliseconds); else 0
} EmbedStatus;

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

//! A note's file was renamed (a live title): its index follows it to new_path, pieces and all, so
//! nothing is embedded again and old_path is never offered as a hit. Copied.
void embed_note_renamed(const char* old_path, const char* new_path);

//! Whether meaning features have anything to offer: an embedder exists and at least one note is
//! indexed. When false the UI shows nothing embedding-related.
bool embed_ready(void);

//! Call once per frame: true when a query finished, a failed query may be tried again, the index
//! changed, or the indexer's phase or progress moved since the last call, meaning a caller showing
//! results or status should ask again (and redraw).
bool embed_poll(void);

//! A consistent snapshot of what the indexer is doing. Cheap (one short lock); callable every
//! frame. EMBED_PHASE_OFF before embed_start(), while disabled, and on builds without embeddings.
void embed_status(EmbedStatus* out);

//! Turn meaning features off (false) or back on (true); on by default, and may be called before
//! embed_start(). Off: the worker drops the batch in flight (its HTTP call aborts within about a
//! second; that note is redone later), sends nothing more, and embed_ready(), embed_search() and
//! embed_related() answer as if there were no embedder; the index on disk is kept. On again:
//! discovery and a rescan run at once.
void embed_set_enabled(bool enabled);

//! The switch embed_set_enabled() sets (always false on builds without embeddings).
bool embed_enabled(void);

//! Start over: the worker drops every note from the index, deletes every index file in the cache
//! directory, looks for the embedder again and re-embeds every note. Returns at once.
void embed_rebuild(void);

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
