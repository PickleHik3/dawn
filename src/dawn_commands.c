// dawn_commands.c - the writing screen's commands: one table of what can be done, and its keys
//
// A phone in Termux Launcher has no Ctrl key in reach, and nothing on dawn's screen says which
// chords exist. This table lists every command the writing screen takes, each with the key its
// handler already answers (handle_writing in dawn.c, Ctrl+Q in dawn_voice.c), the words for it,
// the shortcut as printed, and its ribbon button: a glyph (a Nerd Font icon where the font is
// there, else a letter or a symbol, never bare Markdown) over a caption.
//
// On a phone (or any terminal under 80 columns) a tab row runs across the top, as a word
// processor has it: "File  Formatting  AI" (AI only where the chat is compiled in). A tap on a tab
// drops its ribbon under the row: File holds the note group, then edit and find, then view;
// Formatting the format group (one button per FormatOp), then help; AI the chat and the model,
// then undo, a title, auto format and read aloud. Sections are split by a divider, and a ribbon
// wraps into rows, paging (a "more" button) when its rows would leave the note too little room.
// Where the terminal sizes text (kitty's OSC 66) the tab row and the ribbons are drawn at a scale
// chosen from the pane's pixels (commands_ui_scale), so they keep their size on screen when a
// pinch changes the terminal's font; their places (tabs_layout, ribbon_layout) are worked out
// here, in screen cells, so the drawing and the tap regions take the same numbers. The full
// command menu (Ctrl+], or "all commands" in help) lists every command but the format group's
// under its group's heading, the shortcut beside each, so every entry also teaches its key. The
// help pages read the same table: every command by task, with its key and the ribbon it is on, so
// a new command is listed in help by adding it here.
//
// Running a command means pressing its key: the menu feeds the key to the writing handler, and a
// ribbon button registers a tap region for it, so a command behaves exactly as its shortcut does.
// Keys with no physical chord (the tabs, the ribbon's buttons, the cheatsheet, the AI's actions)
// are virtual DawnKeys that handle_writing takes like any other.
//
// The menu's line list (headings between groups, the highlighted line, scrolling) and the help
// pages' lines and page breaks live here too, as plain data. Pure: no app, no backend, so
// tests/test_commands.c links this file alone. Whether a command can run right now (selection,
// focus mode, a model) is decided by command_enabled() in dawn.c from the CommandFlag bits.

#include "dawn_commands.h"
#include "dawn_backend.h"

#include <stdio.h>
#include <string.h>

// #region Table

