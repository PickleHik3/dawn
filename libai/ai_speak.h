/**
 * @file ai_speak.h
 * @brief Read-aloud over Termux Launcher's POST /v1/ai/speak, one sentence per request.
 *
 * Built with ai_bridge_openai.c (every platform but macOS). The caller hands over the whole list
 * of sentences at once; a detached worker thread sends them one at a time, each request returning
 * only once the phone has finished saying it, so the sentence being spoken is always known
 * exactly. Everything here is safe to call from dawn's thread while the worker runs: the worker
 * only ever touches its own copy of the sentences and the small status block behind a mutex.
 */

#ifndef AI_SPEAK_H
#define AI_SPEAK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Where a read-aloud run is.
typedef enum {
    AI_SPEAK_IDLE, //!< Nothing started, or the last run's end was acknowledged
    AI_SPEAK_RUNNING, //!< A sentence is out (sent, being synthesised or playing)
    AI_SPEAK_DONE, //!< Every sentence was spoken
    AI_SPEAK_STOPPED, //!< ai_speak_stop(), or the phone reported it was stopped
    AI_SPEAK_FAILED, //!< A request failed; error says why
} ai_speak_phase_t;

//! A snapshot of the run for the frame loop.
typedef struct {
    ai_speak_phase_t phase;
    int32_t index; //!< The sentence out now (RUNNING), or the last one sent
    int64_t elapsed_ms; //!< Since that sentence's request went out
    double sec_per_byte; //!< Measured on the last finished sentence (audioSeconds / bytes), 0 = none yet
    int32_t lead_in_ms; //!< The last finished sentence's firstSoundMs, -1 = none yet
    char error[128]; //!< FAILED only: a short, lower-case reason
} ai_speak_status_t;

//! Start speaking count sentences (texts[i], lens[i] bytes, UTF-8, no NUL needed). Everything is
//! copied. A run already going is stopped first. Returns false when nothing could be started
//! (no memory, no thread); the endpoint itself is only looked up on the worker, so a missing
//! launcher shows up later as AI_SPEAK_FAILED.
bool ai_speak_start(const char* const* texts, const size_t* lens, int32_t count);

//! Stop the run: the request in flight is abandoned and POST /v1/ai/speak/stop goes out
//! (fire-and-forget, on its own thread) so the phone falls silent now. Never blocks.
void ai_speak_stop(void);

//! ai_speak_stop() for when dawn is exiting: the stop request goes out on the calling thread
//! (at most ~2 s, and only when something is being spoken), since a detached thread would die
//! with the process before the phone heard it.
void ai_speak_shutdown(void);

//! The run's current state. Cheap; call it every frame.
void ai_speak_status(ai_speak_status_t* out);

//! Forget a finished run (DONE, STOPPED or FAILED): the phase goes back to IDLE.
void ai_speak_ack(void);

//! The launcher endpoint ai_bridge_openai.c uses for the chat (TAI only: base URL ending in /v1,
//! no trailing slash, and the optional bearer token), both malloc'd for the caller. On failure
//! returns false and, when error is non-NULL, a malloc'd reason (or NULL).
bool ai_bridge_endpoint(char** base_url, char** api_key, char** error);

#ifdef __cplusplus
}
#endif

#endif // AI_SPEAK_H
