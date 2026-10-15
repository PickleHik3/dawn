// dawn_commands.c - the writing screen's commands: one table of what can be done, and its keys
//
// A phone in Termux Launcher has no Ctrl key in reach, and nothing on dawn's screen says which
// chords exist. This table lists every command the writing screen takes, each with the key its
// handler already answers (handle_writing in dawn.c, Ctrl+Q in dawn_voice.c), the words for it
// and the shortcut as printed. On a phone (or any terminal under 80 columns) a menu bar across
// the top, "File  Edit  View  AI  Help", opens a dropdown of each menu's command groups
// (menubar_of_group), and the full command menu (Ctrl+], or "all commands" under Help) lists
// every command under its group's heading, the shortcut beside each, so every entry also teaches
// its key. The formatting ribbon under the menu bar draws the format group, one button per
// FormatOp, laid out here in rows that wrap between clusters. Where the terminal sizes text
// (kitty's OSC 66) the menu bar, the ribbon and the status line are drawn at a scale chosen from
// the pane's pixels (commands_ui_scale), so they keep their size on screen when a pinch changes
// the terminal's font; their scaled places (menubar_layout, commands_ribbon_place) are worked out
// here too, in screen cells, so the drawing and the tap regions take the same numbers. The menus
// leave the format group out, since sixteen rows of styles would bury the rest. The help pages
// read the same table: every command by task, with its key and how to reach it by touch (its
// menu, or its ribbon button), so a new command is listed in help by adding it here.
//
// Running a command means pressing its key: a menu feeds the key to the writing handler, and a
// ribbon button registers a tap region for it, so a command behaves exactly as its shortcut does.
// Keys with no physical chord (the menus themselves, the ribbon and its buttons, the cheatsheet)
// are virtual DawnKeys that handle_writing takes like any other.
//
// The menus' line lists (headings between groups, the highlighted line, scrolling) and the help
// pages' lines and page breaks live here too, as plain data. Pure: no app, no backend, so
// tests/test_commands.c links this file alone. Whether a command can run right now (selection,
// focus mode, a model) is decided by command_enabled() in dawn.c from the CommandFlag bits.

#include "dawn_commands.h"
#include "dawn_backend.h"

#include <stddef.h>

// #region Table

