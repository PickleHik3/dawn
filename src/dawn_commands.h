// dawn_commands.h - The writing screen's commands, one table read by every place that lists them
//
// Each command is the key dawn's writing handler already takes for it, with the words and the
// shortcut a person sees. The tab row's ribbons (File, Formatting, AI), the command menu and the
// help pages draw from this table, so a button and the key it teaches cannot disagree.
// Pure: no app, no backend; whether a command can run right now is decided in dawn.c.

#ifndef DAWN_COMMANDS_H
#define DAWN_COMMANDS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

//! The command groups, in display order
typedef enum {
    CMD_GROUP_NOTE,
    CMD_GROUP_EDIT,
    CMD_GROUP_FORMAT, //!< The Formatting ribbon's styles; left out of the command menu
    CMD_GROUP_VIEW,
    CMD_GROUP_FIND,
    CMD_GROUP_AI,
    CMD_GROUP_HELP,
    CMD_GROUP_COUNT
} CommandGroup;

//! What a command needs before it can run (dawn.c's command_enabled() checks them)
typedef enum {
    CMD_NEEDS_EDIT = 1 << 0, //!< CAN_EDIT(): not in focus or preview mode
    CMD_NEEDS_MODIFY = 1 << 1, //!< CAN_MODIFY(): not in preview mode
    CMD_NEEDS_SELECTION = 1 << 2, //!< Some text is selected
    CMD_NEEDS_AI = 1 << 3, //!< A model is reachable (app.ai_ready)
    CMD_NEEDS_TIMER = 1 << 4, //!< A timer is running
} CommandFlag;

//! One command of the writing screen
typedef struct {
    const char* label; //!< Lower case, as dawn writes its hints ("undo", "table of contents")
    CommandGroup group;
    int32_t key; //!< The key handle_writing takes for it: a Ctrl code, a character or a DawnKey
    const char* shortcut; //!< The key as printed ("^Z", "esc", "tab"); "" when it has none
    uint32_t flags; //!< CommandFlag bits
    const char* caption; //!< The word under its ribbon button ("bold", "heading 1"), lower case
    const char* glyph; //!< Its button's face without Nerd Font: a letter or a symbol ("B", "H1", "↶"), never Markdown
    uint32_t icon; //!< Its button's Nerd Font icon (a Material Design code point, U+F0001 on), 0 for none
} Command;

//! Every command, grouped in display order (the AI ones only where the chat is compiled in).
//! @param count receives how many there are
const Command* commands_all(int32_t* count);

//! The command run by key, or NULL
const Command* commands_find(int32_t key);

//! A group's heading in the menu ("note", "edit", ...)
const char* commands_group_name(CommandGroup group);

//! Biggest scale the tab row and the ribbons are drawn at
#define UI_SCALE_MAX 4

//! A chrome row (the tab row, a ribbon button's glyph) is about the pane's width over this on
//! screen: 26 gives about 41 pixels on a 1080-pixel phone, one row of the launcher's normal font
#define UI_SCALE_DIVISOR 26

//! The scale the tab row, the ribbons and the command menu are drawn at where the terminal sizes
//! text (kitty's OSC 66), chosen from the pane's pixels so a row of them is about
//! 1/UI_SCALE_DIVISOR of the pane's width on screen whatever the font:
//! round(cols x cell_w / UI_SCALE_DIVISOR / cell_h), between 1 and UI_SCALE_MAX. A pinch to a
//! small font (more columns, smaller cells) raises it and a large font lowers it, so the chrome
//! stays the same size. Without the cell size (cell_w or cell_h not above 0) it is 1.
int32_t commands_ui_scale(int32_t cols, int32_t cell_w, int32_t cell_h);

// #region Tab row

//! The tabs across the top, left to right; each drops its own ribbon
typedef enum {
    RIBBON_TAB_FILE, //!< The note, edit and find, and view groups
    RIBBON_TAB_FORMAT, //!< The format group, then help
    RIBBON_TAB_AI, //!< The chat, the model and the AI's actions; only where the chat is compiled in
    RIBBON_TAB_COUNT
} RibbonTab;

