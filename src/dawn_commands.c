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
// drops its ribbon under the row: File holds the dozen most used (details, pause, close; undo,
// redo and the clipboard; find, contents, focus, light or dark); Formatting the format group (one
// button per FormatOp), then help; AI the chat and the model, then undo, a title, auto format and
// read aloud. A ribbon's buttons are all as wide as its widest, so they line up as a grid. Two
// sections sit side by side, split by a divider, only when both fit one row; otherwise each starts
// a row of its own under a thin rule. A ribbon pages (a "more" button) when its rows would leave
// the note too little room. Commands on no ribbon are in the full command menu.
// Where the terminal sizes text (kitty's OSC 66) the tab row and the ribbons are drawn at a scale
// chosen from the pane's pixels (commands_ui_scale), so they keep their size on screen when a
// pinch changes the terminal's font; their places (tabs_layout, ribbon_layout) are worked out
// here, in screen cells, so the drawing and the tap regions take the same numbers. The full
// command menu (Ctrl+], or "all commands" in help) lists every command but the format group's
// under its group's heading, the shortcut beside each, so every entry also teaches its key. The
// help pages read the same table: every command by task, with its key and the ribbon it is on (or
// "menu"), so a new command is listed in help by adding it here.
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
    { "code", CMD_GROUP_FORMAT, DAWN_KEY_FMT_CODE, "", CMD_NEEDS_EDIT, "code", "<>", 0xF0174 },
    { "heading 1", CMD_GROUP_FORMAT, DAWN_KEY_FMT_H1, "", CMD_NEEDS_EDIT, "head 1", "H1", 0xF026B },
    { "heading 2", CMD_GROUP_FORMAT, DAWN_KEY_FMT_H2, "", CMD_NEEDS_EDIT, "head 2", "H2", 0xF026C },
    { "heading 3", CMD_GROUP_FORMAT, DAWN_KEY_FMT_H3, "", CMD_NEEDS_EDIT, "head 3", "H3", 0xF026D },
    { "bullet list", CMD_GROUP_FORMAT, DAWN_KEY_FMT_BULLET, "", CMD_NEEDS_EDIT, "bullets", "•", 0xF0279 },
    { "numbered list", CMD_GROUP_FORMAT, DAWN_KEY_FMT_NUMBERED, "", CMD_NEEDS_EDIT, "numbers", "1.", 0xF027B },
    { "task list", CMD_GROUP_FORMAT, DAWN_KEY_FMT_TASK, "", CMD_NEEDS_EDIT, "task", "✓", 0xF0135 },
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
    { "light or dark", CMD_GROUP_VIEW, DAWN_KEY_CMD_THEME, "", 0, "theme", "◐", 0xF050E },

    // find
    { "find", CMD_GROUP_FIND, 19, "^S", 0, "find", "⌕", 0xF0349 },
    { "table of contents", CMD_GROUP_FIND, 12, "^L", 0, "outline", "☰", 0xF0836 },
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
    { "markdown cheatsheet", CMD_GROUP_HELP, DAWN_KEY_CMD_CHEATSHEET, "", 0, "syntax", "M↓", 0xF0354 },
    { "welcome tour", CMD_GROUP_HELP, DAWN_KEY_CMD_TOUR, "", 0, "tour", "★", 0xF02FD },
    { "all commands", CMD_GROUP_HELP, DAWN_KEY_CMD_MENU, "^]", 0, "menu", "☰", 0xF035C },
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

