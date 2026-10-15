// dawn_commands.h - The writing screen's commands, one table read by every place that lists them
//
// Each command is the key dawn's writing handler already takes for it, with the words and the
// shortcut a person sees. The action bar, the command menu, the formatting ribbon and the help
// pages draw from this table, so a button and the key it teaches cannot disagree.
// Pure: no app, no backend; whether a command can run right now is decided in dawn.c.

#ifndef DAWN_COMMANDS_H
#define DAWN_COMMANDS_H

#include <stdbool.h>
#include <stdint.h>

//! The menu's sections, in display order
typedef enum {
    CMD_GROUP_NOTE,
    CMD_GROUP_EDIT,
    CMD_GROUP_FORMAT, //!< The formatting ribbon's buttons; left out of the command menu
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
    int8_t bar; //!< Place on the action bar, 1 leftmost; 0 when it is not on the bar
    const char* bar_label; //!< The bar's or the ribbon's shorter text ("Aa", "AI", "H1"); NULL to use label
} Command;

//! Every command, grouped in display order (the AI ones only where the chat is compiled in).
//! @param count receives how many there are
const Command* commands_all(int32_t* count);

//! The command run by key, or NULL
const Command* commands_find(int32_t key);

//! A group's heading in the menu ("note", "edit", ...)
const char* commands_group_name(CommandGroup group);

//! The action bar's commands, left to right, without the menu's own "more".
//! @return how many were written to out (at most max)
int32_t commands_bar(const Command** out, int32_t max);

//! The text the action bar shows for cmd
const char* commands_bar_label(const Command* cmd);

//! How many of the bar's n leading items fit in room columns, with "more" (more_w columns) always
//! kept at the end and gap columns between neighbours. Items are dropped from the right.
//! @param widths each item's width in columns
//! @return the number of items (not counting "more") that fit
int32_t commands_bar_fit(const int32_t* widths, int32_t n, int32_t more_w, int32_t gap, int32_t room);

//! Screen cells between two action bar items at scale: three at scale 1, two (one scaled space)
//! at scale 2 and above
int32_t commands_bar_gap(int32_t scale);

//! Where an action bar item goes, in screen cells
typedef struct {
    int32_t col; //!< Cells from the bar's left edge
    int32_t width; //!< Cells its text takes: its label's columns times the scale
} BarSlot;

//! Lay the action bar out at scale (1, or 2 where kitty text sizing draws it twice the size, two
//! rows tall): each label takes widths[i] x scale screen cells, commands_bar_gap(scale) apart,
//! and items are dropped from the right (commands_bar_fit on the scaled widths) so that "more"
//! always fits at the end.
//! @param widths each item's label width in columns at scale 1
//! @param more_w "more"'s label width at scale 1
//! @param room the bar's width in screen cells
//! @param slots receives the kept items' places, then "more"'s (the return value + 1 entries;
//!              room for n + 1)
//! @return the number of items (not counting "more") that fit, or -1 when "more" alone does not
int32_t commands_bar_layout(const int32_t* widths, int32_t n, int32_t more_w, int32_t scale, int32_t room, BarSlot* slots);

// #region Formatting ribbon

//! Most buttons the ribbon holds
#define RIBBON_MAX 24

//! Columns between two ribbon buttons; a button's tap region is its text and one cell either side
#define RIBBON_GAP 2

//! Where a ribbon button goes
typedef struct {
    int32_t row; //!< Row of the ribbon, 0 first (screen rows from its top in commands_ribbon_place)
    int32_t col; //!< Columns from the row's left edge (screen cells in commands_ribbon_place)
    int32_t width; //!< Columns its text takes (screen cells in commands_ribbon_place)
} RibbonSlot;

//! The ribbon's buttons, left to right (the CMD_GROUP_FORMAT commands in table order).
//! @return how many were written to out (at most max)
int32_t commands_ribbon(const Command** out, int32_t max);

//! Lay the ribbon's n buttons out in rows of room columns, RIBBON_GAP apart. A row breaks before
//! a cluster (inline styles, headings, lists and quote, inserts) that no longer fits on it but
//! fits on a row of its own; a cluster wider than a row breaks between its buttons.
//! @param slots receives each button's place (n entries)
//! @return the number of rows, or 0 when there are no buttons or one is wider than room
int32_t commands_ribbon_layout(const Command* const* items, int32_t n, int32_t room, RibbonSlot* slots);

//! Lay the ribbon's n buttons out at scale in room screen cells. At scale 1 this is
//! commands_ribbon_layout. At scale 2 (kitty text sizing) the layout is worked out in scaled cells,
//! room / 2 of them, with the buttons RIBBON_GAP scaled cells apart, or one when that saves a row;
//! every slot's row, col and width is then multiplied by the scale, so a row of buttons is two
//! screen rows and slots are in screen cells.
//! @param slots receives each button's place in screen rows and cells (n entries)
//! @return the screen rows the ribbon takes (button rows x scale), 0 when it does not fit
int32_t commands_ribbon_place(const Command* const* items, int32_t n, int32_t room, int32_t scale, RibbonSlot* slots);

//! Screen rows the ribbon takes at scale in room screen cells (0 when it does not fit at all)
int32_t commands_ribbon_rows(int32_t room, int32_t scale);

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
} MenuState;

//! Fill m with every command but the menu's own and the ribbon's buttons, a heading before each
//! group, and highlight the first that enabled() allows.
void menu_build(MenuState* m, bool (*enabled)(const Command*));

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
    const char* tap; //!< How to do it by touch: "bar", "ribbon", "menu" or a gesture ("swipe"); "" when none
    const char* tap_label; //!< The bar's or the ribbon's button text ("Aa", "B"), NULL when none
} HelpRow;

//! Most lines help_rows() writes
#define HELP_ROWS_MAX 96

//! The shortcut pages' lines: a hand-written "move and select" group (the arrow and selection keys
//! are not commands), then every command under its group's heading in table order, the format
//! group as the ribbon's buttons, a blank between groups.
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
