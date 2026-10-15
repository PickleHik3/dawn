// test_commands.c - host tests for the command table (dawn_commands.c): every entry complete,
// keys unique, groups in display order, the UI scale from the pane's pixels, the menu bar's menus
// and layout and the ribbon's layout (at scale 1, and at the scales kitty text sizing draws them
// at), the full menu's and the dropdowns' lines, stepping and scrolling, and the help pages' lines
// and page breaks. Built with USE_LIBAI and DAWN_HAS_SPEAK so the fullest table is checked.

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
    return key == DAWN_KEY_CMD_MENU || key == DAWN_KEY_CMD_RIBBON || key == DAWN_KEY_CMD_CHEATSHEET
        || key == DAWN_KEY_CMD_TOUR || (key >= DAWN_KEY_FMT_BOLD && key <= DAWN_KEY_FMT_CODE_BLOCK);
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
        // Labels are lower case, as dawn writes its hints (menu names and button texts are not labels).
        for (const char* p = c->label; *p; p++)
            CHECK(!(*p >= 'A' && *p <= 'Z'));
        CHECK(commands_find(c->key) == c);
        CHECK((c->button != NULL) == (c->group == CMD_GROUP_FORMAT)); // only ribbon buttons have button text
        CHECK(c->key < DAWN_KEY_MENU_FILE || c->key > DAWN_KEY_MENU_HELP); // the menus are not commands
    }
    CHECK(commands_find('q') == NULL);
    CHECK(commands_find(26) && strcmp(commands_find(26)->label, "undo") == 0);
    CHECK(commands_find(19) && strcmp(commands_find(19)->shortcut, "^S") == 0);
    CHECK(commands_find(DAWN_KEY_CMD_MENU) != NULL);
    CHECK(commands_find(DAWN_KEY_CMD_RIBBON) != NULL);
    CHECK(commands_find(DAWN_KEY_CMD_CHEATSHEET) != NULL);
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