//! Every command, grouped in display order. Within a group the most used come first.
static const Command COMMANDS[] = {
    // note
    { "title and details", CMD_GROUP_NOTE, 7, "^G", CMD_NEEDS_MODIFY, NULL },
    { "edit image", CMD_GROUP_NOTE, 5, "^E", CMD_NEEDS_MODIFY, NULL },
    { "pause timer", CMD_GROUP_NOTE, 16, "^P", CMD_NEEDS_TIMER, NULL },
    { "add five minutes", CMD_GROUP_NOTE, 20, "^T", 0, NULL },
    { "close note", CMD_GROUP_NOTE, '\x1b', "esc", 0, NULL },

    // edit
    { "undo", CMD_GROUP_EDIT, 26, "^Z", CMD_NEEDS_MODIFY, NULL },
    { "redo", CMD_GROUP_EDIT, 25, "^Y", CMD_NEEDS_MODIFY, NULL },
    { "cut", CMD_GROUP_EDIT, 24, "^X", CMD_NEEDS_EDIT | CMD_NEEDS_SELECTION, NULL },
    { "copy", CMD_GROUP_EDIT, 3, "^C", CMD_NEEDS_SELECTION, NULL },
    { "paste", CMD_GROUP_EDIT, 22, "^V", CMD_NEEDS_MODIFY, NULL },
    { "select all", CMD_GROUP_EDIT, 1, "^A", 0, NULL },
    { "delete word", CMD_GROUP_EDIT, 23, "^W", CMD_NEEDS_EDIT, NULL },
    { "delete element", CMD_GROUP_EDIT, 4, "^D", CMD_NEEDS_EDIT, NULL },
    { "delete to line start", CMD_GROUP_EDIT, 21, "^U", CMD_NEEDS_EDIT, NULL },
    { "delete to line end", CMD_GROUP_EDIT, 11, "^K", CMD_NEEDS_EDIT, NULL },
    { "indent", CMD_GROUP_EDIT, '\t', "tab", CMD_NEEDS_MODIFY, NULL },
    { "outdent", CMD_GROUP_EDIT, DAWN_KEY_BTAB, "shift+tab", CMD_NEEDS_MODIFY, NULL },

    // format: the ribbon's buttons, in FormatOp order, with the text each shows
    { "bold", CMD_GROUP_FORMAT, DAWN_KEY_FMT_BOLD, "", CMD_NEEDS_EDIT, "B" },
    { "italic", CMD_GROUP_FORMAT, DAWN_KEY_FMT_ITALIC, "", CMD_NEEDS_EDIT, "I" },
    { "strikethrough", CMD_GROUP_FORMAT, DAWN_KEY_FMT_STRIKE, "", CMD_NEEDS_EDIT, "S" },
    { "code", CMD_GROUP_FORMAT, DAWN_KEY_FMT_CODE, "", CMD_NEEDS_EDIT, "`" },
    { "heading 1", CMD_GROUP_FORMAT, DAWN_KEY_FMT_H1, "", CMD_NEEDS_EDIT, "H1" },
    { "heading 2", CMD_GROUP_FORMAT, DAWN_KEY_FMT_H2, "", CMD_NEEDS_EDIT, "H2" },
    { "heading 3", CMD_GROUP_FORMAT, DAWN_KEY_FMT_H3, "", CMD_NEEDS_EDIT, "H3" },
    { "bullet list", CMD_GROUP_FORMAT, DAWN_KEY_FMT_BULLET, "", CMD_NEEDS_EDIT, "•" },
    { "numbered list", CMD_GROUP_FORMAT, DAWN_KEY_FMT_NUMBERED, "", CMD_NEEDS_EDIT, "1." },
    { "task list", CMD_GROUP_FORMAT, DAWN_KEY_FMT_TASK, "", CMD_NEEDS_EDIT, "[ ]" },
    { "quote", CMD_GROUP_FORMAT, DAWN_KEY_FMT_QUOTE, "", CMD_NEEDS_EDIT, ">" },
    { "link", CMD_GROUP_FORMAT, DAWN_KEY_FMT_LINK, "", CMD_NEEDS_EDIT, "link" },
    { "image", CMD_GROUP_FORMAT, DAWN_KEY_FMT_IMAGE, "", CMD_NEEDS_EDIT, "img" },
    { "table", CMD_GROUP_FORMAT, DAWN_KEY_FMT_TABLE, "", CMD_NEEDS_EDIT, "table" },
    { "horizontal rule", CMD_GROUP_FORMAT, DAWN_KEY_FMT_RULE, "", CMD_NEEDS_EDIT, "---" },
    { "code block", CMD_GROUP_FORMAT, DAWN_KEY_FMT_CODE_BLOCK, "", CMD_NEEDS_EDIT, "code" },

    // view
    { "formatting", CMD_GROUP_VIEW, DAWN_KEY_CMD_RIBBON, "", CMD_NEEDS_MODIFY, NULL },
    { "focus mode", CMD_GROUP_VIEW, 6, "^F", CMD_NEEDS_MODIFY, NULL },
    { "plain text", CMD_GROUP_VIEW, 18, "^R", 0, NULL },
    { "hide marks at cursor", CMD_GROUP_VIEW, 2, "^B", 0, NULL },

    // find
    { "find", CMD_GROUP_FIND, 19, "^S", 0, NULL },
    { "table of contents", CMD_GROUP_FIND, 12, "^L", 0, NULL },
    { "jump to footnote", CMD_GROUP_FIND, 14, "^N", 0, NULL },

#ifdef USE_LIBAI
    // ai
    { "chat", CMD_GROUP_AI, 31, "^/", CMD_NEEDS_MODIFY | CMD_NEEDS_AI, NULL },
#ifdef DAWN_HAS_SPEAK
    { "read aloud", CMD_GROUP_AI, 17, "^Q", 0, NULL },
#endif
#endif

    // help
    { "keyboard help", CMD_GROUP_HELP, 15, "^O", 0, NULL },
    { "markdown cheatsheet", CMD_GROUP_HELP, DAWN_KEY_CMD_CHEATSHEET, "", 0, NULL },
    { "welcome tour", CMD_GROUP_HELP, DAWN_KEY_CMD_TOUR, "", 0, NULL },
    { "all commands", CMD_GROUP_HELP, DAWN_KEY_CMD_MENU, "^]", 0, NULL },
};

