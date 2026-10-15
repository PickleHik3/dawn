// dawn_chat.h

#ifndef DAWN_CHAT_H
#define DAWN_CHAT_H

#include "dawn_types.h"

// #region Message Management

//! Add a message to the chat history
//! @param text message content
//! @param is_user true for user messages, false for AI responses
void chat_add(const char* text, bool is_user);

//! Clear all chat messages and free memory
void chat_clear(void);

// #endregion

//! Push the current text onto the undo stack (dawn.c). The AI's edit tools call it on both
//! sides of a change, so one Ctrl+Z lands exactly on the text from before the edit; so does a
//! reload of a note changed elsewhere (dawn_file.c), on every build.
void save_undo_state(void);

#if HAS_LIBAI

// #region AI Session

//! Initialize AI session with system prompt and tools
//! Creates session with document, search, time, and history tools
void ai_init_session(void);

//! Send a prompt to the AI and stream the response
//! @param prompt user's input message
void ai_send(const char* prompt);

//! The AI ribbon's actions on the note: each is a turn of the chat (its line shows there), asked
//! in the USER lane like a typed question, and its edit is made like the chat's, one undo step.
typedef enum {
    AI_ACTION_NONE,
    AI_ACTION_TITLE, //!< A title for the note now, as a <set_title> block (the writer's own choice from then on)
    AI_ACTION_FORMAT, //!< The note restructured into well-formed Markdown, as one <replace_note> block
    AI_ACTION_COUNT
} AiAction;

//! Ask for action. A notice says it went out and, when no edit came of it, why; a made edit says
//! so itself. Nothing is sent while a reply is still coming, without a model, or for an empty note.
//! @return whether the request went out
bool ai_action(AiAction action);

//! Stop the reply in flight. What already streamed stays, marked as stopped; no edit the
//! model had not finished is made. The chat is ready for the next question once the reply ends.
void ai_stop(void);

//! The AI's per-frame work: the warm writing session (priming, the waiting question, the
//! compaction, keep-warm) and live titles. Call once per frame.
void ai_tick(void);

//! A new, empty conversation with dawn's one system prompt and its tools, its history verbatim
//! (ai_set_session_verbatim()). dawn_session.c builds every conversation with it. 0 on failure.
ai_session_id_t ai_new_conversation(void);

//! raw reduced to one clean title line: no heading marks, "Title:" label, quotes or final
//! period, at most cap-1 bytes (never splitting a character). False when nothing is left.
bool ai_clean_title(const char* raw, char* out, size_t cap);

//! Dawn's one system prompt, and its tools as JSON, for the session's token budget.
const char* ai_system_prompt(void);
const char* ai_tools_json(void);

// #endregion

#endif // HAS_LIBAI

#endif // DAWN_CHAT_H
