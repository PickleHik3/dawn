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

//! Name an untitled note once it has enough text, in the background. Call once per frame.
void ai_title_tick(void);

// #endregion

#endif // HAS_LIBAI

#endif // DAWN_CHAT_H
