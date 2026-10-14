// dawn_commands.c - the writing screen's commands: one table of what can be done, and its keys
//
// A phone in Termux Launcher has no Ctrl key in reach, and nothing on dawn's screen says which
// chords exist. This table lists every command the writing screen takes, each with the key its
// handler already answers (handle_writing in dawn.c, Ctrl+Q in dawn_voice.c), the words for it
// and the shortcut as printed. The phone-width action bar draws the entries that carry a bar
// place, and the command menu ("more" on the bar, or Ctrl+]) lists all of them with the shortcut
// beside each, so every button also teaches its key. The formatting ribbon draws the format group,
// one button per FormatOp, laid out here in rows that wrap between clusters; the menu leaves that
// group out, since sixteen rows of styles would bury the rest. The help pages read the same
// table: every command by task, with its key and how to reach it by touch (its bar button, its
// ribbon button, or the menu), so a new command is listed in help by adding it here.
//
// Running a command means pressing its key: the bar registers a tap region for the key, the menu
// feeds the key to the writing handler, so a command behaves exactly as its shortcut does. Keys
// with no physical chord (the menu itself, the ribbon and its buttons, the cheatsheet) are virtual
// DawnKeys that handle_writing takes like any other.
//
// The menu's line list (headings between groups, the highlighted line, scrolling) and the help
// pages' lines and page breaks live here too, as plain data. Pure: no app, no backend, so tests/test_commands.c links this file alone. Whether
// a command can run right now (selection, focus mode, a model) is decided by command_enabled() in
// dawn.c from the CommandFlag bits.

#include "dawn_commands.h"
#include "dawn_backend.h"

#include <stddef.h>

// #region Table

