// dawn_input.h

#ifndef DAWN_INPUT_H
#define DAWN_INPUT_H

#include "dawn_types.h"

// #region Input Reading

//! Read a single key from input
//! Handles escape sequences, arrow keys, modifiers, and mouse events
//! @return key code or KEY_NONE if no input available
int32_t input_read_key(void);

//! Get the column of the last mouse event
//! @return column (1-based) of last mouse click/scroll
int32_t input_last_mouse_col(void);

//! Get the row of the last mouse event
//! @return row (1-based) of last mouse click/scroll
int32_t input_last_mouse_row(void);

//! Pop the oldest pending dictation event (after read_key returned DAWN_KEY_DICTATION).
//! For PHRASE/REPLACE the caller frees out->text. False when none is waiting or the backend
//! has no dictation marks.
bool input_take_dictation(DawnDictEvent* out);

// #endregion

#endif // DAWN_INPUT_H
