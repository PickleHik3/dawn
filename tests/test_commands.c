// test_commands.c - host tests for the command table (dawn_commands.c): every entry complete,
// keys unique, groups in display order, every command on a ribbon, the UI scale from the pane's
// pixels, the tab row's layout and status text, the ribbons' buttons and layout (at every scale
// kitty text sizing draws them at and every width from a narrow phone to a wide pane, paging when
// short), the full menu's lines, stepping and scrolling, and the help pages' lines and page
// breaks. Built with USE_LIBAI and DAWN_HAS_SPEAK so the fullest table is checked.

#include "dawn_backend.h"
#include "dawn_commands.h"

#include <stdio.h>
#include <string.h>

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond)                                                         \
    do {                                                                    \
        tests_run++;                                                        \
        if (!(cond)) {                                                      \
            tests_failed++;                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                   \
    } while (0)

//! A key with no physical chord, which may print no shortcut
static bool is_virtual(int32_t key)
{
    return key == DAWN_KEY_CMD_MENU || key == DAWN_KEY_CMD_CHEATSHEET || key == DAWN_KEY_CMD_TOUR
        || key == DAWN_KEY_CMD_THEME || key == DAWN_KEY_AI_MODEL || key == DAWN_KEY_AI_TITLE
        || key == DAWN_KEY_AI_FORMAT || (key >= DAWN_KEY_FMT_BOLD && key <= DAWN_KEY_FMT_CODE_BLOCK);
}

//! Code points of s
static int32_t cols(const char* s)
{
    int32_t w = 0;
    for (const char* p = s; *p; p++)
        w += ((unsigned char)*p & 0xC0) != 0x80;
    return w;
}

static void test_entries(void)
{
    int32_t n = 0;
    const Command* all = commands_all(&n);
    CHECK(all != NULL);
    CHECK(n > 20);
    int32_t last_group = 0;
    for (int32_t i = 0; i < n; i++) {
        const Command* c = &all[i];
        CHECK(c->label && c->label[0]);
        CHECK(c->key != 0 && c->key != DAWN_KEY_NONE);
        CHECK(c->shortcut != NULL);
        CHECK(c->shortcut[0] || is_virtual(c->key));
        CHECK(c->group < CMD_GROUP_COUNT);
        CHECK((int32_t)c->group >= last_group); // grouped, groups in display order
        last_group = (int32_t)c->group;
        for (int32_t j = i + 1; j < n; j++)
            CHECK(all[j].key != c->key);
        // Labels and captions are lower case, as dawn writes its hints.
        for (const char* p = c->label; *p; p++)
            CHECK(!(*p >= 'A' && *p <= 'Z'));
        CHECK(c->caption && c->caption[0] && cols(c->caption) <= 9); // a ribbon's buttons are as wide as its widest
        for (const char* p = c->caption; *p; p++)
            CHECK(!(*p >= 'A' && *p <= 'Z'));
        // Every button has a face of its own, short, and never bare Markdown syntax.
        CHECK(c->glyph && c->glyph[0] && cols(c->glyph) <= 3);
        CHECK(!strchr(c->glyph, '`') && !strchr(c->glyph, '[') && !strchr(c->glyph, '#'));
        CHECK(strcmp(c->glyph, ">") != 0 && strcmp(c->glyph, "-") != 0 && strcmp(c->glyph, "*") != 0);
        CHECK(!strstr(c->glyph, "--") && !strstr(c->glyph, "**"));
        CHECK(c->icon == 0 || (c->icon >= 0xF0001 && c->icon <= 0xF1AF0)); // nf-md-*
        CHECK(commands_find(c->key) == c);
        CHECK(tabs_from_key(c->key) < 0); // the tabs are not commands
        CHECK(c->key != DAWN_KEY_RIBBON_MORE);
        // Every command is on some ribbon: nothing is reachable from the keyboard alone.
        CHECK(ribbon_tab_of(c) >= 0);
    }
    CHECK(commands_find('q') == NULL);
    CHECK(commands_find(26) && strcmp(commands_find(26)->label, "undo") == 0);
    CHECK(commands_find(19) && strcmp(commands_find(19)->shortcut, "^S") == 0);
    CHECK(commands_find(DAWN_KEY_CMD_MENU) != NULL);
    CHECK(commands_find(DAWN_KEY_CMD_THEME) && commands_find(DAWN_KEY_CMD_THEME)->group == CMD_GROUP_VIEW);
    CHECK(commands_find(DAWN_KEY_CMD_CHEATSHEET) != NULL);
    CHECK(commands_find(DAWN_KEY_AI_MODEL) && commands_find(DAWN_KEY_AI_MODEL)->group == CMD_GROUP_AI);
    CHECK(commands_find(DAWN_KEY_AI_TITLE) && commands_find(DAWN_KEY_AI_FORMAT));
    for (int32_t g = 0; g < CMD_GROUP_COUNT; g++)
        CHECK(commands_group_name((CommandGroup)g)[0]);
}

static void test_ui_scale(void)
{
    // A 1080-pixel-wide phone: the chrome's row is about 1080 / 13 = 83 pixels whatever the font.
    CHECK(commands_ui_scale(53, 20, 44, true) == 2); // the normal font: 83 / 44 rounds to 2
    CHECK(commands_ui_scale(120, 9, 19, true) == 4); // pinched small: 83 / 19 rounds to 4
    CHECK(commands_ui_scale(80, 13, 28, true) == 3); // between: 80 / 28 rounds to 3
    CHECK(commands_ui_scale(30, 36, 80, true) == 1); // a large font: 83 / 80 rounds to 1
    CHECK(commands_ui_scale(200, 5, 11, true) == UI_SCALE_MAX); // 7, held at the most
    CHECK(commands_ui_scale(10, 10, 200, false) == 1); // never below 1
    // Rounding: exactly half way rounds up, just under rounds down.
    CHECK(commands_ui_scale(13, 3, 2, false) == 2); // 1.5
    CHECK(commands_ui_scale(13, 29, 20, false) == 1); // 1.45
    // No pixel size: 2 in Termux Launcher's terminal, 1 elsewhere.
    CHECK(commands_ui_scale(53, 0, 0, true) == 2);
    CHECK(commands_ui_scale(53, 0, 0, false) == 1);
    CHECK(commands_ui_scale(53, 20, 0, true) == 2);
    CHECK(commands_ui_scale(0, 20, 44, false) == 1);
    // A 1080-pixel pane at any font: below the cap the chrome's row on screen (scale x cell
    // height) is within half a cell of a thirteenth of the width.
    for (int32_t cell_h = 8; cell_h <= 80; cell_h++) {
        int32_t cell_w = cell_h / 2;
        int32_t cols = 1080 / cell_w;
        int32_t scale = commands_ui_scale(cols, cell_w, cell_h, true);
        CHECK(scale >= 1 && scale <= UI_SCALE_MAX);
        int32_t px = scale * cell_h;
        int32_t want = cols * cell_w / 13;
        if (scale < UI_SCALE_MAX)
            CHECK(px - want <= cell_h / 2 + 1 && want - px <= cell_h / 2 + 1);
    }
}

static void test_tabs(void)
{
    RibbonTab items[RIBBON_TAB_COUNT + 2];
    int32_t n = tabs_items(items, RIBBON_TAB_COUNT + 2);
    CHECK(n == RIBBON_TAB_COUNT); // the AI tab too, in this build
    static const char* const NAMES[] = { "File", "Formatting", "AI" };
    for (int32_t i = 0; i < n && i < 3; i++) {
        CHECK(items[i] == (RibbonTab)i);
        CHECK(strcmp(tabs_name(items[i]), NAMES[i]) == 0);
        CHECK(tabs_key(items[i]) == DAWN_KEY_TAB_FILE + i);
        CHECK(tabs_from_key(tabs_key(items[i])) == i);
        CHECK(strstr(tabs_tap(items[i]), NAMES[i]) == tabs_tap(items[i]));
    }
    CHECK(strcmp(tabs_tap(RIBBON_TAB_FORMAT), "Formatting ribbon") == 0);
    CHECK(tabs_from_key(DAWN_KEY_CMD_MENU) == -1 && tabs_from_key('f') == -1);
    CHECK(tabs_from_key(DAWN_KEY_RIBBON_MORE) == -1);
    CHECK(tabs_items(items, 2) == 2);
    n = tabs_items(items, RIBBON_TAB_COUNT);

    TabSlot s[RIBBON_TAB_COUNT];
    // Scale 1: " File  Formatting  AI " is 6 + 12 + 4 cells, the tabs touching.
    CHECK(tabs_layout(items, n, 1, 22, s) == 22);
    CHECK(s[0].col == 0 && s[0].width == 6 && s[1].col == 6 && s[1].width == 12 && s[2].col == 18);
    CHECK(tabs_layout(items, n, 1, 21, s) == 0);
    // Scale 2 on the phone (53 columns, the row from column 2): 10 + 22 + 6 of 51.
    CHECK(tabs_layout(items, n, 2, 51, s) == 38);
    CHECK(s[1].col == 10 && s[1].width == 22 && s[2].col == 32 && s[2].width == 6);
    CHECK(tabs_layout(items, n, 3, 51, s) == 0); // 54
    CHECK(tabs_scale(items, n, 3, 51) == 2);
    CHECK(tabs_scale(items, n, 4, 200) == 4);
    CHECK(tabs_scale(items, n, 2, 10) == 1); // nothing fits: the smallest scale
    // At every scale and width, tabs_scale's scale fits (or is 1), and the tabs touch in order.
    for (int32_t scale = 1; scale <= UI_SCALE_MAX; scale++) {
        for (int32_t room = 20; room <= 140; room++) {
            int32_t at = tabs_scale(items, n, scale, room);
            CHECK(at >= 1 && at <= scale);
            int32_t used = tabs_layout(items, n, at, room, s);
            CHECK(used > 0 || at == 1);
            if (at < scale)
                CHECK(tabs_layout(items, n, at + 1, room, s) == 0);
            used = tabs_layout(items, n, at, room, s);
            for (int32_t i = 0; i < n && used > 0; i++) {
                CHECK(s[i].width == cols(tabs_name(items[i])) * at + 2 * TAB_PAD);
                CHECK(s[i].col == (i > 0 ? s[i - 1].col + s[i - 1].width : 0));
                CHECK(s[i].col + s[i].width <= room);
            }
        }
    }
}

static void test_tabs_status(void)
{
    char out[96];
    CHECK(tabs_status(120, "4:59", NULL, 40, out, sizeof(out)) == 16 && strcmp(out, "120 words · 4:59") == 0);
    CHECK(tabs_status(120, "4:59", NULL, 15, out, sizeof(out)) == 11 && strcmp(out, "120w · 4:59") == 0);
    CHECK(tabs_status(120, "4:59", NULL, 6, out, sizeof(out)) == 4 && strcmp(out, "4:59") == 0);
    CHECK(tabs_status(120, "4:59", NULL, 3, out, sizeof(out)) == 0 && out[0] == '\0');
    CHECK(tabs_status(120, NULL, NULL, 9, out, sizeof(out)) == 9 && strcmp(out, "120 words") == 0);
    CHECK(tabs_status(120, "", NULL, 8, out, sizeof(out)) == 4 && strcmp(out, "120w") == 0);
    CHECK(tabs_status(1, NULL, NULL, 20, out, sizeof(out)) == 6 && strcmp(out, "1 word") == 0);
    CHECK(tabs_status(0, NULL, NULL, 20, out, sizeof(out)) == 7 && strcmp(out, "0 words") == 0);
    // The alert comes first and goes last.
    CHECK(tabs_status(120, "4:59", "not saved", 40, out, sizeof(out)) == 28);
    CHECK(strcmp(out, "not saved · 120 words · 4:59") == 0);
    CHECK(tabs_status(120, NULL, "not saved", 16, out, sizeof(out)) == 16 && strcmp(out, "not saved · 120w") == 0);
    CHECK(tabs_status(120, "4:59", "not saved", 10, out, sizeof(out)) == 9 && strcmp(out, "not saved") == 0);
    CHECK(tabs_status(120, "4:59", "not saved", 0, out, sizeof(out)) == 0 && out[0] == '\0');
    for (int32_t room = 0; room <= 40; room++) {
        int32_t w = tabs_status(12345, "12:34", "saving paused", room, out, sizeof(out));
        CHECK(w == cols(out) && w <= room);
        CHECK(w == 0 || strncmp(out, "saving paused", 13) == 0);
    }
}

//! Whether every button of ribbon tab is placed inside room at scale, in order, apart, inside the
//! frame; or, when the layout said no, that it truly could not.
static void check_ribbon(RibbonTab tab, int32_t room, int32_t scale, bool icons, int32_t max_height)
{
    RibbonButton b[RIBBON_MAX];
    RibbonSlot s[RIBBON_MAX];
    RibbonFrame f;
    int32_t n = ribbon_buttons(tab, scale, icons, b, RIBBON_MAX);
    CHECK(n > 0);
    int32_t h = ribbon_button_rows(scale);
    bool ok = ribbon_layout(b, n, room, scale, max_height, s, &f);
    int32_t widest = 0;
    for (int32_t i = 0; i < n; i++)
        widest = b[i].width > widest ? b[i].width : widest;
    if (!ok) {
        CHECK(f.height == 0 && f.pages == 0);
        CHECK(max_height < h || widest > room - ribbon_more_cols(scale) - RIBBON_GAP);
        return;
    }
    CHECK(f.pages >= 1 && f.rows >= 1);
    CHECK(f.height == f.rows * h && f.height <= max_height);
    int32_t right = room;
    if (f.pages > 1) {
        CHECK(f.more_width == ribbon_more_cols(scale) && f.more_col == room - f.more_width);
        right = f.more_col - RIBBON_GAP;
    } else {
        CHECK(f.more_width == 0);
    }
    for (int32_t i = 0; i < n; i++) {
        CHECK(s[i].width == b[i].width);
        CHECK(s[i].col >= 0 && s[i].col + s[i].width <= right);
        CHECK(s[i].page >= 0 && s[i].page < f.pages);
        CHECK(s[i].row >= 0 && s[i].row % h == 0 && s[i].row + h <= f.height);
        if (i == 0)
            continue;
        bool same = s[i].page == s[i - 1].page && s[i].row == s[i - 1].row;
        int32_t space = b[i].section != b[i - 1].section ? RIBBON_DIVIDER : RIBBON_GAP;
        if (same)
            CHECK(s[i].col == s[i - 1].col + s[i - 1].width + space);
        else
            CHECK(s[i].col == 0 && (s[i].page > s[i - 1].page || (s[i].page == s[i - 1].page && s[i].row > s[i - 1].row)));
        CHECK(b[i].section == b[i - 1].section || b[i].section == b[i - 1].section + 1);
    }
    // When one page holds everything, its rows follow one another.
    if (f.pages == 1) {
        for (int32_t i = 1; i < n; i++) {
            if (s[i].row != s[i - 1].row)
                CHECK(s[i].row == s[i - 1].row + h); // rows follow one another, none left empty
        }
    }
}

static void test_ribbons(void)
{
    // Captions sit under glyphs at half their scale, so a button is 2 rows at scale 1, 3 at 2
    // (about 43dp on the phone), 4 at 3 and 6 at 4.
    CHECK(ribbon_caption_scale(1) == 1 && ribbon_caption_scale(2) == 1 && ribbon_caption_scale(3) == 1);
    CHECK(ribbon_caption_scale(4) == 2);
    CHECK(ribbon_button_rows(1) == 2 && ribbon_button_rows(2) == 3 && ribbon_button_rows(3) == 4);
    CHECK(ribbon_button_rows(4) == 6);

    // The Formatting ribbon: the format group, one button per format key in FormatOp order, then help.
    RibbonButton b[RIBBON_MAX];
    int32_t n = ribbon_buttons(RIBBON_TAB_FORMAT, 2, true, b, RIBBON_MAX);
    int32_t styles = DAWN_KEY_FMT_CODE_BLOCK - DAWN_KEY_FMT_BOLD + 1;
    CHECK(n == styles + 4);
    for (int32_t i = 0; i < n; i++) {
        CHECK(b[i].section == (i < styles ? 0 : 1));
        if (i < styles)
            CHECK(b[i].cmd->key == DAWN_KEY_FMT_BOLD + i && b[i].cmd->group == CMD_GROUP_FORMAT);
        else
            CHECK(b[i].cmd->group == CMD_GROUP_HELP);
    }
    CHECK(b[styles].cmd->key == 15 && b[n - 1].cmd->key == DAWN_KEY_CMD_MENU);
    // Widths: an icon at scale 2 is 4 cells and a cell either side; a longer caption widens it.
    CHECK(b[0].width == 6); // bold: icon 6, caption 4
    CHECK(b[4].width == 9); // heading 1: its caption
    CHECK(ribbon_glyph_cols(b[0].cmd, true) == 2 && ribbon_glyph_cols(b[0].cmd, false) == 1);
    CHECK(ribbon_button_cols(b[0].cmd, 1, false) == 4); // "bold" under "B" and its two cells
    CHECK(ribbon_button_cols(b[3].cmd, 2, false) == 8); // "</>" at 2, and a cell either side
    CHECK(ribbon_button_cols(b[0].cmd, 4, true) == 10 && ribbon_button_cols(b[4].cmd, 4, true) == 18);

    // File: the note group, edit and find, then view; AI: the chat and the model, then the actions.
    n = ribbon_buttons(RIBBON_TAB_FILE, 1, false, b, RIBBON_MAX);
    CHECK(n == 5 + 15 + 4);
    CHECK(b[0].cmd->key == 7 && b[0].section == 0 && b[4].cmd->key == '\x1b');
    CHECK(b[5].cmd->key == 26 && b[5].section == 1 && b[19].cmd->key == 14 && b[19].section == 1);
    CHECK(b[20].cmd->key == 6 && b[20].section == 2 && b[23].cmd->key == DAWN_KEY_CMD_THEME);
    n = ribbon_buttons(RIBBON_TAB_AI, 2, true, b, RIBBON_MAX);
    CHECK(n == 6);
    CHECK(b[0].cmd->key == 31 && b[1].cmd->key == DAWN_KEY_AI_MODEL && b[1].section == 0);
    CHECK(b[2].cmd->key == 26 && b[2].section == 1 && b[3].cmd->key == DAWN_KEY_AI_TITLE);
    CHECK(b[4].cmd->key == DAWN_KEY_AI_FORMAT && b[5].cmd->key == 17);
    CHECK(b[1].width == RIBBON_MODEL_COLS); // the model's name at the caption's scale
    CHECK(ribbon_tab_of(commands_find(26)) == RIBBON_TAB_FILE); // undo's first ribbon is File's
    CHECK(ribbon_tab_of(commands_find(DAWN_KEY_FMT_BOLD)) == RIBBON_TAB_FORMAT);
    CHECK(ribbon_tab_of(commands_find(15)) == RIBBON_TAB_FORMAT);
    CHECK(ribbon_tab_of(commands_find(17)) == RIBBON_TAB_AI);
    CHECK(ribbon_tab_of(NULL) == -1);

    // The phone at scale 2 (51 cells): packed, the Formatting ribbon takes four rows of three
    // screen rows each, all on one page when there is room.
    RibbonSlot s[RIBBON_MAX];
    RibbonFrame f;
    n = ribbon_buttons(RIBBON_TAB_FORMAT, 2, true, b, RIBBON_MAX);
    CHECK(ribbon_layout(b, n, 51, 2, 40, s, &f));
    CHECK(f.rows == 4 && f.height == 12 && f.pages == 1 && f.more_width == 0);
    CHECK(s[0].col == 0 && s[1].col == 7 && s[3].col == 21 && s[4].col == 28 && s[4].row == 0);
    CHECK(s[styles].col == s[styles - 1].col + s[styles - 1].width + RIBBON_DIVIDER);
    // With the keyboard up, room for two rows: the ribbon pages, a "more" button at the right.
    CHECK(ribbon_layout(b, n, 51, 2, 7, s, &f));
    CHECK(f.rows == 2 && f.height == 6 && f.pages >= 2 && f.more_width == 4 && f.more_col == 47);
    // Not even one row: no ribbon.
    CHECK(!ribbon_layout(b, n, 51, 2, 2, s, &f) && f.height == 0);
    // A wide pane holds the whole ribbon on one row.
    n = ribbon_buttons(RIBBON_TAB_FORMAT, 1, false, b, RIBBON_MAX);
    CHECK(ribbon_layout(b, n, 200, 1, 40, s, &f) && f.rows == 1 && f.height == 2);

    // Every tab at every scale, width and height, with and without the Nerd Font.
    RibbonTab tabs[RIBBON_TAB_COUNT];
    int32_t nt = tabs_items(tabs, RIBBON_TAB_COUNT);
    static const int32_t HEIGHTS[] = { 1, 3, 6, 9, 14, 40 };
    for (int32_t t = 0; t < nt; t++)
        for (int32_t scale = 1; scale <= UI_SCALE_MAX; scale++)
            for (int32_t room = 30; room <= 120; room++)
                for (int32_t icons = 0; icons < 2; icons++)
                    for (size_t hi = 0; hi < sizeof(HEIGHTS) / sizeof(HEIGHTS[0]); hi++)
                        check_ribbon(tabs[t], room, scale, icons != 0, HEIGHTS[hi]);
    // The phone's widths at the phone's scale always show the ribbon with room for one row.
    for (int32_t t = 0; t < nt; t++)
        for (int32_t room = 30; room <= 120; room++) {
            n = ribbon_buttons(tabs[t], 2, true, b, RIBBON_MAX);
            CHECK(ribbon_layout(b, n, room, 2, 3, s, &f));
        }
}

static bool all_on(const Command* c)
{
    (void)c;
    return true;
}

static bool no_edit(const Command* c)
{
    return c->group != CMD_GROUP_NOTE && c->group != CMD_GROUP_EDIT;
}

static bool none_on(const Command* c)
{
    (void)c;
    return false;
}

static void test_menu(void)
{
    MenuState m;
    menu_build(&m, all_on);
    int32_t n = 0;
    const Command* all = commands_all(&n);
    // Every command but the menu's own and the format group's, plus a heading per group but format.
    int32_t styles = 0;
    for (int32_t i = 0; i < n; i++)
        styles += all[i].group == CMD_GROUP_FORMAT;
    CHECK(m.count == n - 1 - styles + CMD_GROUP_COUNT - 1);
    CHECK(m.lines[0].cmd == NULL && m.lines[0].group == CMD_GROUP_NOTE);
    CHECK(m.selected == 1);
    for (int32_t i = 0; i < m.count; i++) {
        CHECK(!m.lines[i].cmd || m.lines[i].cmd->key != DAWN_KEY_CMD_MENU);
        CHECK(m.lines[i].group != CMD_GROUP_FORMAT);
    }

    // Stepping skips headings and stops at the ends.
    CHECK(menu_step(&m, 1, -1, 1) == 1);
    int32_t last = m.count - 1;
    CHECK(menu_step(&m, last, 1, 1) == last);
    int32_t first_edit = -1;
    for (int32_t i = 0; i < m.count && first_edit < 0; i++)
        if (m.lines[i].cmd && m.lines[i].group == CMD_GROUP_EDIT)
            first_edit = i;
    CHECK(first_edit > 0 && m.lines[first_edit - 1].cmd == NULL);
    CHECK(menu_step(&m, first_edit - 2, 1, 1) == first_edit);
    CHECK(menu_step(&m, first_edit, -1, 1) == first_edit - 2);
    CHECK(menu_step(&m, 1, 1, 1000) == last);

    // Disabled commands are skipped and not selected first.
    menu_build(&m, no_edit);
    CHECK(m.selected > 0 && m.lines[m.selected].group == CMD_GROUP_VIEW);
    CHECK(menu_step(&m, m.selected, -1, 1) == m.selected);
    menu_build(&m, none_on);
    CHECK(m.selected == -1);

    // Scrolling keeps the selection in view, with its heading when it opens a group.
    menu_build(&m, all_on);
    m.selected = last;
    menu_scroll_to(&m, 7);
    CHECK(m.scroll == m.count - 7);
    m.selected = first_edit;
    menu_scroll_to(&m, 7);
    CHECK(m.scroll == first_edit - 1);
    m.selected = 1;
    menu_scroll_to(&m, 7);
    CHECK(m.scroll == 0);
    menu_scroll_to(&m, 200);
    CHECK(m.scroll == 0);

    // The widest line of the full menu: "delete to line start" and "^U" two cells apart.
    CHECK(menu_list_cols(&m) == 24);
}

static void test_help_rows(void)
{
    HelpRow rows[HELP_ROWS_MAX];
    int32_t n = help_rows(rows, HELP_ROWS_MAX);
    CHECK(n > 40 && n < HELP_ROWS_MAX);
    CHECK(rows[0].kind == HELP_ROW_HEADING && strcmp(rows[0].text, "move and select") == 0);
    CHECK(rows[n - 1].kind == HELP_ROW_ITEM);

    // Every command is listed once, under its group's heading, with the ribbon it is on.
    int32_t count = 0;
    const Command* all = commands_all(&count);
    for (int32_t c = 0; c < count; c++) {
        int32_t found = 0;
        for (int32_t i = 0; i < n; i++) {
            if (rows[i].kind != HELP_ROW_ITEM || rows[i].text != all[c].label)
                continue;
            found++;
            CHECK(strcmp(rows[i].key, all[c].shortcut) == 0);
            CHECK(strcmp(rows[i].tap, tabs_tap((RibbonTab)ribbon_tab_of(&all[c]))) == 0);
            int32_t h = i;
            while (h >= 0 && rows[h].kind != HELP_ROW_HEADING)
                h--;
            CHECK(h >= 0 && strcmp(rows[h].text, commands_group_name(all[c].group)) == 0);
        }
        CHECK(found == 1);
    }
    for (int32_t i = 0; i < n; i++) {
        if (rows[i].kind != HELP_ROW_ITEM)
            continue;
        if (strcmp(rows[i].text, "undo") == 0 || strcmp(rows[i].text, "light or dark") == 0)
            CHECK(strcmp(rows[i].tap, "File ribbon") == 0);
        if (strcmp(rows[i].text, "bold") == 0 || strcmp(rows[i].text, "keyboard help") == 0)
            CHECK(strcmp(rows[i].tap, "Formatting ribbon") == 0);
        if (strcmp(rows[i].text, "auto format") == 0)
            CHECK(strcmp(rows[i].tap, "AI ribbon") == 0);
    }
    const Command* cheat = commands_find(DAWN_KEY_CMD_CHEATSHEET);
    CHECK(cheat && cheat->group == CMD_GROUP_HELP && strcmp(cheat->label, "markdown cheatsheet") == 0);

    // Groups come in table order, a blank before each heading but the first.
    int32_t headings = 0;
    int32_t last_group = -1;
    for (int32_t i = 1; i < n; i++) {
        if (rows[i].kind != HELP_ROW_HEADING)
            continue;
        headings++;
        CHECK(rows[i - 1].kind == HELP_ROW_BLANK);
        int32_t g = 0;
        while (g < CMD_GROUP_COUNT && strcmp(commands_group_name((CommandGroup)g), rows[i].text) != 0)
            g++;
        CHECK(g < CMD_GROUP_COUNT && g > last_group);
        last_group = g;
    }
    CHECK(headings == CMD_GROUP_COUNT); // every group has commands in the fullest table
    for (int32_t i = 0; i < n; i++)
        CHECK(rows[i].text && rows[i].key && rows[i].tap);
}

static void test_help_paginate(void)
{
    HelpRow rows[HELP_ROWS_MAX];
    int32_t n = help_rows(rows, HELP_ROWS_MAX);
    HelpSpan pages[HELP_ROWS_MAX];
    for (int32_t per = 1; per <= 40; per++) {
        int32_t count = help_paginate(rows, n, per, pages, HELP_ROWS_MAX);
        CHECK(count >= 1);
        int32_t next = 0; // every line but blanks is on exactly one page, in order
        for (int32_t p = 0; p < count; p++) {
            const HelpSpan* s = &pages[p];
            CHECK(s->first < s->end);
            while (next < s->first) {
                CHECK(rows[next].kind == HELP_ROW_BLANK);
                next++;
            }
            CHECK(next == s->first);
            next = s->end;
            CHECK(rows[s->first].kind != HELP_ROW_BLANK);
            CHECK(rows[s->end - 1].kind != HELP_ROW_BLANK);
            CHECK(s->end - s->first + (s->heading >= 0 ? 1 : 0) <= per);
            // Headings never orphan at a page's bottom.
            if (per >= 2)
                CHECK(rows[s->end - 1].kind != HELP_ROW_HEADING);
            // A page that opens inside a group repeats that group's heading.
            int32_t h = s->first - 1;
            while (h >= 0 && rows[h].kind == HELP_ROW_ITEM)
                h--;
            bool inside = rows[s->first].kind == HELP_ROW_ITEM && h >= 0 && rows[h].kind == HELP_ROW_HEADING;
            if (per >= 2)
                CHECK(s->heading == (inside ? h : -1));
            else
                CHECK(s->heading == -1);
        }
        while (next < n && rows[next].kind == HELP_ROW_BLANK)
            next++;
        CHECK(next == n);
    }
    // The phone pane, 24 rows (17 lines) and 12 rows with the keyboard up (8 lines)
    printf("shortcut pages: %d at 17 lines, %d at 8, %d at 19\n", help_paginate(rows, n, 17, pages, HELP_ROWS_MAX),
        help_paginate(rows, n, 8, pages, HELP_ROWS_MAX), help_paginate(rows, n, 19, pages, HELP_ROWS_MAX));
}

static void test_help_note(void)
{
    for (int32_t n = 0; n <= 24; n++) {
        for (int32_t per = 1; per <= 24; per++) {
            int32_t each = -1;
            bool blank = true;
            int32_t pages = help_pages_with_note(n, per, &each, &blank);
            CHECK(pages >= 1);
            CHECK(each >= 0 && each <= per);
            int32_t last = n - (pages - 1) * each;
            CHECK(last >= 0 && last <= (pages > 1 ? each : n));
            CHECK(last + 1 + (blank ? 1 : 0) <= per);
            if (n > 0 && per >= 2)
                CHECK(last >= 1); // the note is never alone where a page holds two lines
            if (n + 1 <= per)
                CHECK(pages == 1);
            if (blank)
                CHECK(last > 0);
        }
    }
    // The cheatsheet's 16 rows: one page at 24 rows (17 lines), three of 6, 6 and 4 at 12 (8 lines).
    int32_t each = 0;
    bool blank = false;
    CHECK(help_pages_with_note(16, 17, &each, &blank) == 1 && !blank);
    CHECK(help_pages_with_note(16, 8, &each, &blank) == 3 && each == 6 && blank);
}

int main(void)
{
    test_entries();
    test_ui_scale();
    test_tabs();
    test_tabs_status();
    test_ribbons();
    test_menu();
    test_help_rows();
    test_help_paginate();
    test_help_note();
    printf("%d/%d checks passed\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}