//! Every command, grouped in display order. Within a group the most used come first.
static const Command COMMANDS[] = {
    // note
    { "title and details", CMD_GROUP_NOTE, 7, "^G", CMD_NEEDS_MODIFY, 0, NULL },
    { "edit image", CMD_GROUP_NOTE, 5, "^E", CMD_NEEDS_MODIFY, 0, NULL },
    { "pause timer", CMD_GROUP_NOTE, 16, "^P", CMD_NEEDS_TIMER, 0, NULL },
    { "add five minutes", CMD_GROUP_NOTE, 20, "^T", 0, 0, NULL },
    { "close note", CMD_GROUP_NOTE, '\x1b', "esc", 0, 0, NULL },

    // edit
    { "undo", CMD_GROUP_EDIT, 26, "^Z", CMD_NEEDS_MODIFY, 1, NULL },
    { "redo", CMD_GROUP_EDIT, 25, "^Y", CMD_NEEDS_MODIFY, 0, NULL },
    { "cut", CMD_GROUP_EDIT, 24, "^X", CMD_NEEDS_EDIT | CMD_NEEDS_SELECTION, 0, NULL },
    { "copy", CMD_GROUP_EDIT, 3, "^C", CMD_NEEDS_SELECTION, 0, NULL },
    { "paste", CMD_GROUP_EDIT, 22, "^V", CMD_NEEDS_MODIFY, 0, NULL },
    { "select all", CMD_GROUP_EDIT, 1, "^A", 0, 0, NULL },
    { "delete word", CMD_GROUP_EDIT, 23, "^W", CMD_NEEDS_EDIT, 0, NULL },
    { "delete element", CMD_GROUP_EDIT, 4, "^D", CMD_NEEDS_EDIT, 0, NULL },
    { "delete to line start", CMD_GROUP_EDIT, 21, "^U", CMD_NEEDS_EDIT, 0, NULL },
    { "delete to line end", CMD_GROUP_EDIT, 11, "^K", CMD_NEEDS_EDIT, 0, NULL },
    { "indent", CMD_GROUP_EDIT, '\t', "tab", CMD_NEEDS_MODIFY, 0, NULL },
    { "outdent", CMD_GROUP_EDIT, DAWN_KEY_BTAB, "shift+tab", CMD_NEEDS_MODIFY, 0, NULL },

    // format: the ribbon's buttons, in FormatOp order; bar_label is the button's text
    { "bold", CMD_GROUP_FORMAT, DAWN_KEY_FMT_BOLD, "", CMD_NEEDS_EDIT, 0, "B" },
    { "italic", CMD_GROUP_FORMAT, DAWN_KEY_FMT_ITALIC, "", CMD_NEEDS_EDIT, 0, "I" },
    { "strikethrough", CMD_GROUP_FORMAT, DAWN_KEY_FMT_STRIKE, "", CMD_NEEDS_EDIT, 0, "S" },
    { "code", CMD_GROUP_FORMAT, DAWN_KEY_FMT_CODE, "", CMD_NEEDS_EDIT, 0, "`" },
    { "heading 1", CMD_GROUP_FORMAT, DAWN_KEY_FMT_H1, "", CMD_NEEDS_EDIT, 0, "H1" },
    { "heading 2", CMD_GROUP_FORMAT, DAWN_KEY_FMT_H2, "", CMD_NEEDS_EDIT, 0, "H2" },
    { "heading 3", CMD_GROUP_FORMAT, DAWN_KEY_FMT_H3, "", CMD_NEEDS_EDIT, 0, "H3" },
    { "bullet list", CMD_GROUP_FORMAT, DAWN_KEY_FMT_BULLET, "", CMD_NEEDS_EDIT, 0, "•" },
    { "numbered list", CMD_GROUP_FORMAT, DAWN_KEY_FMT_NUMBERED, "", CMD_NEEDS_EDIT, 0, "1." },
    { "task list", CMD_GROUP_FORMAT, DAWN_KEY_FMT_TASK, "", CMD_NEEDS_EDIT, 0, "[ ]" },
    { "quote", CMD_GROUP_FORMAT, DAWN_KEY_FMT_QUOTE, "", CMD_NEEDS_EDIT, 0, ">" },
    { "link", CMD_GROUP_FORMAT, DAWN_KEY_FMT_LINK, "", CMD_NEEDS_EDIT, 0, "link" },
    { "image", CMD_GROUP_FORMAT, DAWN_KEY_FMT_IMAGE, "", CMD_NEEDS_EDIT, 0, "img" },
    { "table", CMD_GROUP_FORMAT, DAWN_KEY_FMT_TABLE, "", CMD_NEEDS_EDIT, 0, "table" },
    { "horizontal rule", CMD_GROUP_FORMAT, DAWN_KEY_FMT_RULE, "", CMD_NEEDS_EDIT, 0, "---" },
    { "code block", CMD_GROUP_FORMAT, DAWN_KEY_FMT_CODE_BLOCK, "", CMD_NEEDS_EDIT, 0, "code" },

    // view
    { "formatting", CMD_GROUP_VIEW, DAWN_KEY_CMD_RIBBON, "", CMD_NEEDS_MODIFY, 3, "Aa" },
    { "focus mode", CMD_GROUP_VIEW, 6, "^F", CMD_NEEDS_MODIFY, 0, NULL },
    { "plain text", CMD_GROUP_VIEW, 18, "^R", 0, 0, NULL },
    { "hide marks at cursor", CMD_GROUP_VIEW, 2, "^B", 0, 0, NULL },

    // find
    { "find", CMD_GROUP_FIND, 19, "^S", 0, 2, NULL },
    { "table of contents", CMD_GROUP_FIND, 12, "^L", 0, 0, NULL },
    { "jump to footnote", CMD_GROUP_FIND, 14, "^N", 0, 0, NULL },

#ifdef USE_LIBAI
    // ai
    { "chat", CMD_GROUP_AI, 31, "^/", CMD_NEEDS_MODIFY | CMD_NEEDS_AI, 4, "AI" },
#ifdef DAWN_HAS_SPEAK
    { "read aloud", CMD_GROUP_AI, 17, "^Q", 0, 0, NULL },
#endif
#endif

    // help
    { "keyboard help", CMD_GROUP_HELP, 15, "^O", 0, 0, NULL },
    { "markdown cheatsheet", CMD_GROUP_HELP, DAWN_KEY_CMD_CHEATSHEET, "", 0, 0, NULL },
    { "welcome tour", CMD_GROUP_HELP, DAWN_KEY_CMD_TOUR, "", 0, 0, NULL },
    { "all commands", CMD_GROUP_HELP, DAWN_KEY_CMD_MENU, "^]", 0, 0, "more" },
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

// #endregion

// #region Action bar

int32_t commands_bar(const Command** out, int32_t max)
{
    // Places are small numbers; walk them in order so the table's grouping doesn't matter.
    int32_t n = 0;
    for (int32_t place = 1; place <= 127 && n < max; place++) {
        bool any_later = false;
        for (int32_t i = 0; i < COMMAND_COUNT && n < max; i++) {
            if (COMMANDS[i].bar == place)
                out[n++] = &COMMANDS[i];
            else if (COMMANDS[i].bar > place)
                any_later = true;
        }
        if (!any_later)
            break;
    }
    return n;
}

const char* commands_bar_label(const Command* cmd)
{
    if (!cmd)
        return "";
    return cmd->bar_label ? cmd->bar_label : cmd->label;
}

int32_t commands_bar_fit(const int32_t* widths, int32_t n, int32_t more_w, int32_t gap, int32_t room)
{
    int32_t used = more_w;
    int32_t kept = 0;
    for (int32_t i = 0; i < n; i++) {
        int32_t next = used + widths[i] + gap;
        if (next > room)
            break;
        used = next;
        kept++;
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

//! Columns a button's text takes: its code points (every ribbon label is one cell per character)
static int32_t ribbon_width(const Command* cmd)
{
    int32_t w = 0;
    for (const char* p = commands_bar_label(cmd); *p; p++) {
        if (((unsigned char)*p & 0xC0) != 0x80)
            w++;
    }
    return w;
}

//! Whether a button opens a cluster: the headings, the lists (with quote) and the inserts
static bool ribbon_cluster_start(int32_t key)
{
    return key == DAWN_KEY_FMT_H1 || key == DAWN_KEY_FMT_BULLET || key == DAWN_KEY_FMT_LINK;
}

int32_t commands_ribbon_layout(const Command* const* items, int32_t n, int32_t room, RibbonSlot* slots)
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
                cw += RIBBON_GAP + ribbon_width(items[j]);
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
        col += w + RIBBON_GAP;
    }
    return n > 0 ? row + 1 : 0;
}

int32_t commands_ribbon_rows(int32_t room)
{
    const Command* items[RIBBON_MAX];
    RibbonSlot slots[RIBBON_MAX];
    int32_t n = commands_ribbon(items, RIBBON_MAX);
    return commands_ribbon_layout(items, n, room, slots);
}

// #endregion

// #region Command menu

void menu_build(MenuState* m, bool (*enabled)(const Command*))
{
    m->count = 0;
    m->selected = -1;
    m->scroll = 0;
    int32_t last_group = -1;
    for (int32_t i = 0; i < COMMAND_COUNT; i++) {
        const Command* c = &COMMANDS[i];
        if (c->key == DAWN_KEY_CMD_MENU)
            continue; // the menu is open already; its key shows in its title
        if (c->group == CMD_GROUP_FORMAT)
            continue; // the ribbon is their place; "formatting" under view opens it
        if ((int32_t)c->group != last_group) {
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
        HelpRow r = { .kind = HELP_ROW_ITEM, .text = c->label, .key = c->shortcut, .tap = "menu", .tap_label = NULL };
        if (c->group == CMD_GROUP_FORMAT) {
            r.tap = "ribbon";
            r.tap_label = commands_bar_label(c);
        } else if (c->bar > 0) {
            r.tap = "bar";
            r.tap_label = commands_bar_label(c);
        }
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
