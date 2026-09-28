// dawn_voice.h - The voice half of the launcher's helpers: read-aloud (dawn_speak) and
// dictation that glows as it lands (dawn_dictate), behind the handful of calls dawn.c makes.
//
// Everything here runs on dawn's own thread. Read-aloud's network work lives in
// libai/ai_speak.c's worker; dictation arrives from the backend as DAWN_KEY_DICTATION events.
// Nothing draws a control of its own: the only marks on the page are a highlight on the sentence
// being read, a glow on a phrase that just landed, a shimmer at the insertion point while the mic
// is open, and one dim word in the status line. Focus mode silences the glow, shimmer and word.

#ifndef DAWN_VOICE_H
#define DAWN_VOICE_H

#include "dawn_types.h"

//! A colour override for one byte of the note. has_fg/has_bg say which fields apply.
typedef struct {
    bool has_fg;
    bool has_bg;
    DawnColor fg;
    DawnColor bg;
} VoiceStyle;

//! Offer a key to the voice helpers before dawn handles it. Returns true when the key was
//! consumed and dawn must not act on it:
//! - DAWN_KEY_DICTATION: drains the backend's dictation events (always consumed);
//! - Ctrl+Q (17) in the note: starts reading aloud from the cursor, or the selection;
//! - while reading: Esc or Ctrl+Q stop it (consumed); any other key or tap stops it and then
//!   goes on to dawn as usual. Scrolling and drag/release don't stop it.
bool voice_handle_key(int32_t key);

//! Once per frame, before rendering: advances read-aloud and the dictation glow. Returns true
//! while something on screen is animating (the caller may use it to keep frames coming).
bool voice_tick(void);

//! The override for the note byte at pos this frame, if any. Cheap when nothing is active.
bool voice_style_at(size_t pos, VoiceStyle* out);

//! Draw the listening shimmer in the empty cells right of the cursor, when the cursor sits at the
//! end of a line: row/col are the cursor's screen cell (1-based), cols_free the cells from there
//! to the text column's right edge. Leaves the cursor wherever it ends; dawn moves it after.
void voice_draw_overlay(int32_t row, int32_t col, int32_t cols_free);

//! A dim word for the status line ("listening", "reading aloud"), or NULL. Always NULL in focus
//! mode.
const char* voice_status_text(void);

//! Turn the motion off (no shimmer, no glow; a phrase simply appears). Defaults to the
//! DAWN_REDUCED_MOTION environment variable (set and not "0").
void voice_set_reduced_motion(bool on);

//! Whether motion is off (see voice_set_reduced_motion).
bool voice_reduced_motion(void);

#endif // DAWN_VOICE_H