#define COMMAND_COUNT ((int32_t)(sizeof(COMMANDS) / sizeof(COMMANDS[0])))

static const char* const GROUP_NAMES[CMD_GROUP_COUNT] = {
    [CMD_GROUP_NOTE] = "note",
    [CMD_GROUP_EDIT] = "edit",
    [CMD_GROUP_FORMAT] = "format",
    [CMD_GROUP_VIEW] = "view",
    [CMD_GROUP_FIND] = "find",
    [CMD_GROUP_AI] = "ai",
    [CMD_GROUP_HELP] = "help",
};

const Command* commands_all(int32_t* count)
{
    if (count)
        *count = COMMAND_COUNT;
    return COMMANDS;
}

const Command* commands_find(int32_t key)
{
    for (int32_t i = 0; i < COMMAND_COUNT; i++) {
        if (COMMANDS[i].key == key)
            return &COMMANDS[i];
    }
    return NULL;
}

const char* commands_group_name(CommandGroup group)
{
    return group < CMD_GROUP_COUNT ? GROUP_NAMES[group] : "";
}

const char* commands_button_label(const Command* cmd)
{
    if (!cmd)
        return "";
    return cmd->button ? cmd->button : cmd->label;
}

int32_t commands_ui_scale(int32_t cols, int32_t cell_w, int32_t cell_h, bool launcher)
{
    if (cols <= 0 || cell_w <= 0 || cell_h <= 0)
        return launcher ? 2 : 1;
    // round(cols x cell_w / 13 / cell_h) in integers: (2 x px + 13 x cell_h) / (26 x cell_h)
    int64_t px = (int64_t)cols * cell_w;
    int64_t scale = (2 * px + 13 * (int64_t)cell_h) / (26 * (int64_t)cell_h);
    if (scale < 1)
        scale = 1;
    if (scale > UI_SCALE_MAX)
        scale = UI_SCALE_MAX;
    return (int32_t)scale;
}

// #endregion

// #region Menu bar

static const char* const MENUBAR_NAMES[MENUBAR_COUNT] = {
    [MENUBAR_FILE] = "File",
    [MENUBAR_EDIT] = "Edit",
    [MENUBAR_VIEW] = "View",
    [MENUBAR_AI] = "AI",
    [MENUBAR_HELP] = "Help",
};

//! How help names each menu as the way to reach a command by touch
static const char* const MENUBAR_TAPS[MENUBAR_COUNT] = {
    [MENUBAR_FILE] = "File menu",
    [MENUBAR_EDIT] = "Edit menu",
    [MENUBAR_VIEW] = "View menu",
    [MENUBAR_AI] = "AI menu",
    [MENUBAR_HELP] = "Help menu",
};

int32_t menubar_items(MenuBarItem* out, int32_t max)
{
    int32_t n = 0;
    for (int32_t i = 0; i < MENUBAR_COUNT && n < max; i++) {
#ifndef USE_LIBAI
        if (i == MENUBAR_AI)
            continue; // no chat compiled in, so the ai group is empty
#endif
        out[n++] = (MenuBarItem)i;
    }
    return n;
}

