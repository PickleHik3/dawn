// dawn_dictate.h - Dictation that glows as it lands (spec §09, dictation marks protocol v1.1).
//
// The launcher's dictation tags its text with OSC 7727 marks (parsed by the backend into
// DawnDictEvents). This module inserts each phrase at the cursor as one undo step, remembers
// its span, draws it in primary settling to ink over about a second, replaces it in place when a
// polished wording follows (if the span is untouched), and shows "listening" plus a shimmer at
// the insertion point while the mic is open. Single-threaded.

#ifndef DAWN_DICTATE_H
#define DAWN_DICTATE_H

#include "dawn_voice.h"

//! Drain every pending dictation event from the backend and act on it.
void dictate_drain(void);

//! Act on one event (takes ownership of ev->text). Exposed for dictate_drain() and for tests.
void dictate_event(DawnDictEvent* ev);

//! Advance timers for this frame (now: DAWN_CLOCK_MS): the glow, and the listening state's
//! timeout (an `end` can be missed). Returns true while anything is animating.
bool dictate_tick(int64_t now_ms);

//! Whether the mic is open (between `listen` and `end`, or the timeout).
bool dictate_listening(void);

//! The glow on a phrase that just landed, or the shimmer on the bytes just after the cursor.
bool dictate_style_at(size_t pos, VoiceStyle* out);

//! Paint the shimmer into the blank cells right of the cursor (see voice_draw_overlay).
void dictate_draw_overlay(int32_t row, int32_t col, int32_t cols_free);

#endif // DAWN_DICTATE_H