//! Screen cells either side of a tab's name, inside its tab (the tab's surface and tap region)
#define TAB_PAD 1

//! The tabs this build shows, left to right (AI only where the chat is compiled in).
//! @return how many were written to out (at most max)
int32_t tabs_items(RibbonTab* out, int32_t max);

//! A tab's name as the row writes it, capitalised as a word processor has them ("Formatting")
const char* tabs_name(RibbonTab tab);

//! How help names a tab as the way to reach a command by touch ("File ribbon")
const char* tabs_tap(RibbonTab tab);

//! The key a tap on tab is (DAWN_KEY_TAB_FILE ...)
int32_t tabs_key(RibbonTab tab);

//! The tab key stands for, or -1 when it is no tab's
int32_t tabs_from_key(int32_t key);

//! Where a tab goes on the row, in screen cells from the row's left edge
typedef struct {
    int32_t col; //!< Its first cell (the padding before its name)
    int32_t width; //!< Its name's letters times the scale, plus TAB_PAD each side
} TabSlot;

//! Lay the n tabs out side by side at scale in room screen cells, each its name at scale with
//! TAB_PAD cells either side, touching the next (the padding keeps the names apart).
//! @param slots receives each tab's place (n entries, all filled)
//! @return the cells the row of tabs takes, or 0 when they do not all fit room
int32_t tabs_layout(const RibbonTab* items, int32_t n, int32_t scale, int32_t room, TabSlot* slots);

//! The largest scale up to scale at which all n tabs fit room cells (1 when even that does not)
int32_t tabs_scale(const RibbonTab* items, int32_t n, int32_t scale, int32_t room);

//! What the right end of the tab row says in at most room columns, the longest of these that fits:
//! "<alert> · 120 words · 4:59", "<alert> · 120w · 4:59", "<alert> · 4:59", "<alert> · 120w",
//! "<alert>" (each part only when it is there: alert and timer may be NULL or ""), else "".
//! The alert ("not saved") always comes first, so a caller can colour it.
//! @return the columns out takes (its code points)
int32_t tabs_status(int32_t words, const char* timer, const char* alert, int32_t room, char* out, size_t cap);

// #endregion

// #region Ribbons

//! Most buttons one ribbon holds
#define RIBBON_MAX 32

//! Screen cells between two buttons of one section, at least (a spread row may widen it)
#define RIBBON_GAP 1

//! Screen cells between two sections, at least: a gap, the divider's column, a gap
#define RIBBON_DIVIDER 3

//! Screen rows of the thin rule between a section and the one stacked under it
#define RIBBON_RULE 1

//! Columns of the model button's caption at the caption's scale: its widest state, "not loaded"
#define RIBBON_MODEL_STATE_COLS 10

//! One button of a tab's ribbon
typedef struct {
    const Command* cmd;
    int32_t section; //!< Which of the tab's sections it is in, 0 first; a divider or a rule separates two
    int32_t width; //!< Screen cells it takes: the same for every button of a ribbon, its widest's (ribbon_button_cols)
} RibbonButton;

//! The scale a ribbon button's caption is drawn at under a glyph drawn at scale: half, at least 1
int32_t ribbon_caption_scale(int32_t scale);

//! Screen rows a ribbon button takes at scale: its glyph's scale rows and its caption's under them
int32_t ribbon_button_rows(int32_t scale);

//! Blank screen rows between one row of ribbon buttons and the next on a page (a caption and the
//! boxes under it): 1 where the chrome is scaled (scale 2 on), 0 at scale 1, where a terminal's
//! rows are few. A rule above a stacked section is drawn in that row (it takes RIBBON_RULE rows
//! when the gap is narrower). None above a page's first row or below its last.
int32_t ribbon_row_gap(int32_t scale);

