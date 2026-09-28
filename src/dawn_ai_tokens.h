// dawn_ai_tokens.h - Token budgeting and the note-context snapshot for the AI chat.

#ifndef DAWN_AI_TOKENS_H
#define DAWN_AI_TOKENS_H

#include "dawn_embed.h"
#include "dawn_types.h"

#if HAS_LIBAI

#include <stddef.h>

// #region Token estimate

//! Roughly how many tokens text costs: chars/3.6 for mostly-Latin text, chars/2.5 when the text
//! is mostly Arabic script (denser per character on typical BPE vocabularies), then scaled by the
//! running calibration ratio from ai_calibrate_estimate(). NULL or empty text is 0.
int32_t ai_estimate_tokens(const char* text);

//! Fold usage.prompt_tokens (the true count for a prompt this estimate was made for) against the
//! estimate this file made for that same prompt, updating a running ratio applied to future
//! estimates. Call once per completed turn, with the estimate made for exactly the text sent.
void ai_calibrate_estimate(int32_t estimated_prompt_tokens, int32_t actual_prompt_tokens);

//! The endpoint's context window in tokens (ai_context_window(), cached after the first fetch;
//! 4096 until then).
int32_t ai_ctx_window(void);

// #endregion

// #region Note snapshot

//! What the note-context snapshot ended up describing, so the message to the model can say
//! exactly what it was and was not shown (spec: "you see X; the rest is not shown").
typedef struct {
    bool whole_note; //!< Nothing was left out: no explainer is needed
    bool has_selection; //!< The selection was included
    char section_heading[160]; //!< The section's heading text, or "" when the note has no headings
    bool has_outline; //!< The outline was included
    bool has_relevant; //!< Passages picked by relevance to the question were included
    int32_t neighbours_before; //!< Whole paragraphs included before the section
    int32_t neighbours_after; //!< Whole paragraphs included after the section
} AiSnapshotInfo;

//! Build the text of the note to attach to a question, filling a token budget in order:
//! selection, then the section around the cursor (its heading to the next heading of the same or
//! higher level), then the outline (every heading, in order), then neighbouring paragraphs
//! alternating before/after the section until the budget is spent. relevant (may be NULL) are
//! pieces of the note ranked by relevance to the question (dawn_embed, best first, already checked
//! against the current text): between the outline and the neighbours, each one not inside the
//! selection or the section is added while the budget lasts. gb is the document, bc its
//! parsed blocks (NULL is treated as "no headings"), cursor the cursor position, sel_start/
//! sel_end the selection (equal when there is none). Returns a malloc'd string (never NULL,
//! "" for an empty note) and fills *info; caller frees the string.
char* ai_note_snapshot(const GapBuffer* gb, void* block_cache, size_t cursor, size_t sel_start,
    size_t sel_end, int32_t budget_tokens, const EmbedHit* relevant, int32_t relevant_count, AiSnapshotInfo* info);

// #endregion

#endif // HAS_LIBAI

#endif // DAWN_AI_TOKENS_H
