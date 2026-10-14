// dawn_render.h

#ifndef DAWN_RENDER_H
#define DAWN_RENDER_H

#include "dawn_commands.h"
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

//! How many cards the first-run tour has (the "ask" card only where the chat is compiled in)
int32_t render_tour_cards(void);

//! Render MODE_TOUR: app.tour_card, centred like the welcome screen, with the dots that mark it
//! and the footer's tappable hints. app.tour_pushed says whether esc skips or closes.
void render_tour(void);

//! Render the timer selection screen
void render_timer_select(void);

//! Render the style selection screen
void render_style_select(void);

//! Render the help screen app.help_screen of page app.help_page (HelpPage): the shortcuts by task
//! from the command table, the Markdown cheatsheet, the activity list (notices), or the meaning
//! index's state with its switch and rebuild (only where DAWN_EMBED_LIVE). The box swallows taps
//! (HIT_INERT); its footer hints and the meaning page's two action rows are taps on their keys.
void render_help(void);

//! How many screens page takes at the screen's current size: the shortcuts and the cheatsheet
//! split over as many as their lines need, the others take one, and the meaning page none where
//! the meaning index is not compiled in.
int32_t render_help_screens(HelpPage page);

//! The help screen after (dir 1) or before (dir -1) the one shown, as tab and <- turn to it:
//! the next screen of the same page, else the first (or last) screen of the next page that has one.
//! @return false when there is none that way
bool render_help_neighbour(int32_t dir, HelpPage* page, int32_t* screen);

//! How long the meaning page's rebuild waits for its confirming second r (or tap)
#define HELP_REBUILD_CONFIRM_MS 3000

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
//! (MODE_CONFLICT): what happened, where the writer's text is kept, and the three choices, each
//! a tap on its key (r, o, k) across the dialog's width.
void render_conflict(void);

//! Render the command menu (MODE_MENU) over the note: every command of dawn_commands under its
//! group's heading, its shortcut right-aligned, the selection highlighted and kept in view (m's
//! scroll is updated), commands that cannot run now drawn dim. Each command that can run is a
//! tap on its row; the rest of the box swallows taps.
void render_menu(MenuState* m);

// #endregion

#endif // DAWN_RENDER_H