//! Every command, grouped in display order. Within a group the most used come first. Icons are
//! Material Design code points in the Nerd Font (nf-md-*).
static const Command COMMANDS[] = {
    // note
    { "title and details", CMD_GROUP_NOTE, 7, "^G", CMD_NEEDS_MODIFY, "details", "✎", 0xF03EB },
    { "edit image", CMD_GROUP_NOTE, 5, "^E", CMD_NEEDS_MODIFY, "edit img", "▣", 0xF02E9 },
    { "pause timer", CMD_GROUP_NOTE, 16, "^P", CMD_NEEDS_TIMER, "pause", "‖", 0xF03E4 },
    { "add five minutes", CMD_GROUP_NOTE, 20, "^T", 0, "+5 min", "+5", 0xF0415 },
    { "close note", CMD_GROUP_NOTE, '\x1b', "esc", 0, "close", "✕", 0xF0156 },

    // edit
    { "undo", CMD_GROUP_EDIT, 26, "^Z", CMD_NEEDS_MODIFY, "undo", "↶", 0xF054C },
    { "redo", CMD_GROUP_EDIT, 25, "^Y", CMD_NEEDS_MODIFY, "redo", "↷", 0xF044E },
    { "cut", CMD_GROUP_EDIT, 24, "^X", CMD_NEEDS_EDIT | CMD_NEEDS_SELECTION, "cut", "✂", 0xF0190 },
    { "copy", CMD_GROUP_EDIT, 3, "^C", CMD_NEEDS_SELECTION, "copy", "⧉", 0xF018F },
    { "paste", CMD_GROUP_EDIT, 22, "^V", CMD_NEEDS_MODIFY, "paste", "⎘", 0xF0192 },
    { "select all", CMD_GROUP_EDIT, 1, "^A", 0, "select", "▭", 0xF0486 },
    { "delete word", CMD_GROUP_EDIT, 23, "^W", CMD_NEEDS_EDIT, "del word", "⌫", 0xF006E },
    { "delete element", CMD_GROUP_EDIT, 4, "^D", CMD_NEEDS_EDIT, "del elem", "⌦", 0xF01B4 },
    { "delete to line start", CMD_GROUP_EDIT, 21, "^U", CMD_NEEDS_EDIT, "del start", "⇤", 0 },
    { "delete to line end", CMD_GROUP_EDIT, 11, "^K", CMD_NEEDS_EDIT, "del end", "⇥", 0 },
    { "indent", CMD_GROUP_EDIT, '\t', "tab", CMD_NEEDS_MODIFY, "indent", "→", 0xF0276 },
    { "outdent", CMD_GROUP_EDIT, DAWN_KEY_BTAB, "shift+tab", CMD_NEEDS_MODIFY, "outdent", "←", 0xF0275 },

    // format: the Formatting ribbon's styles, in FormatOp order
    { "bold", CMD_GROUP_FORMAT, DAWN_KEY_FMT_BOLD, "", CMD_NEEDS_EDIT, "bold", "B", 0xF0264 },
    { "italic", CMD_GROUP_FORMAT, DAWN_KEY_FMT_ITALIC, "", CMD_NEEDS_EDIT, "italic", "I", 0xF0277 },
    { "strikethrough", CMD_GROUP_FORMAT, DAWN_KEY_FMT_STRIKE, "", CMD_NEEDS_EDIT, "strike", "S", 0xF0280 },
    { "code", CMD_GROUP_FORMAT, DAWN_KEY_FMT_CODE, "", CMD_NEEDS_EDIT, "code", "</>", 0xF0174 },
    { "heading 1", CMD_GROUP_FORMAT, DAWN_KEY_FMT_H1, "", CMD_NEEDS_EDIT, "heading 1", "H1", 0xF026B },
    { "heading 2", CMD_GROUP_FORMAT, DAWN_KEY_FMT_H2, "", CMD_NEEDS_EDIT, "heading 2", "H2", 0xF026C },
    { "heading 3", CMD_GROUP_FORMAT, DAWN_KEY_FMT_H3, "", CMD_NEEDS_EDIT, "heading 3", "H3", 0xF026D },
    { "bullet list", CMD_GROUP_FORMAT, DAWN_KEY_FMT_BULLET, "", CMD_NEEDS_EDIT, "bullets", "•", 0xF0279 },
    { "numbered list", CMD_GROUP_FORMAT, DAWN_KEY_FMT_NUMBERED, "", CMD_NEEDS_EDIT, "numbered", "1.", 0xF027B },
    { "task list", CMD_GROUP_FORMAT, DAWN_KEY_FMT_TASK, "", CMD_NEEDS_EDIT, "task", "☐", 0xF0135 },
    { "quote", CMD_GROUP_FORMAT, DAWN_KEY_FMT_QUOTE, "", CMD_NEEDS_EDIT, "quote", "❝", 0xF027E },
    { "link", CMD_GROUP_FORMAT, DAWN_KEY_FMT_LINK, "", CMD_NEEDS_EDIT, "link", "↗", 0xF0337 },
    { "image", CMD_GROUP_FORMAT, DAWN_KEY_FMT_IMAGE, "", CMD_NEEDS_EDIT, "image", "▣", 0xF02E9 },
    { "table", CMD_GROUP_FORMAT, DAWN_KEY_FMT_TABLE, "", CMD_NEEDS_EDIT, "table", "▦", 0xF04EB },
    { "horizontal rule", CMD_GROUP_FORMAT, DAWN_KEY_FMT_RULE, "", CMD_NEEDS_EDIT, "rule", "—", 0xF0374 },
    { "code block", CMD_GROUP_FORMAT, DAWN_KEY_FMT_CODE_BLOCK, "", CMD_NEEDS_EDIT, "block", "{}", 0xF0169 },

    // view
    { "focus mode", CMD_GROUP_VIEW, 6, "^F", CMD_NEEDS_MODIFY, "focus", "◎", 0xF0208 },
    { "plain text", CMD_GROUP_VIEW, 18, "^R", 0, "plain", "¶", 0xF027D },
    { "hide marks at cursor", CMD_GROUP_VIEW, 2, "^B", 0, "marks", "◌", 0xF0209 },
    { "light or dark", CMD_GROUP_VIEW, DAWN_KEY_CMD_THEME, "", 0, "theme", "◐", 0xF07F5 },

    // find
    { "find", CMD_GROUP_FIND, 19, "^S", 0, "find", "⌕", 0xF0349 },
    { "table of contents", CMD_GROUP_FIND, 12, "^L", 0, "contents", "☰", 0xF0836 },
    { "jump to footnote", CMD_GROUP_FIND, 14, "^N", 0, "footnote", "¹", 0xF0283 },

#ifdef USE_LIBAI
    // ai
    { "chat", CMD_GROUP_AI, 31, "^/", CMD_NEEDS_MODIFY | CMD_NEEDS_AI, "chat", "✦", 0xF0369 },
    { "choose the model", CMD_GROUP_AI, DAWN_KEY_AI_MODEL, "", CMD_NEEDS_AI, "model", "◇", 0xF061A },
    { "give the note a title", CMD_GROUP_AI, DAWN_KEY_AI_TITLE, "", CMD_NEEDS_MODIFY | CMD_NEEDS_AI, "title", "T", 0xF05F4 },
    { "auto format", CMD_GROUP_AI, DAWN_KEY_AI_FORMAT, "", CMD_NEEDS_EDIT | CMD_NEEDS_AI, "format", "≣", 0xF0068 },
#ifdef DAWN_HAS_SPEAK
    { "read aloud", CMD_GROUP_AI, 17, "^Q", 0, "read", "♪", 0xF057E },
#endif
#endif

    // help
    { "keyboard help", CMD_GROUP_HELP, 15, "^O", 0, "keys", "?", 0xF030C },
    { "markdown cheatsheet", CMD_GROUP_HELP, DAWN_KEY_CMD_CHEATSHEET, "", 0, "markdown", "M↓", 0xF0354 },
    { "welcome tour", CMD_GROUP_HELP, DAWN_KEY_CMD_TOUR, "", 0, "tour", "★", 0xF02FD },
    { "all commands", CMD_GROUP_HELP, DAWN_KEY_CMD_MENU, "^]", 0, "commands", "⋯", 0xF035C },
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

//! Columns a string takes: its code points (every name, caption and glyph here is one cell each)
static int32_t text_cols(const char* s)
{
    int32_t w = 0;
    for (const char* p = s; *p; p++) {
        if (((unsigned char)*p & 0xC0) != 0x80)
            w++;
    }
    return w;
}

// #endregion

// #region Tab row

static const char* const TAB_NAMES[RIBBON_TAB_COUNT] = {
    [RIBBON_TAB_FILE] = "File",
    [RIBBON_TAB_FORMAT] = "Formatting",
    [RIBBON_TAB_AI] = "AI",
};

//! How help names each tab as the way to reach a command by touch
static const char* const TAB_TAPS[RIBBON_TAB_COUNT] = {
    [RIBBON_TAB_FILE] = "File ribbon",
    [RIBBON_TAB_FORMAT] = "Formatting ribbon",
    [RIBBON_TAB_AI] = "AI ribbon",
};

int32_t tabs_items(RibbonTab* out, int32_t max)
{
    int32_t n = 0;
    for (int32_t i = 0; i < RIBBON_TAB_COUNT && n < max; i++) {
#ifndef USE_LIBAI
        if (i == RIBBON_TAB_AI)
            continue; // no chat compiled in, so there is nothing for the AI ribbon to hold
#endif
        out[n++] = (RibbonTab)i;
    }
    return n;
}

const char* tabs_name(RibbonTab tab)
{
    return tab < RIBBON_TAB_COUNT ? TAB_NAMES[tab] : "";
}

const char* tabs_tap(RibbonTab tab)
{
    return tab < RIBBON_TAB_COUNT ? TAB_TAPS[tab] : "";
}

int32_t tabs_key(RibbonTab tab)
{
    return tab < RIBBON_TAB_COUNT ? DAWN_KEY_TAB_FILE + (int32_t)tab : DAWN_KEY_NONE;
}

int32_t tabs_from_key(int32_t key)
{
    if (key < DAWN_KEY_TAB_FILE || key > DAWN_KEY_TAB_AI)
        return -1;
    return key - DAWN_KEY_TAB_FILE;
}

int32_t tabs_layout(const RibbonTab* items, int32_t n, int32_t scale, int32_t room, TabSlot* slots)
{
    if (scale < 1)
        scale = 1;
    int32_t col = 0;
    for (int32_t i = 0; i < n; i++) {
        int32_t w = text_cols(tabs_name(items[i])) * scale + 2 * TAB_PAD;
        slots[i] = (TabSlot) { .col = col, .width = w };
        col += w;
    }
    return col <= room ? col : 0;
}

int32_t tabs_scale(const RibbonTab* items, int32_t n, int32_t scale, int32_t room)
{
    TabSlot slots[RIBBON_TAB_COUNT];
    if (n > RIBBON_TAB_COUNT)
        n = RIBBON_TAB_COUNT;
    while (scale > 1 && tabs_layout(items, n, scale, room, slots) == 0)
        scale--;
    return scale < 1 ? 1 : scale;
}

//! out = the parts that are there, joined by " · "; its columns
static int32_t status_join(char* out, size_t cap, const char* a, const char* b, const char* c)
{
    const char* parts[3] = { a, b, c };
    size_t at = 0;
    out[0] = '\0';
    for (int32_t i = 0; i < 3; i++) {
        if (!parts[i] || !parts[i][0])
            continue;
        int32_t w = snprintf(out + at, cap - at, "%s%s", at > 0 ? " · " : "", parts[i]);
        if (w < 0 || (size_t)w >= cap - at)
            break;
        at += (size_t)w;
    }
    return text_cols(out);
}

int32_t tabs_status(int32_t words, const char* timer, const char* alert, int32_t room, char* out, size_t cap)
{
    if (!out || cap == 0)
        return 0;
    out[0] = '\0';
    if (room <= 0)
        return 0;
    char full[32], brief[32];
    snprintf(full, sizeof(full), "%d word%s", words, words == 1 ? "" : "s");
    snprintf(brief, sizeof(brief), "%dw", words);
    const char* forms[][3] = {
        { alert, full, timer },
        { alert, brief, timer },
        { alert, NULL, timer },
        { alert, brief, NULL },
        { alert, NULL, NULL },
    };
    char buf[160];
    for (size_t i = 0; i < sizeof(forms) / sizeof(forms[0]); i++) {
        int32_t w = status_join(buf, sizeof(buf), forms[i][0], forms[i][1], forms[i][2]);
        if (w > 0 && w <= room) {
            snprintf(out, cap, "%s", buf);
            return text_cols(out);
        }
    }
    return 0;
}

// #endregion

// #region Ribbons

//! One section of a ribbon: the commands of up to two groups in table order, or a list of keys
typedef struct {
    RibbonTab tab;
    int32_t groups[2]; //!< CommandGroup values, -1 for none
    int32_t keys[6]; //!< When groups are both -1: these commands in this order, 0 ending the list
} RibbonSection;

//! Every ribbon's sections, tab by tab, left to right
static const RibbonSection SECTIONS[] = {
    { RIBBON_TAB_FILE, { CMD_GROUP_NOTE, -1 }, { 0 } },
    { RIBBON_TAB_FILE, { CMD_GROUP_EDIT, CMD_GROUP_FIND }, { 0 } },
    { RIBBON_TAB_FILE, { CMD_GROUP_VIEW, -1 }, { 0 } },
    { RIBBON_TAB_FORMAT, { CMD_GROUP_FORMAT, -1 }, { 0 } },
    { RIBBON_TAB_FORMAT, { CMD_GROUP_HELP, -1 }, { 0 } },
    // The AI's: talking to it and which model answers, then what it does to the note (undo is the
    // edit group's own, here too since an AI edit is one undo step).
    { RIBBON_TAB_AI, { -1, -1 }, { 31, DAWN_KEY_AI_MODEL, 0 } },
    { RIBBON_TAB_AI, { -1, -1 }, { 26, DAWN_KEY_AI_TITLE, DAWN_KEY_AI_FORMAT, 17, 0 } },
};

#define SECTION_COUNT ((int32_t)(sizeof(SECTIONS) / sizeof(SECTIONS[0])))

int32_t ribbon_caption_scale(int32_t scale)
{
    return scale / 2 > 1 ? scale / 2 : 1;
}

int32_t ribbon_button_rows(int32_t scale)
{
    if (scale < 1)
        scale = 1;
    return scale + ribbon_caption_scale(scale);
}

int32_t ribbon_glyph_cols(const Command* cmd, bool icons)
{
    if (!cmd)
        return 0;
    if (icons && cmd->icon)
        return 2;
    return text_cols(cmd->glyph ? cmd->glyph : cmd->label);
}

int32_t ribbon_button_cols(const Command* cmd, int32_t scale, bool icons)
{
    if (!cmd)
        return 0;
    if (scale < 1)
        scale = 1;
    int32_t cs = ribbon_caption_scale(scale);
    int32_t glyph = ribbon_glyph_cols(cmd, icons) * scale + 2;
    int32_t caption = text_cols(cmd->caption ? cmd->caption : cmd->label) * cs;
    if (cmd->key == DAWN_KEY_AI_MODEL && caption < RIBBON_MODEL_COLS * cs)
        caption = RIBBON_MODEL_COLS * cs;
    return glyph > caption ? glyph : caption;
}

int32_t ribbon_buttons(RibbonTab tab, int32_t scale, bool icons, RibbonButton* out, int32_t max)
{
    int32_t n = 0;
    int32_t section = -1;
    for (int32_t s = 0; s < SECTION_COUNT; s++) {
        const RibbonSection* sec = &SECTIONS[s];
        if (sec->tab != tab)
            continue;
        int32_t before = n;
        if (sec->groups[0] >= 0) {
            for (int32_t i = 0; i < COMMAND_COUNT && n < max; i++) {
                if ((int32_t)COMMANDS[i].group == sec->groups[0] || (int32_t)COMMANDS[i].group == sec->groups[1])
                    out[n++] = (RibbonButton) { .cmd = &COMMANDS[i], .section = section + 1, .width = 0 };
            }
        } else {
            for (int32_t k = 0; k < 6 && sec->keys[k] && n < max; k++) {
                const Command* c = commands_find(sec->keys[k]);
                if (c) // read aloud only where speech is compiled in
                    out[n++] = (RibbonButton) { .cmd = c, .section = section + 1, .width = 0 };
            }
        }
        if (n > before)
            section++;
    }
    for (int32_t i = 0; i < n; i++)
        out[i].width = ribbon_button_cols(out[i].cmd, scale, icons);
    return n;
}

int32_t ribbon_tab_of(const Command* cmd)
{
    if (!cmd)
        return -1;
    RibbonTab tabs[RIBBON_TAB_COUNT];
    int32_t nt = tabs_items(tabs, RIBBON_TAB_COUNT);
    RibbonButton b[RIBBON_MAX];
    for (int32_t t = 0; t < nt; t++) {
        int32_t n = ribbon_buttons(tabs[t], 1, false, b, RIBBON_MAX);
        for (int32_t i = 0; i < n; i++) {
            if (b[i].cmd == cmd)
                return (int32_t)tabs[t];
        }
    }
    return -1;
}

int32_t ribbon_more_cols(int32_t scale)
{
    if (scale < 1)
        scale = 1;
    int32_t glyph = 1 * scale + 2; // "▸"
    int32_t caption = 4 * ribbon_caption_scale(scale); // "more"
    return glyph > caption ? glyph : caption;
}

//! Whether button i opens a unit a row would rather not split: a section, or one of the format
//! group's clusters (headings, lists with quote, inserts)
static bool ribbon_unit_start(const RibbonButton* b, int32_t i)
{
    if (i == 0 || b[i].section != b[i - 1].section)
        return true;
    int32_t key = b[i].cmd ? b[i].cmd->key : 0;
    return key == DAWN_KEY_FMT_H1 || key == DAWN_KEY_FMT_BULLET || key == DAWN_KEY_FMT_LINK;
}

//! The space before button i on a row that already holds i - 1: a divider between two sections
static int32_t ribbon_space_before(const RibbonButton* b, int32_t i)
{
    return i > 0 && b[i].section != b[i - 1].section ? RIBBON_DIVIDER : RIBBON_GAP;
}

//! Lay the buttons out in rows of room cells, row r's slots getting row r. With units, a unit that
//! no longer fits on a row but fits on one of its own starts the next row.
//! @return the rows used, 0 when a button is wider than room
static int32_t ribbon_pack(const RibbonButton* b, int32_t n, int32_t room, bool units, RibbonSlot* slots)
{
    int32_t row = 0;
    int32_t end = 0; // where the row's last button ends; 0 on an empty row
    for (int32_t i = 0; i < n; i++) {
        int32_t w = b[i].width;
        if (w > room || w <= 0)
            return 0;
        int32_t sp = ribbon_space_before(b, i);
        if (end > 0 && units && ribbon_unit_start(b, i)) {
            int32_t uw = w;
            for (int32_t j = i + 1; j < n && !ribbon_unit_start(b, j); j++)
                uw += RIBBON_GAP + b[j].width;
            if (end + sp + uw > room && uw <= room) {
                row++;
                end = 0;
            }
        }
        if (end > 0 && end + sp + w > room) {
            row++;
            end = 0;
        }
        int32_t at = end > 0 ? end + sp : 0;
        slots[i] = (RibbonSlot) { .page = 0, .row = row, .col = at, .width = w };
        end = at + w;
    }
    return n > 0 ? row + 1 : 0;
}

//! ribbon_pack keeping units together, or packed wherever they fit when that takes fewer rows
static int32_t ribbon_pack_best(const RibbonButton* b, int32_t n, int32_t room, RibbonSlot* slots)
{
    RibbonSlot packed[RIBBON_MAX];
    int32_t rows = ribbon_pack(b, n, room, true, slots);
    if (rows <= 0)
        return 0;
    int32_t tight = ribbon_pack(b, n, room, false, packed);
    if (tight > 0 && tight < rows) {
        rows = tight;
        for (int32_t i = 0; i < n; i++)
            slots[i] = packed[i];
    }
    return rows;
}

bool ribbon_layout(const RibbonButton* buttons, int32_t n, int32_t room, int32_t scale, int32_t max_height,
    RibbonSlot* slots, RibbonFrame* frame)
{
    *frame = (RibbonFrame) { 0 };
    if (scale < 1)
        scale = 1;
    if (n > RIBBON_MAX)
        n = RIBBON_MAX;
    int32_t h = ribbon_button_rows(scale);
    if (n <= 0 || max_height < h)
        return false;
    int32_t rows = ribbon_pack_best(buttons, n, room, slots);
    if (rows <= 0)
        return false;
    int32_t pages = 1;
    int32_t per_page = rows;
    if (rows * h > max_height) {
        // Too tall: as many rows as fit, paged, each row narrowed by the paging button.
        per_page = max_height / h;
        int32_t more = ribbon_more_cols(scale);
        int32_t narrow = room - more - RIBBON_GAP;
        if (narrow <= 0)
            return false;
        rows = ribbon_pack_best(buttons, n, narrow, slots);
        if (rows <= 0)
            return false;
        pages = (rows + per_page - 1) / per_page;
        frame->more_col = room - more;
        frame->more_width = more;
    }
    for (int32_t i = 0; i < n; i++) {
        int32_t r = slots[i].row;
        slots[i].page = r / per_page;
        slots[i].row = (r % per_page) * h;
    }
    frame->rows = per_page;
    frame->pages = pages;
    frame->height = per_page * h;
    if (pages == 1) {
        frame->more_col = 0;
        frame->more_width = 0;
    }
    return true;
}

// #endregion

// #region Command menu

void menu_build(MenuState* m, bool (*enabled)(const Command*))
{
    m->count = 0;
    m->selected = -1;
    m->scroll = 0;
    m->page = 0;
    int32_t last_group = -1;
    for (int32_t i = 0; i < COMMAND_COUNT; i++) {
        const Command* c = &COMMANDS[i];
        if (c->group == CMD_GROUP_FORMAT)
            continue; // the Formatting ribbon is their place
        if (c->key == DAWN_KEY_CMD_MENU)
            continue; // the menu is open already; its key shows in its title
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
    { HELP_ROW_HEADING, "move and select", "", "" },
    { HELP_ROW_ITEM, "move the cursor", "arrows", "tap" },
    { HELP_ROW_ITEM, "jump a word", "^arrows", "" },
    { HELP_ROW_ITEM, "line start or end", "home/end", "" },
    { HELP_ROW_ITEM, "scroll", "pgup/pgdn", "swipe" },
    { HELP_ROW_ITEM, "select", "shift+arrows", "hold, drag" },
    { HELP_ROW_ITEM, "select a word", "", "double tap" },
    { HELP_ROW_ITEM, "select a paragraph", "", "triple tap" },
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
            out[n++] = (HelpRow) { .kind = HELP_ROW_BLANK, .text = "", .key = "", .tap = "" };
            out[n++] = (HelpRow) { .kind = HELP_ROW_HEADING, .text = commands_group_name(c->group), .key = "", .tap = "" };
            last_group = (int32_t)c->group;
        }
        if (n >= max)
            break;
        int32_t tab = ribbon_tab_of(c);
        out[n++] = (HelpRow) { .kind = HELP_ROW_ITEM, .text = c->label, .key = c->shortcut, .tap = tab >= 0 ? tabs_tap((RibbonTab)tab) : "" };
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
