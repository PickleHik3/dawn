// dawn_status.h - The status panel: what the model and the meaning index are doing, bottom right.
//
// A small panel slides out from the right edge while something runs in the background (the model
// loading, a live title, the meaning index embedding notes, read-aloud...) and slides back about
// a second and a half after it ends. Nothing is drawn while nothing happens. The screen's own
// renderer says where the panel may go (status_area) and where it left the cursor
// (status_cursor); render() then calls status_frame() once, after the mode has drawn.

#ifndef DAWN_STATUS_H
#define DAWN_STATUS_H

#include "dawn_types.h"

//! Where the panel may go this frame: rows top..bottom (it grows up from bottom), its right edge
//! at right_col, at most max_cols wide. welcome: the welcome screen, with room for more lines, a
//! longer bar and the note being indexed. Forgotten after status_frame(); no call, no panel.
void status_area(int32_t top, int32_t bottom, int32_t right_col, int32_t max_cols, bool welcome);

//! Where the screen left the visible cursor this frame (1-based): the panel keeps off that row and
//! puts the cursor back there after drawing (drawing moves it). Forgotten after status_frame().
void status_cursor(int32_t row, int32_t col);

//! Once per frame, after the mode drew: works out what is happening, moves the slide along and
//! draws the panel in the area given, when show is true. With show false (a dialog is open, or a
//! screen without an area) the slide keeps time but nothing is drawn.
void status_frame(bool show);

//! Whether the panel was on screen (any part of it) in the last frame.
bool status_visible(void);

#endif // DAWN_STATUS_H