//! Columns a button's glyph is given at scale 1: 2, the Nerd Font icon's block (the icon and a
//! space, one OSC 66 block w=2) and the widest text glyph alike, so a button is as wide with the
//! font as without it; more only for a text glyph wider than that
int32_t ribbon_glyph_cols(const Command* cmd, bool icons);

//! Screen cells a button needs at scale: its glyph at scale with a cell either side, or its
//! caption at the caption's scale when that is wider; the model button's caption is its state, up
//! to RIBBON_MODEL_STATE_COLS columns.
int32_t ribbon_button_cols(const Command* cmd, int32_t scale, bool icons);

//! A tab's buttons in order, with their sections, all as wide as the widest of them at scale (so
//! they line up as a grid): File is details, pause and close, then undo, redo, cut, copy and paste,
//! then find, contents, focus and light or dark; Formatting is the format group, then help; AI is
//! the chat and the model, then undo, title, auto format and read aloud. Commands this build lacks
//! are left out.
//! @return how many were written to out (at most max)
int32_t ribbon_buttons(RibbonTab tab, int32_t scale, bool icons, RibbonButton* out, int32_t max);

//! The first tab whose ribbon holds cmd (left to right), or -1 when none does (it is then in the
//! full command menu, commands_in_menu)
int32_t ribbon_tab_of(const Command* cmd);

//! Screen cells the paging button ("more") takes at scale
int32_t ribbon_more_cols(int32_t scale);

//! Where a ribbon button goes
typedef struct {
    int32_t page; //!< The page it is on, 0 first (always 0 when the ribbon fits at once)
    int32_t row; //!< Screen rows from the ribbon's top (of its page)
    int32_t col; //!< Screen cells from the ribbon's left edge
    int32_t width; //!< Screen cells it takes
    bool rule_above; //!< A thin rule runs across the ribbon on the screen row above: the button opens
                     //!< a row whose section is stacked under another (set on the row's first button)
} RibbonSlot;

//! What a laid-out ribbon takes
typedef struct {
    int32_t rows; //!< Most rows of buttons a page shows
    int32_t pages; //!< 1 when every button shows at once
    int32_t height; //!< Screen rows the ribbon takes: its tallest page's rows of buttons and the gaps and rules between them
    int32_t more_col; //!< Where the paging button goes on the first row (pages > 1)
    int32_t more_width; //!< Its width in screen cells (0 when there is none)
} RibbonFrame;

//! Lay a ribbon's n buttons out in rows of room screen cells at scale, RIBBON_GAP apart. A section
//! either goes on after the one before it on the same row (after RIBBON_DIVIDER cells, where a
//! divider goes; it needs its first button to fit there, and what does not fit wraps to the left
//! edge of the rows under it) or starts at the left edge of a new row, under a RIBBON_RULE-row rule.
//! Two rows on a page are ribbon_row_gap(scale) rows apart, the rule drawn in that gap (the larger
//! of the two counts). Of those choices the one with the fewest rows wins, then the one splitting
//! the fewest sections over rows, so a section stays a block unless going on saves a row. A section
//! on rows of its own spreads its buttons evenly over them (the buttons are one width, so they form
//! a grid). When the rows, gaps and rules are taller than max_height screen rows, the ribbon pages:
//! as many rows as fit on each page (no gap or rule opens a page), the room of each narrowed by a
//! paging button at the right end of the first row.
//! Then every row is spread to the full room (less the paging button): its buttons are widened
//! evenly by whole cells, all of a row one width, and the cells left over widen its gaps (else its
//! dividers, else its one button), so no strip is left empty at the right. Rows that hold one
//! section only and follow each other are one grid, set as the fullest of them: a shorter last row
//! keeps the columns above it.
//! @param slots receives each button's place (n entries); a slot's width is its button's or more
//! @return false (frame zeroed) when there are no buttons, one is wider than room, or not even one
//!         row fits max_height
bool ribbon_layout(const RibbonButton* buttons, int32_t n, int32_t room, int32_t scale, int32_t max_height,
    RibbonSlot* slots, RibbonFrame* frame);

// #endregion

// #region Command menu

