// dawn_scrollind.h - The fading scroll indicator: a thin pill in a pane's right margin

#ifndef DAWN_SCROLLIND_H
#define DAWN_SCROLLIND_H

#include <stdbool.h>
#include <stdint.h>

//! The panes that carry an indicator, each with its own kitty image.
typedef enum {
    SCROLLIND_NOTE,
    SCROLLIND_CHAT,
    SCROLLIND_PANE_COUNT
} ScrollIndPane;

//! Start a frame: call once per render, inside the synchronized-output frame, before any pane is
//! drawn. Every pane that isn't given to scrollind_show() before scrollind_frame_end() is hidden.
void scrollind_frame_begin(void);

//! The pane is on screen and in MODE_WRITING this frame. col is the 1-based screen column of the
//! margin cell the pill sits in; track_row/track_rows are the pane's text rows (1-based first row,
//! count); content, viewport and offset are in rows (offset = first visible row of the content).
//! Draws nothing when the content fits, in print mode, and without kitty graphics.
void scrollind_show(ScrollIndPane pane, int32_t col, int32_t track_row, int32_t track_rows,
    int32_t content, int32_t viewport, int32_t offset);

//! Whether pills can be drawn this frame (kitty graphics, a pixel cell size, interactive mode), so
//! a pane can drop its own text hint ("↑ scroll for more") in favour of one.
bool scrollind_available(void);

//! End a frame: hides every pane that wasn't shown since scrollind_frame_begin().
void scrollind_frame_end(void);

//! The terminal's images were all deleted behind this module's back (a=d,d=A): forget what it
//! had transmitted, so the next show sends the pill again.
void scrollind_forget(void);

//! Delete the pills' image data from the terminal (on exit).
void scrollind_shutdown(void);

#endif // DAWN_SCROLLIND_H
