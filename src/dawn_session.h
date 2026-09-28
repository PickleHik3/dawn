// dawn_session.h - The warm writing session: one long-lived conversation per note (spec §07).
//
// TAI keeps one conversation's KV cache and reuses it only when the next request is the held
// transcript plus exactly one message, with the same system prompt, options and tools
// (LiteRtTaiRuntime.ensureConversationLocked). So everything dawn asks - chat questions,
// rewrites, live titles - goes through one conversation with one system prompt:
//
//   prime    the system prompt plus a note snapshot, as a first turn with a tiny reply, sent once
//            the model is already loaded and the writer has typed for a moment, or when the chat
//            opens (which may load the model);
//   append   every later job is exactly one appended turn: the question plus a compact "since
//            last time" diff of the sections that changed; earlier messages are never edited;
//   watch    the conversation's size is tracked from each response's usage;
//   compact  at 70% of the context window, in a quiet moment (8 s idle, nothing waiting, nothing
//            streaming), a new conversation is built - same system prompt, a fresh snapshot, and
//            a recap (the last two exchanges word for word plus a model-written one-line summary
//            of anything older) - primed, then switched to.
//
// A question always goes first: a quiet job (a live title, the compaction) in flight is cancelled
// on the server and retried later. No request is ever sent that would overflow the window (TAI
// answers overflow with a hard error). Single-threaded: called only from dawn's own thread.

#ifndef DAWN_SESSION_H
#define DAWN_SESSION_H

#include "dawn_types.h"

#if HAS_LIBAI

// #region Lifecycle

//! Once per frame (from ai_tick()): starts what is due - a waiting question, priming, the
//! compaction, keep-warm, the live title - and keeps the header state current.
void session_tick(void);

//! Forget the conversation (the note was closed or another one opened, or the chat was cleared):
//! whatever is in flight is cancelled and the next job starts a fresh conversation.
void session_reset(void);

//! The chat was opened: prime now if the conversation has not read the note yet, loading the
//! model if it has to (the writer asked for the AI, so a load is expected here).
void session_chat_opened(void);

// #endregion

// #region Jobs

//! Ask the user's question (a chat message or a rewrite) through the conversation. The message
//! sent is the question plus what changed in the note since the model last saw it (or a whole
//! snapshot when the conversation is new). cb receives the reply's chunks and then NULL, exactly
//! like ai_generate_response_stream()'s callback; it is called for this question only, even when
//! the question waits behind priming or TAI's "one generation at a time" backoff (1/2/4 s).
//! Only "Error: generation_active" after the last retry reaches cb as an error of that kind.
//! Returns false when nothing could be started (no AI); cb is then never called.
bool session_ask(const char* question, int32_t max_tokens, ai_stream_callback_t cb);

//! Stop the question: a live reply is cancelled on the server (its NULL chunk still arrives), a
//! question still waiting to start ends at once with its NULL chunk.
void session_stop(void);

//! Whether the user's question is the one generating right now (not waiting behind something).
bool session_user_live(void);

//! A quiet job's result: reply is the whole reply text (NULL when it failed or was cancelled).
typedef void (*SessionQuietDone)(const char* reply, void* user_data);

//! Run a quiet job (a live title) as one appended turn: instruction plus the note's changes since
//! last time, with a reply limit of at most 32 tokens. Starts only if the conversation has read the
//! note, the model is loaded right now, no question is waiting and it fits; a question arriving
//! later cancels it on the server (done then gets NULL). Returns false when it did not start.
bool session_quiet(const char* instruction, int32_t max_tokens, SessionQuietDone done, void* user_data);

//! Whether a quiet job could start right now (see session_quiet()), without starting one.
bool session_quiet_ready(void);

// #endregion

// #region What the chat shows

//! When the model started waking for the job in flight (DAWN_CLOCK_MS), or 0 when it isn't.
int64_t session_waking_since(void);

//! The index of the first chat message the model still holds word for word; everything before it
//! is dim scrollback with one faint divider above this one. 0 when the model holds all of it.
int32_t session_held_from(void);

//! Whether the model has now seen the whole note as it is (so replacing the whole note is safe).
bool session_saw_whole_note(void);

//! The note context as a fresh snapshot (title, what is and isn't shown, the snapshot itself), for
//! the read_document tool's "context" action. Caller frees.
char* session_note_context(void);

// #endregion

#endif // HAS_LIBAI

#endif // DAWN_SESSION_H
