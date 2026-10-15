// dawn_commands.h - The writing screen's commands, one table read by every place that lists them
//
// Each command is the key dawn's writing handler already takes for it, with the words and the
// shortcut a person sees. The menu bar's dropdowns, the command menu, the formatting ribbon and the help
// pages draw from this table, so a button and the key it teaches cannot disagree.
// Pure: no app, no backend; whether a command can run right now is decided in dawn.c.

#ifndef DAWN_COMMANDS_H
#define DAWN_COMMANDS_H

#include <stdbool.h>
#include <stdint.h>

//! The command groups, in display order
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
    const char* button; //!< The ribbon button's text ("B", "H1"); NULL to use label
} Command;

//! Every command, grouped in display order (the AI ones only where the chat is compiled in).
//! @param count receives how many there are
const Command* commands_all(int32_t* count);

//! The command run by key, or NULL
const Command* commands_find(int32_t key);

//! A group's heading in the menu ("note", "edit", ...)
const char* commands_group_name(CommandGroup group);

//! The text a ribbon button shows for cmd (its label when it has no button text)
const char* commands_button_label(const Command* cmd);

//! Biggest scale the menu bar, the ribbon and the status line are drawn at
#define UI_SCALE_MAX 4

//! The scale the menu bar, the ribbon, the status line and the dropdowns are drawn at where the
//! terminal sizes text (kitty's OSC 66), chosen from the pane's pixels so a row of them is about a
//! thirteenth of the pane's width on screen whatever the font: round(cols x cell_w / 13 / cell_h),
//! between 1 and UI_SCALE_MAX. A pinch to a small font (more columns, smaller cells) raises it and
//! a large font lowers it, so the chrome stays the same size. Without the cell size (cell_w or
//! cell_h not above 0): 2 in Termux Launcher's terminal, 1 elsewhere.
int32_t commands_ui_scale(int32_t cols, int32_t cell_w, int32_t cell_h, bool launcher);

// #region Menu bar

//! The menu bar's menus, left to right
typedef enum {
    MENUBAR_FILE, //!< The note group
    MENUBAR_EDIT, //!< The edit and find groups
    MENUBAR_VIEW, //!< The view group
    MENUBAR_AI, //!< The ai group; only where the chat is compiled in
    MENUBAR_HELP, //!< The help group
    MENUBAR_COUNT
} MenuBarItem;

//! Scaled cells between two menus on the bar (one, above scale 1, when two would drop a menu)
#define MENUBAR_GAP 2

//! The menus this build's bar shows, left to right (AI only where the chat is compiled in).
//! @return how many were written to out (at most max)
int32_t menubar_items(MenuBarItem* out, int32_t max);

//! A menu's name as the bar writes it, capitalised as a word processor has them ("File")
const char* menubar_name(MenuBarItem item);

//! The key that opens item's dropdown (DAWN_KEY_MENU_FILE ... DAWN_KEY_MENU_HELP)
int32_t menubar_key(MenuBarItem item);

//! The menu whose dropdown key opens, or -1 when key opens none
int32_t menubar_from_key(int32_t key);

//! The menu a group's commands are listed under, or -1 for the format group (the ribbon's)
int32_t menubar_of_group(CommandGroup group);

//! How help says to reach a group's commands by touch: "Edit menu"; "ribbon" for the format group
const char* menubar_tap(CommandGroup group);

//! The menu dir (-1 left, 1 right) from at among the n menus of items, wrapping round; at itself
//! when it is not among them
MenuBarItem menubar_neighbour(const MenuBarItem* items, int32_t n, MenuBarItem at, int32_t dir);

//! Where a menu's name goes on the bar, in screen cells
typedef struct {
    int32_t col; //!< Cells from the bar's left edge
    int32_t width; //!< Cells its name takes: its letters times the scale
} MenuBarSlot;

//! Lay the bar's n menus out at scale in room screen cells: each name takes its letters x scale
//! cells, MENUBAR_GAP scaled cells apart, or (above scale 1) one scaled cell apart when the wider
//! gap would drop a menu that the narrower keeps; menus are dropped from the right only when even
//! that does not fit. Tap regions (a name and a cell either side) never overlap.
//! @param slots receives the kept menus' places (n entries)
//! @return the number of menus kept, from the left
int32_t menubar_layout(const MenuBarItem* items, int32_t n, int32_t scale, int32_t room, MenuBarSlot* slots);

// #endregion

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
//! commands_ribbon_layout. Above it (kitty text sizing) the layout is worked out in scaled cells,
//! room / scale of them, with the buttons RIBBON_GAP scaled cells apart, or one when that saves a
//! row; every slot's row, col and width is then multiplied by the scale, so a row of buttons is
//! scale screen rows and slots are in screen cells.
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
    int32_t bar_menu; //!< The MenuBarItem this is the dropdown of; -1 for the full menu (Ctrl+])
    int32_t page; //!< Lines the list showed when last drawn (page up and down step this many)
} MenuState;

//! Fill m and highlight the first command that enabled() allows. With bar_menu -1, the full menu:
//! every command but the menu's own and the ribbon's buttons, a heading before each group. With a
//! MenuBarItem, that menu's dropdown: the commands of its groups (menubar_of_group) in table
//! order, no headings, "all commands" included.
void menu_build(MenuState* m, int32_t bar_menu, bool (*enabled)(const Command*));

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
    const char* tap; //!< How to do it by touch: its menu ("Edit menu"), "ribbon" or a gesture ("swipe"); "" when none
    const char* tap_label; //!< The ribbon's button text ("B"), NULL when none
} HelpRow;

//! Most lines help_rows() writes
#define HELP_ROWS_MAX 96

//! The shortcut pages' lines: a hand-written "move and select" group (the arrow and selection keys
//! are not commands), then every command under its group's heading in table order with the menu
//! bar's menu it is in, the format group as the ribbon's buttons, a blank between groups.
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