int32_t commands_ui_scale(int32_t cols, int32_t cell_w, int32_t cell_h)
{
    if (cols <= 0 || cell_w <= 0 || cell_h <= 0)
        return 1;
    // round(cols x cell_w / D / cell_h) in integers: (2 x px + D x cell_h) / (2 x D x cell_h)
    int64_t px = (int64_t)cols * cell_w;
    int64_t d = UI_SCALE_DIVISOR;
    int64_t scale = (2 * px + d * cell_h) / (2 * d * cell_h);
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

//! One section of a ribbon: the commands of a group in table order, or a list of keys
typedef struct {
    RibbonTab tab;
    int32_t group; //!< A CommandGroup, or -1 for the keys below
    int32_t keys[6]; //!< When group is -1: these commands in this order, 0 ending the list
} RibbonSection;

//! Every ribbon's sections, tab by tab, left to right. File keeps the dozen most used: the note
//! (details, the timer's pause, close), the clipboard and undo, then finding and seeing; the rest of
//! its groups (select all, the deletes, indent, plain text, ...) are in the full command menu.
static const RibbonSection SECTIONS[] = {
    { RIBBON_TAB_FILE, -1, { 7, 16, '\x1b', 0 } },
    { RIBBON_TAB_FILE, -1, { 26, 25, 24, 3, 22, 0 } },
    { RIBBON_TAB_FILE, -1, { 19, 12, 6, DAWN_KEY_CMD_THEME, 0 } },
    { RIBBON_TAB_FORMAT, CMD_GROUP_FORMAT, { 0 } },
    { RIBBON_TAB_FORMAT, CMD_GROUP_HELP, { 0 } },
    // The AI's: talking to it and which model answers, then what it does to the note (undo is the
    // edit group's own, here too since an AI edit is one undo step).
    { RIBBON_TAB_AI, -1, { 31, DAWN_KEY_AI_MODEL, 0 } },
    { RIBBON_TAB_AI, -1, { 26, DAWN_KEY_AI_TITLE, DAWN_KEY_AI_FORMAT, 17, 0 } },
};

#define SECTION_COUNT ((int32_t)(sizeof(SECTIONS) / sizeof(SECTIONS[0])))

int32_t ribbon_caption_scale(int32_t scale)
{
    return scale / 2 > 1 ? scale / 2 : 1;
}

int32_t ribbon_glyph_rows(int32_t scale, bool sized)
{
    if (scale < 1)
        scale = 1;
    return sized && scale < 2 ? 2 : scale;
}

void ribbon_glyph_frac(int32_t glyph_rows, int32_t* num, int32_t* den)
{
    if (glyph_rows < 2) {
        *num = 0;
        *den = 0;
        return;
    }
    *num = 2 * glyph_rows - 1;
    *den = 2 * glyph_rows;
}

//! a / b rounded down (b > 0), for a of either sign
static int64_t floor_div(int64_t a, int64_t b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

RibbonGlyphPlace ribbon_glyph_place(int32_t w, int32_t block, int32_t num, int32_t den)
{
    RibbonGlyphPlace best = { .col = w > block ? (w - block) / 2 : 0, .halign = 2 };
    if (w <= block)
        return best;
    if (num <= 0 || den <= num) {
        num = 1; // drawn the whole block's size: only h=2 means anything
        den = 1;
    }
    // In units of 1 / (2 den) cells: the box's centre is w den; with the block from cell x the
    // text's centre is 2 den x plus, for h=2, block den; for h=0, block num (half the text's
    // width); for h=1, 2 block den - block num.
    static const int32_t ORDER[3] = { 2, 0, 1 };
    int64_t best_err = -1;
    int64_t unit = 2 * (int64_t)den;
    int64_t target = (int64_t)w * den;
    for (int32_t k = 0; k < 3; k++) {
        int32_t h = ORDER[k];
        if (num == den && h != 2)
            break;
        int64_t off = h == 2 ? (int64_t)block * den : h == 0 ? (int64_t)block * num : 2 * (int64_t)block * den - (int64_t)block * num;
        int64_t x0 = floor_div(target - off, unit);
        for (int64_t x = x0; x <= x0 + 1; x++) {
            if (x < 0 || x > w - block)
                continue;
            int64_t err = unit * x + off - target;
            err = err < 0 ? -err : err;
            if (best_err < 0 || err < best_err) {
                best_err = err;
                best = (RibbonGlyphPlace) { .col = (int32_t)x, .halign = h };
            }
        }
    }
    return best;
}

int32_t ribbon_button_rows(int32_t scale, bool sized)
{
    if (scale < 1)
        scale = 1;
    return ribbon_glyph_rows(scale, sized) + ribbon_caption_scale(scale);
}

int32_t ribbon_row_gap(int32_t scale)
{
    (void)scale; // one row at every scale: a button's caption is inside its box
    return 1;
}

//! Screen rows between a page's row of buttons and the one before it: the gap, or the rule's rows
//! when the row opens a stacked section and they are more
static int32_t ribbon_row_sep(int32_t scale, bool stacked)
{
    int32_t gap = ribbon_row_gap(scale);
    return stacked && RIBBON_RULE > gap ? RIBBON_RULE : gap;
}

int32_t ribbon_glyph_cols(const Command* cmd, bool icons)
{
    if (!cmd)
        return 0;
    if (icons && cmd->icon)
        return 2;
    int32_t w = text_cols(cmd->glyph ? cmd->glyph : cmd->label);
    return w > 2 ? w : 2;
}

int32_t ribbon_button_cols(const Command* cmd, int32_t scale, bool icons, bool sized)
{
    if (!cmd)
        return 0;
    if (scale < 1)
        scale = 1;
    int32_t cs = ribbon_caption_scale(scale);
    int32_t glyph = ribbon_glyph_cols(cmd, icons) * ribbon_glyph_rows(scale, sized) + 2;
    int32_t caption = text_cols(cmd->caption ? cmd->caption : cmd->label) * cs;
    if (cmd->key == DAWN_KEY_AI_MODEL && caption < RIBBON_MODEL_STATE_COLS * cs)
        caption = RIBBON_MODEL_STATE_COLS * cs;
    return glyph > caption ? glyph : caption;
}

int32_t ribbon_buttons(RibbonTab tab, int32_t scale, bool icons, bool sized, RibbonButton* out, int32_t max)
{
    int32_t n = 0;
    int32_t section = -1;
    for (int32_t s = 0; s < SECTION_COUNT; s++) {
        const RibbonSection* sec = &SECTIONS[s];
        if (sec->tab != tab)
            continue;
        int32_t before = n;
        if (sec->group >= 0) {
            for (int32_t i = 0; i < COMMAND_COUNT && n < max; i++) {
                if ((int32_t)COMMANDS[i].group == sec->group)
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
    // One width for the whole ribbon, its widest button's, so the buttons line up as a grid.
    int32_t widest = 0;
    for (int32_t i = 0; i < n; i++) {
        int32_t w = ribbon_button_cols(out[i].cmd, scale, icons, sized);
        widest = w > widest ? w : widest;
    }
    for (int32_t i = 0; i < n; i++)
        out[i].width = widest;
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
        int32_t n = ribbon_buttons(tabs[t], 1, false, false, b, RIBBON_MAX);
        for (int32_t i = 0; i < n; i++) {
            if (b[i].cmd == cmd)
                return (int32_t)tabs[t];
        }
    }
    return -1;
}

int32_t ribbon_more_cols(int32_t scale, bool sized)
{
    if (scale < 1)
        scale = 1;
    int32_t glyph = 1 * ribbon_glyph_rows(scale, sized) + 2; // "▸"
    int32_t caption = 4 * ribbon_caption_scale(scale); // "more"
    return glyph > caption ? glyph : caption;
}

//! Sections a ribbon_pack tries both ways at most (the rest start rows of their own)
#define RIBBON_PACK_CHOICES 8

//! Lay the buttons out in rows of room cells, each section after the first going on after the one
//! before it (on the same row, after a divider) when bit section of goes_on says so and its first
//! button fits there, else starting a row of its own (stacked[row] true from the second row on).
//! A section on rows of its own spreads its buttons evenly over them (sixteen at five a row go
//! four by four, not five, five, five and one); one that goes on fills the row it joins and spreads
//! the rest evenly over the rows under it, from the left edge.
//! @param stacked receives, for each row, whether it opens a section under another (RIBBON_MAX)
//! @param split receives how many sections went on and did not fit the row they joined
//! @param joins receives how many sections went on
//! @return the rows used, 0 when a button is wider than room
static int32_t ribbon_pack_as(const RibbonButton* b, int32_t n, int32_t room, uint32_t goes_on, RibbonSlot* slots,
    bool* stacked, int32_t* split, int32_t* joins)
{
    int32_t row = -1;
    int32_t end = 0; // where the row's last button ends
    int32_t per_row = 0; // buttons a row of this section holds
    int32_t in_row = 0; // buttons on the row so far
    int32_t left = 0; // the section's buttons not placed yet
    bool joined = false; // the section went on after the one before it
    *split = 0;
    *joins = 0;
    for (int32_t i = 0; i < n; i++) {
        int32_t w = b[i].width;
        if (w > room || w <= 0)
            return 0;
        // The buttons of a ribbon are one width, so a row holds fit of them.
        int32_t fit = (room + RIBBON_GAP) / (w + RIBBON_GAP);
        bool opens = i == 0 || b[i].section != b[i - 1].section;
        int32_t at;
        if (opens) {
            int32_t count = 1;
            for (int32_t j = i + 1; j < n && b[j].section == b[i].section; j++)
                count++;
            int32_t section = b[i].section;
            joined = i > 0 && section < 32 && (goes_on >> section & 1u) && end + RIBBON_DIVIDER + w <= room;
            if (joined) {
                (*joins)++;
                at = end + RIBBON_DIVIDER;
                per_row = 1 + (room - at - w) / (w + RIBBON_GAP);
                if (per_row >= count)
                    per_row = count;
                else
                    (*split)++;
            } else {
                row++;
                stacked[row] = i > 0;
                at = 0;
                int32_t rows = (count + fit - 1) / fit;
                per_row = (count + rows - 1) / rows;
            }
            in_row = 0; // the section's buttons on its row
            left = count;
        } else if (in_row >= per_row || end + RIBBON_GAP + w > room) {
            row++;
            stacked[row] = false;
            at = 0;
            in_row = 0;
            if (joined) {
                // What did not fit the row it joined, spread evenly over the rows under it.
                int32_t rows = (left + fit - 1) / fit;
                per_row = (left + rows - 1) / rows;
            }
        } else {
            at = end + RIBBON_GAP;
        }
        slots[i] = (RibbonSlot) { .page = 0, .row = row, .col = at, .width = w, .rule_above = false };
        end = at + w;
        in_row++;
        left--;
    }
    return row + 1;
}

//! Lay the buttons out in rows of room cells (ribbon_pack_as), each section after the first going
//! on after the one before it or starting a row of its own, whichever way takes the fewest rows,
//! then splits the fewest sections over rows, then has the fewest sections going on: a section
//! stays a block unless going on saves a row. On a phone at six buttons a row File's 3 | 5 | 4 go
//! on two rows (3 | 3, then 2 | 4) where blocks would take three.
//! @param stacked receives, for each row, whether it opens a section under another (RIBBON_MAX)
//! @return the rows used, 0 when a button is wider than room
static int32_t ribbon_pack(const RibbonButton* b, int32_t n, int32_t room, RibbonSlot* slots, bool* stacked)
{
    int32_t sections = n > 0 ? b[n - 1].section + 1 : 0;
    int32_t choices = sections - 1 < RIBBON_PACK_CHOICES ? sections - 1 : RIBBON_PACK_CHOICES;
    if (choices < 0)
        choices = 0;
    RibbonSlot try_slots[RIBBON_MAX];
    bool try_stacked[RIBBON_MAX];
    int32_t best_rows = 0, best_split = 0, best_joins = 0;
    for (uint32_t m = 0; m < (1u << choices); m++) {
        int32_t split = 0, joins = 0;
        // Bit k of m is section k + 1 (the first section has nothing to go on after).
        int32_t rows = ribbon_pack_as(b, n, room, m << 1, try_slots, try_stacked, &split, &joins);
        if (rows <= 0)
            return 0;
        bool better = m == 0 || rows < best_rows
            || (rows == best_rows && (split < best_split || (split == best_split && joins < best_joins)));
        if (better) {
            best_rows = rows;
            best_split = split;
            best_joins = joins;
            memcpy(slots, try_slots, (size_t)n * sizeof(RibbonSlot));
            memcpy(stacked, try_stacked, (size_t)rows * sizeof(bool));
        }
    }
    return best_rows;
}

//! Spare cells share k: the j-th of k (0 first) gets its part, the parts adding up to spare and
//! differing by at most one, the larger ones last
static int32_t ribbon_share(int32_t spare, int32_t k, int32_t j)
{
    return k > 0 ? spare * (j + 1) / k - spare * j / k : 0;
}

//! Widen packed rows (slots[].row a row's index, cols from its left edge) to end at right: the
//! spare cells of a row go to its buttons evenly (each a whole number of cells wider, all of a row
//! one width), and what is left over to its gaps, then to its dividers when it has no gaps, or to
//! the one button of a row that has neither. Rows that hold one section only and follow each other
//! (a section spread over rows, or what of a section did not fit the row it went on in) are one
//! grid: all are set as the fullest of them is, so a shorter last row keeps the columns of the rows
//! above it rather than reaching the right edge.
static void ribbon_spread(const RibbonButton* b, int32_t n, int32_t right, RibbonSlot* slots)
{
    int32_t i = 0;
    while (i < n) {
        // The rows of this block: one row, or the run of rows from here holding only b[i]'s section.
        int32_t end = i; // past the block's last button
        int32_t model = i; // the first button of its fullest row
        int32_t model_k = 0;
        bool single = true;
        int32_t r0 = i;
        while (r0 < n) {
            int32_t r1 = r0;
            bool one = true;
            while (r1 < n && slots[r1].row == slots[r0].row) {
                one = one && b[r1].section == b[r0].section;
                r1++;
            }
            if (r0 > i && (!single || !one || b[r0].section != b[i].section))
                break;
            if (r1 - r0 > model_k) {
                model_k = r1 - r0;
                model = r0;
            }
            single = one;
            end = r1;
            r0 = r1;
            if (!one)
                break; // a row of two sections is a block of its own
        }
        // The fullest row's spread: each button e cells wider, the rest of its spare cells (rest)
        // shared out over its gaps, else its dividers, else the one button.
        int32_t k = model_k;
        int32_t last = model + k - 1;
        int32_t spare = right - (slots[last].col + slots[last].width);
        if (spare < 0)
            spare = 0;
        int32_t e = spare / k;
        int32_t rest = spare - e * k;
        int32_t gaps = 0, dividers = 0;
        for (int32_t j = model + 1; j <= last; j++) {
            if (b[j].section == b[j - 1].section)
                gaps++;
            else
                dividers++;
        }
        if (gaps == 0 && dividers == 0) {
            e += rest;
            rest = 0;
        }
        bool to_gaps = gaps > 0;
        int32_t share_k = to_gaps ? gaps : dividers;
        // Where the fullest row's buttons go, by their place in the row.
        int32_t col[RIBBON_MAX];
        int32_t at = 0, seen = 0;
        for (int32_t j = 0; j < k; j++) {
            int32_t x = model + j;
            if (j > 0) {
                int32_t sep = slots[x].col - (slots[x - 1].col + slots[x - 1].width);
                bool gap = b[x].section == b[x - 1].section;
                if (gap == to_gaps)
                    sep += ribbon_share(rest, share_k, seen++);
                at += sep;
            }
            col[j] = at;
            at += slots[x].width + e;
        }
        // Every row of the block set by it.
        for (int32_t x = i; x < end;) {
            int32_t y = x;
            for (int32_t j = 0; y < end && slots[y].row == slots[x].row; j++, y++) {
                slots[y].col = col[j];
                slots[y].width += e;
            }
            x = y;
        }
        i = end;
    }
}

bool ribbon_layout(const RibbonButton* buttons, int32_t n, int32_t room, int32_t scale, bool sized,
    int32_t max_height, RibbonSlot* slots, RibbonFrame* frame)
{
    *frame = (RibbonFrame) { 0 };
    if (scale < 1)
        scale = 1;
    if (n > RIBBON_MAX)
        n = RIBBON_MAX;
    int32_t h = ribbon_button_rows(scale, sized);
    if (n <= 0 || max_height < h)
        return false;
    bool stacked[RIBBON_MAX];
    int32_t rows = ribbon_pack(buttons, n, room, slots, stacked);
    if (rows <= 0)
        return false;
    int32_t total = rows * h;
    for (int32_t r = 1; r < rows; r++)
        total += ribbon_row_sep(scale, stacked[r]);
    if (total > max_height) {
        // Too tall: pages of as many rows as fit, each row narrowed by the paging button.
        int32_t more = ribbon_more_cols(scale, sized);
        int32_t narrow = room - more - RIBBON_GAP;
        if (narrow <= 0)
            return false;
        rows = ribbon_pack(buttons, n, narrow, slots, stacked);
        if (rows <= 0)
            return false;
        frame->more_col = room - more;
        frame->more_width = more;
    }
    // Each row's page and its top on that page, under the gap from the row before it (a rule in it
    // above a stacked row), with neither when the row opens its page.
    int32_t row_page[RIBBON_MAX], row_top[RIBBON_MAX];
    bool row_rule[RIBBON_MAX];
    int32_t page = 0, used = 0, on_page = 0;
    for (int32_t r = 0; r < rows; r++) {
        int32_t sep = on_page > 0 ? ribbon_row_sep(scale, stacked[r]) : 0;
        if (on_page > 0 && used + sep + h > max_height) {
            page++;
            used = 0;
            on_page = 0;
            sep = 0;
        }
        row_page[r] = page;
        row_top[r] = used + sep;
        row_rule[r] = on_page > 0 && stacked[r];
        used += sep + h;
        on_page++;
        if (used > frame->height)
            frame->height = used;
        if (on_page > frame->rows)
            frame->rows = on_page;
    }
    // Every row spread to the edge: the paging button's when it pages, else the room's (rows packed
    // around the paging button that came to one page after all reach the room's edge too).
    frame->pages = page + 1;
    ribbon_spread(buttons, n, frame->pages > 1 ? frame->more_col - RIBBON_GAP : room, slots);
    for (int32_t i = 0; i < n; i++) {
        int32_t r = slots[i].row;
        slots[i].page = row_page[r];
        slots[i].row = row_top[r];
        slots[i].rule_above = row_rule[r] && slots[i].col == 0;
    }
    if (frame->pages == 1) {
        frame->more_col = 0;
        frame->more_width = 0;
    }
    return true;
}

// #endregion

// #region Command menu

bool commands_in_menu(const Command* cmd)
{
    // The format group's place is the Formatting ribbon; the menu's own key is open already (it
    // shows in the menu's title).
    return cmd && cmd->group != CMD_GROUP_FORMAT && cmd->key != DAWN_KEY_CMD_MENU;
}

void menu_build(MenuState* m, bool (*enabled)(const Command*))
{
    m->count = 0;
    m->selected = -1;
    m->scroll = 0;
    m->page = 0;
    int32_t last_group = -1;
    for (int32_t i = 0; i < COMMAND_COUNT; i++) {
        const Command* c = &COMMANDS[i];
        if (!commands_in_menu(c))
            continue;
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
        const char* tap = tab >= 0 ? tabs_tap((RibbonTab)tab) : commands_in_menu(c) ? "menu" : "";
        out[n++] = (HelpRow) { .kind = HELP_ROW_ITEM, .text = c->label, .key = c->shortcut, .tap = tap };
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