const char* menubar_name(MenuBarItem item)
{
    return item < MENUBAR_COUNT ? MENUBAR_NAMES[item] : "";
}

int32_t menubar_key(MenuBarItem item)
{
    return item < MENUBAR_COUNT ? DAWN_KEY_MENU_FILE + (int32_t)item : DAWN_KEY_NONE;
}

int32_t menubar_from_key(int32_t key)
{
    if (key < DAWN_KEY_MENU_FILE || key > DAWN_KEY_MENU_HELP)
        return -1;
    return key - DAWN_KEY_MENU_FILE;
}

int32_t menubar_of_group(CommandGroup group)
{
    switch (group) {
    case CMD_GROUP_NOTE:
        return MENUBAR_FILE;
    case CMD_GROUP_EDIT:
    case CMD_GROUP_FIND:
        return MENUBAR_EDIT;
    case CMD_GROUP_VIEW:
        return MENUBAR_VIEW;
    case CMD_GROUP_AI:
        return MENUBAR_AI;
    case CMD_GROUP_HELP:
        return MENUBAR_HELP;
    default:
        return -1;
    }
}

const char* menubar_tap(CommandGroup group)
{
    int32_t item = menubar_of_group(group);
    return item >= 0 ? MENUBAR_TAPS[item] : "ribbon";
}

MenuBarItem menubar_neighbour(const MenuBarItem* items, int32_t n, MenuBarItem at, int32_t dir)
{
    for (int32_t i = 0; i < n; i++) {
        if (items[i] == at)
            return items[(i + (dir < 0 ? n - 1 : 1)) % n];
    }
    return at;
}

//! Columns a string takes: its code points (every name and label here is one cell per character)
static int32_t text_cols(const char* s)
{
    int32_t w = 0;
    for (const char* p = s; *p; p++) {
        if (((unsigned char)*p & 0xC0) != 0x80)
            w++;
    }
    return w;
}

//! menubar_layout with gap screen cells between menus
static int32_t menubar_layout_gap(const MenuBarItem* items, int32_t n, int32_t scale, int32_t room, int32_t gap, MenuBarSlot* slots)
{
    int32_t col = 0;
    int32_t kept = 0;
    for (int32_t i = 0; i < n; i++) {
        int32_t w = text_cols(menubar_name(items[i])) * scale;
        int32_t at = kept > 0 ? col + gap : 0;
        if (at + w > room)
            break;
        slots[i] = (MenuBarSlot) { .col = at, .width = w };
        col = at + w;
        kept++;
    }
    return kept;
}

int32_t menubar_layout(const MenuBarItem* items, int32_t n, int32_t scale, int32_t room, MenuBarSlot* slots)
{
    if (scale < 1)
        scale = 1;
    if (n > MENUBAR_COUNT)
        n = MENUBAR_COUNT;
    int32_t kept = menubar_layout_gap(items, n, scale, room, MENUBAR_GAP * scale, slots);
    if (kept < n && scale > 1) {
        // One scaled cell is still two screen cells or more, so tap regions do not overlap.
        MenuBarSlot tight[MENUBAR_COUNT];
        int32_t more = menubar_layout_gap(items, n, scale, room, scale, tight);
        if (more > kept) {
            kept = more;
            for (int32_t i = 0; i < kept; i++)
                slots[i] = tight[i];
        }
    }
    return kept;
}

// #endregion

// #region Formatting ribbon

int32_t commands_ribbon(const Command** out, int32_t max)
{
    int32_t n = 0;
    for (int32_t i = 0; i < COMMAND_COUNT && n < max; i++) {
        if (COMMANDS[i].group == CMD_GROUP_FORMAT)
            out[n++] = &COMMANDS[i];
    }
    return n;
}

//! Columns a button's text takes
static int32_t ribbon_width(const Command* cmd)
{
    return text_cols(commands_button_label(cmd));
}