static void test_menubar(void)
{
    MenuBarItem items[MENUBAR_COUNT + 2];
    int32_t n = menubar_items(items, MENUBAR_COUNT + 2);
    CHECK(n == MENUBAR_COUNT); // the AI menu too, in this build
    static const char* const NAMES[] = { "File", "Edit", "View", "AI", "Help" };
    for (int32_t i = 0; i < n && i < 5; i++) {
        CHECK(items[i] == (MenuBarItem)i);
        CHECK(strcmp(menubar_name(items[i]), NAMES[i]) == 0);
        CHECK(menubar_key(items[i]) == DAWN_KEY_MENU_FILE + i);
        CHECK(menubar_from_key(menubar_key(items[i])) == i);
    }
    CHECK(menubar_key(MENUBAR_HELP) == DAWN_KEY_MENU_HELP);
    CHECK(menubar_from_key(DAWN_KEY_CMD_MENU) == -1);
    CHECK(menubar_from_key('f') == -1);
    CHECK(menubar_items(items, 2) == 2);
    n = menubar_items(items, MENUBAR_COUNT);

    // Groups: note under File, edit and find under Edit, then View, AI, Help; format is the ribbon's.
    CHECK(menubar_of_group(CMD_GROUP_NOTE) == MENUBAR_FILE);
    CHECK(menubar_of_group(CMD_GROUP_EDIT) == MENUBAR_EDIT);
    CHECK(menubar_of_group(CMD_GROUP_FIND) == MENUBAR_EDIT);
    CHECK(menubar_of_group(CMD_GROUP_VIEW) == MENUBAR_VIEW);
    CHECK(menubar_of_group(CMD_GROUP_AI) == MENUBAR_AI);
    CHECK(menubar_of_group(CMD_GROUP_HELP) == MENUBAR_HELP);
    CHECK(menubar_of_group(CMD_GROUP_FORMAT) == -1);
    CHECK(strcmp(menubar_tap(CMD_GROUP_EDIT), "Edit menu") == 0);
    CHECK(strcmp(menubar_tap(CMD_GROUP_FIND), "Edit menu") == 0);
    CHECK(strcmp(menubar_tap(CMD_GROUP_VIEW), "View menu") == 0);
    CHECK(strcmp(menubar_tap(CMD_GROUP_FORMAT), "ribbon") == 0);

    // Left and right walk the bar and wrap round.
    CHECK(menubar_neighbour(items, n, MENUBAR_FILE, 1) == MENUBAR_EDIT);
    CHECK(menubar_neighbour(items, n, MENUBAR_FILE, -1) == MENUBAR_HELP);
    CHECK(menubar_neighbour(items, n, MENUBAR_HELP, 1) == MENUBAR_FILE);
    CHECK(menubar_neighbour(items, 3, MENUBAR_HELP, 1) == MENUBAR_HELP); // not among the three
    MenuBarItem no_ai[] = { MENUBAR_FILE, MENUBAR_EDIT, MENUBAR_VIEW, MENUBAR_HELP };
    CHECK(menubar_neighbour(no_ai, 4, MENUBAR_VIEW, 1) == MENUBAR_HELP);

    MenuBarSlot s[MENUBAR_COUNT];
    // Scale 1, a 53-column phone pane less its margins (49 cells): two cells between menus.
    CHECK(menubar_layout(items, n, 1, 49, s) == 5);
    CHECK(s[0].col == 0 && s[1].col == 6 && s[2].col == 12 && s[3].col == 18 && s[4].col == 22);
    CHECK(s[4].width == 4);
    // Scale 2 in the same 49 cells: 18 scaled letters and four gaps of two take 52, so the gap
    // narrows to one scaled cell (44) rather than drop Help.
    CHECK(menubar_layout(items, n, 2, 49, s) == 5);
    CHECK(s[0].col == 0 && s[0].width == 8);
    CHECK(s[1].col == 10 && s[2].col == 20 && s[3].col == 30 && s[3].width == 4 && s[4].col == 36);
    CHECK(s[4].col + s[4].width == 44);
    CHECK(menubar_layout(items, n, 2, 52, s) == 5 && s[1].col == 12); // the full gap fits
    // Without the AI menu the wider gap fits at scale 2: 16 letters and three gaps of two.
    CHECK(menubar_layout(no_ai, 4, 2, 49, s) == 4 && s[3].col == 36 && s[3].col + s[3].width == 44);
    // Scale 4 in a pane pinched to 120 columns (116 cells): 72 + 4 gaps of 8.
    CHECK(menubar_layout(items, n, 4, 116, s) == 5);
    CHECK(s[1].col == 24 && s[4].col == 88 && s[4].col + s[4].width == 104);
    // Too narrow even with one cell: menus drop from the right.
    CHECK(menubar_layout(items, n, 2, 43, s) == 4);
    CHECK(menubar_layout(items, n, 2, 8, s) == 1);
    CHECK(menubar_layout(items, n, 2, 7, s) == 0);

    // At every scale and width the kept menus stay inside the row, keep their order, and a tap
    // region (the name and a cell either side) never overlaps the next one's; a menu is dropped
    // only when it does not fit even the narrow gap (one scaled cell, two cells at scale 1) after
    // the last one kept.
    for (int32_t scale = 1; scale <= UI_SCALE_MAX; scale++) {
        for (int32_t room = 1; room <= 140; room++) {
            int32_t kept = menubar_layout(items, n, scale, room, s);
            CHECK(kept >= 0 && kept <= n);
            for (int32_t i = 0; i < kept; i++) {
                CHECK(s[i].width == (int32_t)strlen(menubar_name(items[i])) * scale);
                CHECK(s[i].col >= 0 && s[i].col + s[i].width <= room);
                if (i > 0)
                    CHECK((s[i].col - 1) - (s[i - 1].col + s[i - 1].width) >= 1);
            }
            if (kept < n) {
                int32_t narrow = 0;
                for (int32_t i = 0; i <= kept; i++)
                    narrow += (int32_t)strlen(menubar_name(items[i])) * scale + (i > 0 ? (scale > 1 ? scale : 2) : 0);
                CHECK(narrow > room);
            }
        }
    }
}

