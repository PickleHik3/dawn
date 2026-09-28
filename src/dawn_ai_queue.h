// dawn_ai_queue.h - One AI job at a time, in two lanes: USER (chat, rewrites) and QUIET (title).
//
// Pure policy/state, single-threaded (called only from dawn's own thread, like the rest of
// dawn_chat.c): it decides who may run and remembers what happened, but never itself touches a
// session or a stream. dawn_chat.c still holds every session/stream id (g_title_session and
// app.ai_session/app.ai_stream) and does the actual starting, cancelling and retrying; it just
// asks this module what it is and isn't allowed to do right now.

#ifndef DAWN_AI_QUEUE_H
#define DAWN_AI_QUEUE_H

#include "dawn_types.h"

#if HAS_LIBAI

typedef enum { AI_LANE_NONE, AI_LANE_USER, AI_LANE_QUIET } AiLane;

//! The lane generating right now, or AI_LANE_NONE when idle. The single source of truth for
//! "is anything running": dawn_chat.c reports every start and end through ai_queue_set_lane().
AiLane ai_queue_active_lane(void);

//! Report that lane started (or, with AI_LANE_NONE, that whatever was running just ended).
void ai_queue_set_lane(AiLane lane);

//! Whether the QUIET lane is the one running now: a USER job about to start must cancel it (the
//! caller holds the stream id and does the cancelling itself, and is responsible for making sure
//! its own retry logic treats that cancellation as a displacement, not a real failure).
bool ai_queue_quiet_is_running(void);

//! Whether a QUIET (title) job may start right now: the USER lane must be idle, and TAI must
//! already have a model resident — a QUIET job never triggers a load. An unknown runtime state
//! (the endpoint didn't answer, or its shape wasn't recognized) counts as "not loaded".
bool ai_queue_quiet_may_start(void);

//! Call once at the start of every new user-initiated turn (a chat question or a rewrite), before
//! the first request goes out.
void ai_queue_user_reset(void);

//! Call when a USER-lane request comes back with TAI's "generation_active" (409, one generation
//! at a time). Returns the delay in milliseconds before the same request should be retried (1000,
//! 2000, then 4000), or -1 once three retries are used up, meaning give up this turn.
int32_t ai_queue_user_busy(void);

#endif // HAS_LIBAI

#endif // DAWN_AI_QUEUE_H