//! Whether a button opens a cluster: the headings, the lists (with quote) and the inserts
static bool ribbon_cluster_start(int32_t key)
{
    return key == DAWN_KEY_FMT_H1 || key == DAWN_KEY_FMT_BULLET || key == DAWN_KEY_FMT_LINK;
}

//! Packs the buttons into rows of room cells with no regard for clusters: a row breaks wherever the
//! next button does not fit. Returns the rows used, 0 when a button is wider than room.
static int32_t ribbon_pack_gap(const Command* const* items, int32_t n, int32_t room, int32_t gap, RibbonSlot* slots)
{
    int32_t row = 0;
    int32_t col = 0;
    for (int32_t i = 0; i < n; i++) {
        int32_t w = ribbon_width(items[i]);
        if (w > room)
            return 0;
        if (col > 0 && col + w > room) {
            row++;
            col = 0;
        }
        slots[i] = (RibbonSlot) { .row = row, .col = col, .width = w };
        col += w + gap;
    }
    return n > 0 ? row + 1 : 0;
}

//! commands_ribbon_layout with the gap between buttons as a parameter
static int32_t ribbon_layout_gap(const Command* const* items, int32_t n, int32_t room, int32_t gap, RibbonSlot* slots)
{
    int32_t row = 0;
    int32_t col = 0; // where the next button would start; 0 on an empty row
    for (int32_t i = 0; i < n; i++) {
        int32_t w = ribbon_width(items[i]);
        if (w > room)
            return 0;
        if (col > 0 && ribbon_cluster_start(items[i]->key)) {
            // The whole cluster moves down when it fits on a row of its own but not here.
            int32_t cw = w;
            for (int32_t j = i + 1; j < n && !ribbon_cluster_start(items[j]->key); j++)
                cw += gap + ribbon_width(items[j]);
            if (col + cw > room && cw <= room) {
                row++;
                col = 0;
            }
        }
        if (col > 0 && col + w > room) {
            row++;
            col = 0;
        }
        slots[i] = (RibbonSlot) { .row = row, .col = col, .width = w };
        col += w + gap;
    }
    return n > 0 ? row + 1 : 0;
}

int32_t commands_ribbon_layout(const Command* const* items, int32_t n, int32_t room, RibbonSlot* slots)
{
    return ribbon_layout_gap(items, n, room, RIBBON_GAP, slots);
}

int32_t commands_ribbon_place(const Command* const* items, int32_t n, int32_t room, int32_t scale, RibbonSlot* slots)
{
    if (scale <= 1)
        return commands_ribbon_layout(items, n, room, slots);
    if (n > RIBBON_MAX)
        n = RIBBON_MAX;
    int32_t cells = room / scale;
    int32_t rows = ribbon_layout_gap(items, n, cells, RIBBON_GAP, slots);
    if (rows <= 0)
        return 0;
    // Big buttons take two rows each, and a row of them is worth more than the space between
    // them: a gap of one scaled cell (still two screen cells, so tap regions do not overlap) when
    // that leaves fewer rows.
    RibbonSlot tight[RIBBON_MAX];
    int32_t tight_rows = ribbon_layout_gap(items, n, cells, 1, tight);
    if (tight_rows > 0 && tight_rows < rows) {
        rows = tight_rows;
        for (int32_t i = 0; i < n; i++)
            slots[i] = tight[i];
    }
    // Still more than two rows: break anywhere between buttons, so they pack into two when the
    // width allows (a third row only when even that does not fit).
    if (rows > 2) {
        int32_t packed_rows = ribbon_pack_gap(items, n, cells, 1, tight);
        if (packed_rows > 0 && packed_rows < rows) {
            rows = packed_rows;
            for (int32_t i = 0; i < n; i++)
                slots[i] = tight[i];
        }
    }
    for (int32_t i = 0; i < n; i++) {
        slots[i].row *= scale;
        slots[i].col *= scale;
        slots[i].width *= scale;
    }
    return rows * scale;
}