//! Most lines the command menu holds (headings and commands)
#define MENU_LINES_MAX 64

//! One line of the command menu: a group heading (cmd NULL) or a command
typedef struct {
    const Command* cmd;
    CommandGroup group;
    bool enabled; //!< The command can run now; disabled lines are drawn dim and never selected
} MenuLine;

//! The command menu's contents and where it is
typedef struct {
    MenuLine lines[MENU_LINES_MAX];
    int32_t count;
    int32_t selected; //!< Line of the highlighted command, -1 when none can run
    int32_t scroll; //!< First line shown
    int32_t page; //!< Lines the list showed when last drawn (page up and down step this many)
} MenuState;

//! Whether the full command menu lists cmd: every command but the format group's (the Formatting
//! ribbon is their place) and the menu's own
bool commands_in_menu(const Command* cmd);

//! Fill m with the full menu and highlight the first command that enabled() allows: every command
//! but the menu's own and the format group's (the Formatting ribbon is their place), a heading
//! before each group.
void menu_build(MenuState* m, bool (*enabled)(const Command*));

//! Scaled cells the widest line of m needs: its label, then two cells and its shortcut when it has
//! one (a heading: its name). Padding around the list is the drawing's.
int32_t menu_list_cols(const MenuState* m);

//! The enabled command line steps lines away from line from (dir -1 up, 1 down, steps > 0),
//! stopping at the last enabled one that way.
//! @return that line, or from when there is none that way
int32_t menu_step(const MenuState* m, int32_t from, int32_t dir, int32_t steps);

//! Scroll m so its selection shows in visible lines, with its group's heading too when the
//! selection is the group's first command.
void menu_scroll_to(MenuState* m, int32_t visible);

// #endregion

// #region Help pages

//! What one line of a help page holds
typedef enum {
    HELP_ROW_BLANK, //!< Space between two groups
    HELP_ROW_HEADING, //!< A group's name
    HELP_ROW_ITEM, //!< Something to do, its key and how to do it by touch
} HelpRowKind;

//! One line of the shortcut pages
typedef struct {
    HelpRowKind kind;
    const char* text; //!< The heading's words or what the item does ("undo"); "" on a blank
    const char* key; //!< The item's key as printed ("^Z"), "" when it has none
    const char* tap; //!< How to do it by touch: its ribbon ("File ribbon"), "menu" (the full command menu) or a gesture ("swipe"); "" when none
} HelpRow;

//! Most lines help_rows() writes
#define HELP_ROWS_MAX 96

//! The shortcut pages' lines: a hand-written "move and select" group (the arrow and selection keys
//! are not commands), then every command under its group's heading in table order with the ribbon
//! it is on (ribbon_tab_of), or "menu" when it is on none, a blank between groups.
//! @return how many were written to out (at most max)
int32_t help_rows(HelpRow* out, int32_t max);

//! One page of a paginated list of lines
typedef struct {
    int32_t first; //!< Index of the page's first line
    int32_t end; //!< One past its last
    int32_t heading; //!< A heading drawn above first when the page opens inside its group, -1 when none
} HelpSpan;

//! Split n lines into pages of per_page lines. A page never starts or ends with a blank and never
//! ends with a heading (it moves to the next page with the line it heads); a page that opens
//! inside a group repeats that group's heading, which counts toward per_page.
//! @return pages written to pages (at most max)
int32_t help_paginate(const HelpRow* rows, int32_t n, int32_t per_page, HelpSpan* pages, int32_t max);

//! Split n lines that end with a one-line note into pages of per_page lines. The lines are spread
//! evenly so the last page, which carries the note (under a blank when there is room), is never
//! left with the note alone unless a page holds a single line.
//! @param rows_per receives how many lines each page but the last holds (the last holds the rest)
//! @param blank receives whether a blank goes between the last line and the note
//! @return the number of pages, 1 or more
int32_t help_pages_with_note(int32_t n, int32_t per_page, int32_t* rows_per, bool* blank);

// #endregion

#endif // DAWN_COMMANDS_H
