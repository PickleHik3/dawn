// dawn_speak.h - Read aloud, karaoke-style (spec §09).
//
// The note from the cursor (or the selection) is cut into sentences of rendered text, markdown
// syntax and code left out, and handed to libai/ai_speak.c, which sends them one at a time to the
// launcher's POST /v1/ai/speak. Because sentence boundaries are request boundaries, the sentence
// being spoken is known exactly and highlighted; inside it a soft glow runs across the words at
// the pace the previous sentence was measured at (audioSeconds per byte). Single-threaded.

#ifndef DAWN_SPEAK_H
#define DAWN_SPEAK_H

#include "dawn_voice.h"

//! One sentence as spoken: text[0..len) is what goes to the launcher, and map[i] is the note
//! byte that spoken byte i came from (strictly increasing), so the renderer can find its words.
typedef struct {
    size_t start; //!< Note byte of the first spoken byte
    size_t end; //!< One past the note byte of the last spoken byte
    size_t off; //!< Offset of this sentence in SpeakText.text / .map
    size_t len; //!< Spoken bytes
} SpeakSentence;

//! Sentences built from a stretch of the note.
typedef struct {
    char* text; //!< All sentences' spoken bytes, back to back (not NUL-separated)
    uint32_t* map; //!< Note byte for each spoken byte
    size_t text_len;
    SpeakSentence* sentences;
    int32_t count;
} SpeakText;

//! Cut src[0..len) (the note bytes from note offset base on) into sentences of speakable text.
//! at_line_start: src[0] begins a line; in_fence: src[0] is inside a ``` code block. Pure, no
//! globals: dawn's markdown is followed loosely (headings, lists, quotes, tables, links, images,
//! code, math, HTML, footnotes, emoji shortcodes, bare URLs). Returns false when out of memory;
//! out->count may be 0. Free with speak_text_free().
bool speak_build(const char* src, size_t len, size_t base, bool at_line_start, bool in_fence, SpeakText* out);

//! Free what speak_build() allocated.
void speak_text_free(SpeakText* t);

//! Whether a read-aloud run is going (from start until it ends, fails or is stopped).
bool speak_active(void);

//! Start reading the selection, or from the cursor to the end of the note. A notice says why
//! when nothing can be read (no launcher, nothing speakable). Returns true when a run started.
bool speak_start(void);

//! Stop the run now; the phone falls silent (POST /v1/ai/speak/stop). No-op when idle.
void speak_stop(void);

//! Advance the run for this frame (now: DAWN_CLOCK_MS). Returns true while it is animating.
bool speak_tick(int64_t now_ms);

//! The highlight and glow for note byte pos, if it is in the sentence being spoken.
bool speak_style_at(size_t pos, VoiceStyle* out);

#endif // DAWN_SPEAK_H