int32_t commands_ribbon_rows(int32_t room, int32_t scale)
{
    const Command* items[RIBBON_MAX];
    RibbonSlot slots[RIBBON_MAX];
    int32_t n = commands_ribbon(items, RIBBON_MAX);
    return commands_ribbon_place(items, n, room, scale, slots);
}

// #endregion

// #region Command menu

void menu_build(MenuState* m, int32_t bar_menu, bool (*enabled)(const Command*))
{
    m->count = 0;
    m->selected = -1;
    m->scroll = 0;
    m->bar_menu = bar_menu;
    m->page = 0;
    int32_t last_group = -1;
    for (int32_t i = 0; i < COMMAND_COUNT; i++) {
        const Command* c = &COMMANDS[i];
        if (c->group == CMD_GROUP_FORMAT)
            continue; // the ribbon is their place; "formatting" under view opens it
        if (bar_menu >= 0 && menubar_of_group(c->group) != bar_menu)
            continue;
        if (bar_menu < 0 && c->key == DAWN_KEY_CMD_MENU)
            continue; // the menu is open already; its key shows in its title
        if (bar_menu < 0 && (int32_t)c->group != last_group) {
            if (m->count >= MENU_LINES_MAX)
                break;
            m->lines[m->count++] = (MenuLine) { .cmd = NULL, .group = c->group, .enabled = false };
            last_group = (int32_t)c->group;
        }
        if (m->count >= MENU_LINES_MAX)
            break;
        bool on = enabled ? enabled(c) : true;
        m->lines[m->count] = (MenuLine) { .cmd = c, .group = c->group, .enabled = on };
        if (on && m->selected < 0)
            m->selected = m->count;
        m->count++;
    }
}

int32_t menu_list_cols(const MenuState* m)
{
    int32_t cols = 0;
    for (int32_t i = 0; i < m->count; i++) {
        const MenuLine* line = &m->lines[i];
        int32_t w;
        if (!line->cmd) {
            w = text_cols(commands_group_name(line->group));
        } else {
            int32_t sc = text_cols(line->cmd->shortcut);
            w = text_cols(line->cmd->label) + (sc > 0 ? 2 + sc : 0);
        }
        if (w > cols)
            cols = w;
    }
    return cols;
}

int32_t menu_step(const MenuState* m, int32_t from, int32_t dir, int32_t steps)
{
    int32_t at = from;
    for (int32_t i = from + dir; i >= 0 && i < m->count && steps > 0; i += dir) {
        if (m->lines[i].cmd && m->lines[i].enabled) {
            at = i;
            steps--;
        }
    }
    return at;
}

void menu_scroll_to(MenuState* m, int32_t visible)
{
    if (visible < 1)
        visible = 1;
    int32_t sel = m->selected;
    if (sel >= 0) {
        // Show the group's heading with its first command.
        int32_t want_top = (sel > 0 && !m->lines[sel - 1].cmd) ? sel - 1 : sel;
        if (want_top < m->scroll)
            m->scroll = want_top;
        if (sel >= m->scroll + visible)
            m->scroll = sel - visible + 1;
    }
    int32_t max_scroll = m->count - visible;
    if (max_scroll < 0)
        max_scroll = 0;
    if (m->scroll > max_scroll)
        m->scroll = max_scroll;
    if (m->scroll < 0)
        m->scroll = 0;
}

// #endregion

// #region Help pages

//! The help pages' first group: moving and selecting are keys of the editor, not commands, and on
//! a phone they are gestures on the note itself.
static const HelpRow MOVE_ROWS[] = {
    { HELP_ROW_HEADING, "move and select", "", "", NULL },
    { HELP_ROW_ITEM, "move the cursor", "arrows", "tap", NULL },
    { HELP_ROW_ITEM, "jump a word", "^arrows", "", NULL },
    { HELP_ROW_ITEM, "line start or end", "home/end", "", NULL },
    { HELP_ROW_ITEM, "scroll", "pgup/pgdn", "swipe", NULL },
    { HELP_ROW_ITEM, "select", "shift+arrows", "hold, drag", NULL },
    { HELP_ROW_ITEM, "select a word", "", "double tap", NULL },
    { HELP_ROW_ITEM, "select a paragraph", "", "triple tap", NULL },
};

