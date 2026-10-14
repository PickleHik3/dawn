// dawn_commands.h - The writing screen's commands, one table read by every place that lists them
//
// Each command is the key dawn's writing handler already takes for it, with the words and the
// shortcut a person sees. The action bar, the command menu, the formatting ribbon (and later the
// help page) draw from this table, so a button and the key it teaches cannot disagree.
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

// #region Formatting ribbon

//! Most buttons the ribbon holds
#define RIBBON_MAX 24

//! Columns between two ribbon buttons; a button's tap region is its text and one cell either side
#define RIBBON_GAP 2

//! Where a ribbon button goes
typedef struct {
    int32_t row; //!< Row of the ribbon, 0 first
    int32_t col; //!< Columns from the row's left edge
    int32_t width; //!< Columns its text takes
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

//! Rows the ribbon takes in room columns (0 when it does not fit at all)
int32_t commands_ribbon_rows(int32_t room);

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

#endif // DAWN_COMMANDS_H
