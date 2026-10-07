// dawn_render.h

#ifndef DAWN_RENDER_H
#define DAWN_RENDER_H

#include "dawn_types.h"

// #region Utility Functions

//! Clear entire screen with background color
void render_clear(void);

//! Print centered text at given row
//! @param row display row (1-based)
//! @param text text to display
//! @param fg foreground color
void render_center_text(int32_t row, const char* text, DawnColor fg);

//! Render a floating popup box centered on screen
//! @param width box width in columns
//! @param height box height in rows
//! @param out_top output: top-left row (1-based)
//! @param out_left output: top-left column (1-based)
void render_popup_box(int32_t width, int32_t height, int32_t* out_top, int32_t* out_left);

// #endregion

// #region Screen Renderers

//! Render the welcome/menu screen
void render_welcome(void);

//! Render the timer selection screen
void render_timer_select(void);

//! Render the style selection screen
void render_style_select(void);

//! Render the help screen with keyboard shortcuts
void render_help(void);

//! Render the session history browser
void render_history(void);

//! Render the session completion screen
void render_finished(void);

//! Render the frontmatter editing screen
void render_fm_edit(void);

//! Render the block editing screen (images, etc.)
void render_block_edit(void);

//! Render the table of contents overlay
void render_toc(void);

//! Render the search overlay
void render_search(void);

//! The search result drawn on screen cell (row, col), both 1-based, as last rendered: each row
//! owns the box's width, so a tap anywhere on it counts.
//! @return the SearchState.selected value that picks it, or -1 for none (the "by meaning" label)
int32_t render_search_hit(int32_t row, int32_t col);

//! MODE_CONFLICT's choices, in the order the dialog lists them
enum { CONFLICT_RELOAD, CONFLICT_OVERWRITE, CONFLICT_KEEP, CONFLICT_CHOICES };

//! Render the dialog shown when the note changed elsewhere while it had unsaved edits here
//! (MODE_CONFLICT): what happened, where the writer's text is kept, and the three choices.
void render_conflict(void);

//! The choice drawn on screen cell (row, col), both 1-based, in the dialog as last rendered: each
//! choice owns its row across the dialog's width, so a tap anywhere on it counts.
//! @return a CONFLICT_* value, or -1 for none
int32_t render_conflict_hit(int32_t row, int32_t col);

// #endregion

#endif // DAWN_RENDER_H