#define MOVE_ROW_COUNT ((int32_t)(sizeof(MOVE_ROWS) / sizeof(MOVE_ROWS[0])))

int32_t help_rows(HelpRow* out, int32_t max)
{
    int32_t n = 0;
    for (int32_t i = 0; i < MOVE_ROW_COUNT && n < max; i++)
        out[n++] = MOVE_ROWS[i];

    int32_t last_group = -1;
    for (int32_t i = 0; i < COMMAND_COUNT; i++) {
        const Command* c = &COMMANDS[i];
        if ((int32_t)c->group != last_group) {
            if (n + 2 > max)
                break;
            out[n++] = (HelpRow) { .kind = HELP_ROW_BLANK, .text = "", .key = "", .tap = "", .tap_label = NULL };
            out[n++] = (HelpRow) { .kind = HELP_ROW_HEADING, .text = commands_group_name(c->group), .key = "", .tap = "", .tap_label = NULL };
            last_group = (int32_t)c->group;
        }
        if (n >= max)
            break;
        HelpRow r = { .kind = HELP_ROW_ITEM, .text = c->label, .key = c->shortcut, .tap = menubar_tap(c->group), .tap_label = NULL };
        if (c->group == CMD_GROUP_FORMAT)
            r.tap_label = commands_button_label(c);
        out[n++] = r;
    }
    return n;
}

int32_t help_paginate(const HelpRow* rows, int32_t n, int32_t per_page, HelpSpan* pages, int32_t max)
{
    if (per_page < 1)
        per_page = 1;
    int32_t count = 0;
    int32_t heading = -1; // the heading of the group line i belongs to, -1 outside any
    int32_t i = 0;
    while (i < n && count < max) {
        while (i < n && rows[i].kind == HELP_ROW_BLANK) {
            heading = -1;
            i++;
        }
        if (i >= n)
            break;
        HelpSpan page = { .first = i, .end = i, .heading = -1 };
        int32_t used = 0;
        if (rows[i].kind == HELP_ROW_ITEM && heading >= 0 && per_page > 1) {
            page.heading = heading;
            used = 1;
        }
        while (i < n && used < per_page) {
            // A heading goes down to the next page unless the line it heads fits beside it here.
            if (rows[i].kind == HELP_ROW_HEADING && used > 0 && used + 2 > per_page)
                break;
            if (rows[i].kind == HELP_ROW_HEADING)
                heading = i;
            else if (rows[i].kind == HELP_ROW_BLANK)
                heading = -1;
            used++;
            i++;
        }
        page.end = i;
        while (page.end > page.first && rows[page.end - 1].kind == HELP_ROW_BLANK)
            page.end--;
        pages[count++] = page;
    }
    return count;
}

int32_t help_pages_with_note(int32_t n, int32_t per_page, int32_t* rows_per, bool* blank)
{
    if (per_page < 1)
        per_page = 1;
    if (n < 0)
        n = 0;
    // The fewest pages whose last one, holding what the others leave, still has room for the note:
    // first with a line left beside the note, then (a page of one line) with the note alone.
    for (int32_t pass = 0; pass < 2; pass++) {
        int32_t least_last = pass == 0 && n > 0 ? 1 : 0;
        for (int32_t pages = 1; pages <= n + 1; pages++) {
            int32_t each = (n + pages - 1) / pages;
            int32_t last = n - (pages - 1) * each;
            if (each > per_page || last < least_last || last + 1 > per_page)
                continue;
            *rows_per = each;
            *blank = last > 0 && last + 2 <= per_page;
            return pages;
        }
    }
    // Not reached: n + 1 pages of one line each always fit.
    *rows_per = 1;
    *blank = false;
    return n + 1;
}

// #endregion
