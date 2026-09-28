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

#if HAS_LIBAI

//! Push the current text onto the undo stack (dawn.c). The AI's edit tools call it on both
//! sides of a change, so one Ctrl+Z lands exactly on the text from before the edit.
void save_undo_state(void);

// #region AI Session

//! Initialize AI session with system prompt and tools
//! Creates session with document, search, time, and history tools
void ai_init_session(void);

//! Send a prompt to the AI and stream the response
//! @param prompt user's input message
void ai_send(const char* prompt);

//! Stop the reply in flight. What already streamed stays, marked as stopped; no edit the
//! model had not finished is made. The chat is ready for the next question once the reply ends.
void ai_stop(void);

//! The AI's per-frame work: the warm writing session (priming, the waiting question, the
//! compaction, keep-warm) and the automatic title. Call once per frame.
void ai_tick(void);

//! A new, empty conversation with dawn's one system prompt and its tools, its history verbatim
//! (ai_set_session_verbatim()). dawn_session.c builds every conversation with it. 0 on failure.
ai_session_id_t ai_new_conversation(void);

//! Dawn's one system prompt, and its tools as JSON, for the session's token budget.
const char* ai_system_prompt(void);
const char* ai_tools_json(void);

// #endregion

#endif // HAS_LIBAI

#endif // DAWN_CHAT_H