static void test_ribbon(void)
{
    static const char* const LABELS[] = { "bold", "italic", "strikethrough", "code", "heading 1",
        "heading 2", "heading 3", "bullet list", "numbered list", "task list", "quote", "link",
        "image", "table", "horizontal rule", "code block" };
    static const char* const TEXTS[] = { "B", "I", "S", "`", "H1", "H2", "H3", "•", "1.", "[ ]",
        ">", "link", "img", "table", "---", "code" };
    const int32_t want = (int32_t)(sizeof(LABELS) / sizeof(LABELS[0]));

    // One button per format key, in key order (the key minus DAWN_KEY_FMT_BOLD is the FormatOp).
    const Command* items[RIBBON_MAX];
    int32_t n = commands_ribbon(items, RIBBON_MAX);
    CHECK(n == want);
    CHECK(DAWN_KEY_FMT_CODE_BLOCK - DAWN_KEY_FMT_BOLD + 1 == want);
    for (int32_t i = 0; i < n && i < want; i++) {
        CHECK(items[i]->key == DAWN_KEY_FMT_BOLD + i);
        CHECK(items[i]->group == CMD_GROUP_FORMAT);
        CHECK(items[i]->flags == CMD_NEEDS_EDIT);
        CHECK(strcmp(items[i]->label, LABELS[i]) == 0);
        CHECK(strcmp(commands_button_label(items[i]), TEXTS[i]) == 0);
        CHECK(commands_find(DAWN_KEY_FMT_BOLD + i) == items[i]);
    }
    CHECK(commands_ribbon(items, 3) == 3);
    CHECK(strcmp(commands_group_name(CMD_GROUP_FORMAT), "format") == 0);

    // "B  I  S  `  H1  H2  H3  •  1.  [ ]  >  link  img  table  ---  code" is 66 columns.
    RibbonSlot s[RIBBON_MAX];
    CHECK(commands_ribbon_layout(items, n, 66, s) == 1);
    CHECK(s[0].col == 0 && s[n - 1].col == 62 && s[n - 1].width == 4);
    CHECK(s[7].width == 1); // the bullet is one cell, three bytes
    CHECK(commands_ribbon_rows(80, 1) == 1);

    // The phone pane's note is 45 columns: the inserts go down whole, the rest stays up.
    CHECK(commands_ribbon_layout(items, n, 45, s) == 2);
    for (int32_t i = 0; i < n; i++)
        CHECK(s[i].row == (i >= 11 ? 1 : 0));
    CHECK(s[10].col + s[10].width == 37);
    CHECK(s[11].col == 0);
    CHECK(commands_ribbon_rows(65, 1) == 2);
    CHECK(commands_ribbon_place(items, n, 45, 1, s) == 2); // scale 1 is the layout above
    CHECK(s[11].row == 1 && s[11].col == 0 && s[10].col == 36);

    // Narrower than a cluster: it breaks between buttons. Every button stays inside the row and
    // RIBBON_GAP apart from the one before it on its row, so tap regions never overlap.
    for (int32_t room = 5; room <= 70; room++) {
        int32_t rows = commands_ribbon_layout(items, n, room, s);
        CHECK(rows >= 1);
        for (int32_t i = 0; i < n; i++) {
            CHECK(s[i].col >= 0 && s[i].col + s[i].width <= room);
            CHECK(s[i].row >= 0 && s[i].row < rows);
            if (i > 0 && s[i].row == s[i - 1].row)
                CHECK(s[i].col == s[i - 1].col + s[i - 1].width + RIBBON_GAP);
            if (i > 0)
                CHECK(s[i].row >= s[i - 1].row);
        }
    }
    CHECK(commands_ribbon_layout(items, n, 4, s) == 0); // "table" does not fit
    CHECK(commands_ribbon_layout(items, 0, 40, s) == 0);

    // Scale 2 (kitty text sizing): the phone pane's 45-cell note is 22 scaled cells. Three rows
    // of buttons, six screen rows: the styles and headings, the lists with "link", then the rest.
    // Slots come back in screen rows and cells.
    CHECK(commands_ribbon_place(items, n, 45, 2, s) == 6);
    CHECK(commands_ribbon_rows(45, 2) == 6);
    for (int32_t i = 0; i < n; i++)
        CHECK(s[i].row == (i <= 6 ? 0 : i <= 11 ? 2 : 4));
    CHECK(s[1].col == 6 && s[1].width == 2); // two scaled cells after "B"
    CHECK(s[6].col == 40 && s[6].col + s[6].width == 44); // "H3"
    CHECK(s[7].col == 0 && s[11].col == 30 && s[11].width == 8); // "•" ... "link"
    CHECK(s[12].col == 0 && s[15].col == 34 && s[15].col + s[15].width == 42); // "img" ... "code"
    // The whole 53-column pane (26 scaled cells) also takes three rows.
    CHECK(commands_ribbon_rows(53, 2) == 6);

    // At 36 cells (18 scaled) the two-cell gap would take five rows; one scaled cell takes three.
    CHECK(commands_ribbon_layout(items, n, 18, s) == 5);
    CHECK(commands_ribbon_place(items, n, 36, 2, s) == 6);
    CHECK(s[1].col == 4 && s[4].row == 0 && s[7].row == 2 && s[13].row == 4);

    // Scale 4, a pane pinched to 120 columns (a 116-cell note, 29 scaled cells): with the narrow
    // gap the styles, headings and lists share a row and the inserts take the next, two rows of
    // buttons (eight screen rows) where the two-cell gap would take three.
    CHECK(commands_ribbon_place(items, n, 116, 4, s) == 8);
    CHECK(s[1].col == 8 && s[1].width == 4);
    CHECK(s[10].row == 0 && s[11].row == 4 && s[11].col == 0 && s[11].width == 16);
    // Scale 3 in 80 columns less margins (76 cells, 25 scaled).
    CHECK(commands_ribbon_place(items, n, 76, 3, s) == commands_ribbon_rows(76, 3));
    CHECK(commands_ribbon_rows(76, 3) % 3 == 0);

    // At every scale each button sits on a multiple of the scale inside the note, scale screen
    // rows to a row of buttons, with tap regions (the text and a cell either side) that never
    // overlap, and the ribbon never takes more rows than the two-cell layout of the same scaled
    // width.
    for (int32_t scale = 2; scale <= UI_SCALE_MAX; scale++) {
        for (int32_t room = 5 * scale; room <= 160; room++) {
            int32_t rows = commands_ribbon_place(items, n, room, scale, s);
            CHECK(rows >= scale && rows % scale == 0);
            CHECK(rows / scale <= commands_ribbon_rows(room / scale, 1));
            for (int32_t i = 0; i < n; i++) {
                CHECK(s[i].col % scale == 0 && s[i].width % scale == 0 && s[i].row % scale == 0);
                CHECK(s[i].col >= 0 && s[i].col + s[i].width <= room);
                CHECK(s[i].row >= 0 && s[i].row < rows);
                if (i > 0 && s[i].row == s[i - 1].row)
                    CHECK((s[i].col - 1) - (s[i - 1].col + s[i - 1].width) >= 1);
                if (i > 0)
                    CHECK(s[i].row >= s[i - 1].row);
            }
        }
        CHECK(commands_ribbon_place(items, n, 5 * scale - 1, scale, s) == 0); // "table" needs five scaled cells
    }
    CHECK(commands_ribbon_place(items, n, 9, 2, s) == 0); // "table" needs ten cells
    CHECK(commands_ribbon_place(items, 0, 40, 2, s) == 0);
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
    menu_build(&m, -1, all_on);
    CHECK(m.bar_menu == -1);
    int32_t n = 0;
    commands_all(&n);
    // Every command but the menu's own and the ribbon's, plus a heading per group but format.
    const Command* ribbon[RIBBON_MAX];
    int32_t nr = commands_ribbon(ribbon, RIBBON_MAX);
    CHECK(m.count == n - 1 - nr + CMD_GROUP_COUNT - 1);
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
    menu_build(&m, -1, no_edit);
    CHECK(m.selected > 0 && m.lines[m.selected].group == CMD_GROUP_VIEW);
    CHECK(menu_step(&m, m.selected, -1, 1) == m.selected);
    menu_build(&m, -1, none_on);
    CHECK(m.selected == -1);

    // Scrolling keeps the selection in view, with its heading when it opens a group.
    menu_build(&m, -1, all_on);
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

static void test_dropdowns(void)
{
    int32_t n = 0;
    const Command* all = commands_all(&n);
    MenuBarItem items[MENUBAR_COUNT];
    int32_t ni = menubar_items(items, MENUBAR_COUNT);
    int32_t listed = 0;
    MenuState m;
    for (int32_t k = 0; k < ni; k++) {
        menu_build(&m, (int32_t)items[k], all_on);
        CHECK(m.bar_menu == (int32_t)items[k]);
        CHECK(m.count > 0 && m.selected == 0 && m.scroll == 0);
        // Its groups' commands in table order, no headings, nothing of the ribbon's.
        int32_t at = 0;
        for (int32_t c = 0; c < n; c++) {
            if (menubar_of_group(all[c].group) != (int32_t)items[k])
                continue;
            CHECK(at < m.count && m.lines[at].cmd == &all[c] && m.lines[at].enabled);
            at++;
        }
        CHECK(at == m.count);
        for (int32_t i = 0; i < m.count; i++)
            CHECK(m.lines[i].cmd && m.lines[i].group != CMD_GROUP_FORMAT);
        listed += m.count;
    }
    // Every command but the ribbon's is in exactly one dropdown.
    const Command* ribbon[RIBBON_MAX];
    CHECK(listed == n - commands_ribbon(ribbon, RIBBON_MAX));

    // File: the note group. Edit: the edit group, then find, with no heading between.
    menu_build(&m, MENUBAR_FILE, all_on);
    CHECK(m.count == 5 && strcmp(m.lines[0].cmd->label, "title and details") == 0);
    CHECK(strcmp(m.lines[4].cmd->label, "close note") == 0);
    CHECK(menu_list_cols(&m) == 21); // "title and details  ^G"
    menu_build(&m, MENUBAR_EDIT, all_on);
    CHECK(m.count == 15);
    CHECK(strcmp(m.lines[0].cmd->label, "undo") == 0 && strcmp(m.lines[12].cmd->label, "find") == 0);
    CHECK(strcmp(m.lines[13].cmd->label, "table of contents") == 0);
    CHECK(menu_list_cols(&m) == 24); // "delete to line start  ^U"
    // View: the ribbon's switch first, which has no shortcut.
    menu_build(&m, MENUBAR_VIEW, all_on);
    CHECK(m.count == 4 && m.lines[0].cmd->key == DAWN_KEY_CMD_RIBBON);
    CHECK(strcmp(m.lines[0].cmd->label, "formatting") == 0);
    CHECK(menu_list_cols(&m) == 24); // "hide marks at cursor  ^B"
    menu_build(&m, MENUBAR_AI, all_on);
    CHECK(m.count == 2 && m.lines[0].cmd->key == 31);
    // Help keeps "all commands", which opens the full menu.
    menu_build(&m, MENUBAR_HELP, all_on);
    CHECK(m.count == 4 && m.lines[3].cmd->key == DAWN_KEY_CMD_MENU);
    CHECK(menu_list_cols(&m) == 19); // "markdown cheatsheet" has no shortcut

    // A dropdown whose first commands cannot run highlights the first that can.
    menu_build(&m, MENUBAR_EDIT, no_edit);
    CHECK(m.selected == 12); // find
    CHECK(menu_step(&m, m.selected, -1, 1) == 12);
    menu_build(&m, MENUBAR_FILE, none_on);
    CHECK(m.selected == -1);
}

static void test_help_rows(void)
{
    HelpRow rows[HELP_ROWS_MAX];
    int32_t n = help_rows(rows, HELP_ROWS_MAX);
    CHECK(n > 40 && n < HELP_ROWS_MAX);
    CHECK(rows[0].kind == HELP_ROW_HEADING && strcmp(rows[0].text, "move and select") == 0);
    CHECK(rows[n - 1].kind == HELP_ROW_ITEM);

    // Every command is listed once, under its group's heading, with how to reach it by touch.
    int32_t count = 0;
    const Command* all = commands_all(&count);
    for (int32_t c = 0; c < count; c++) {
        int32_t found = 0;
        for (int32_t i = 0; i < n; i++) {
            if (rows[i].kind != HELP_ROW_ITEM || rows[i].text != all[c].label)
                continue;
            found++;
            CHECK(strcmp(rows[i].key, all[c].shortcut) == 0);
            if (all[c].group == CMD_GROUP_FORMAT) {
                CHECK(strcmp(rows[i].tap, "ribbon") == 0);
                CHECK(rows[i].tap_label && strcmp(rows[i].tap_label, commands_button_label(&all[c])) == 0);
            } else {
                CHECK(strcmp(rows[i].tap, menubar_tap(all[c].group)) == 0);
                CHECK(rows[i].tap_label == NULL);
            }
            int32_t h = i;
            while (h >= 0 && rows[h].kind != HELP_ROW_HEADING)
                h--;
            CHECK(h >= 0 && strcmp(rows[h].text, commands_group_name(all[c].group)) == 0);
        }
        CHECK(found == 1);
    }
    for (int32_t i = 0; i < n; i++) {
        if (rows[i].kind == HELP_ROW_ITEM && strcmp(rows[i].text, "undo") == 0)
            CHECK(strcmp(rows[i].tap, "Edit menu") == 0);
        if (rows[i].kind == HELP_ROW_ITEM && strcmp(rows[i].text, "formatting") == 0)
            CHECK(strcmp(rows[i].tap, "View menu") == 0);
        if (rows[i].kind == HELP_ROW_ITEM && strcmp(rows[i].text, "bold") == 0)
            CHECK(strcmp(rows[i].tap, "ribbon") == 0 && strcmp(rows[i].tap_label, "B") == 0);
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
    test_menubar();
    test_ribbon();
    test_menu();
    test_dropdowns();
    test_help_rows();
    test_help_paginate();
    test_help_note();
    printf("%d/%d checks passed\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}
