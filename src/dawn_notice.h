// dawn_notice.h - Quiet status-line notices and the activity list

#ifndef DAWN_NOTICE_H
#define DAWN_NOTICE_H

#include <stdbool.h>
#include <stdint.h>

//! What a notice is about. Everything except NOTICE_ERROR fades on its own; an error stays on the
//! status line until the writer acts (a key or a tap), because a lost save must not scroll away.
typedef enum {
    NOTICE_INFO, //!< e.g. "model ready"
    NOTICE_AI_CHANGE, //!< the AI changed the note or its title; Ctrl+Z right after reverts it
    NOTICE_ERROR, //!< e.g. "couldn't save" — stays until acknowledged
} NoticeKind;

//! Post a notice. The text is copied; keep it short and lower-case ("renamed · Eid plans").
//! It shows dim on the status line for about four seconds, then fades, and is always appended to
//! the activity list, which is where it can be found again. Focus mode shows nothing but still
//! records. Safe to call from dawn's thread only.
void notice_post(NoticeKind kind, const char* text);

//! Acknowledge (clear) a sticky error notice, if one is showing.
void notice_ack(void);

//! Whether a notice currently wants the status line, and its text and fade (0..1, 1 = fresh).
//! Returns false when nothing should be shown.
bool notice_current(const char** text, NoticeKind* kind, float* fresh);

//! The activity list, newest first. Returns the number of entries written to the out arrays.
int32_t notice_history(const char** texts, int64_t* times_sec, int32_t max);

#endif // DAWN_NOTICE_H
