// test_commands.c - host tests for the command table (dawn_commands.c): every entry complete,
// keys unique, groups in display order, the action bar's items and fit and the ribbon's layout
// (at scale 1, and at scale 2 where kitty text sizing draws them twice the size), the menu's lines,
// stepping and scrolling, and the help pages' lines and page breaks. Built with USE_LIBAI and
// DAWN_HAS_SPEAK so the fullest table is checked.

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
        // Labels are lower case, as dawn writes its hints (the bar's "Aa" and "AI" are not labels).
        for (const char* p = c->label; *p; p++)
            CHECK(!(*p >= 'A' && *p <= 'Z'));
        CHECK(commands_find(c->key) == c);
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

static void test_bar(void)
{
    const Command* bar[16];
    int32_t n = commands_bar(bar, 16);
    CHECK(n == 4);
    if (n == 4) {
        CHECK(strcmp(commands_bar_label(bar[0]), "undo") == 0);
        CHECK(strcmp(commands_bar_label(bar[1]), "find") == 0);
        CHECK(strcmp(commands_bar_label(bar[2]), "Aa") == 0);
        CHECK(strcmp(commands_bar_label(bar[3]), "AI") == 0);
        CHECK(bar[2]->key == DAWN_KEY_CMD_RIBBON);
        CHECK(bar[3]->key == 31);
    }
    for (int32_t i = 0; i < n; i++)
        CHECK(bar[i]->key != DAWN_KEY_CMD_MENU);
    CHECK(strcmp(commands_bar_label(commands_find(DAWN_KEY_CMD_MENU)), "more") == 0);
    CHECK(commands_bar(bar, 2) == 2);

    // "undo   find   Aa   AI   more" is 28 columns.
    int32_t w[4] = { 4, 4, 2, 2 };
    CHECK(commands_bar_fit(w, 4, 4, 3, 28) == 4);
    CHECK(commands_bar_fit(w, 4, 4, 3, 27) == 3); // AI goes first
    CHECK(commands_bar_fit(w, 4, 4, 3, 11) == 1);
    CHECK(commands_bar_fit(w, 4, 4, 3, 10) == 0); // "more" alone
    CHECK(commands_bar_fit(w, 4, 4, 3, 2) == 0);

    // Scale 1 is the bar as above: the three-cell gap, labels at their own width.
    BarSlot b[5];
    CHECK(commands_bar_gap(1) == 3);
    CHECK(commands_bar_layout(w, 4, 4, 1, 28, b) == 4);
    CHECK(b[0].col == 0 && b[1].col == 7 && b[2].col == 14 && b[3].col == 19 && b[4].col == 24);
    CHECK(b[4].width == 4);

    // Scale 2 (kitty text sizing): every label twice as wide, two cells between them. The phone
    // pane (53 columns) has a 45-cell note, and "undo find Aa AI more" takes 32 + 4 x 2 = 40.
    CHECK(commands_bar_gap(2) == 2);
    CHECK(commands_bar_layout(w, 4, 4, 2, 45, b) == 4);
    CHECK(b[0].col == 0 && b[0].width == 8);
    CHECK(b[1].col == 10 && b[1].width == 8);
    CHECK(b[2].col == 20 && b[2].width == 4);
    CHECK(b[3].col == 26 && b[3].width == 4);
    CHECK(b[4].col == 32 && b[4].width == 8);
    CHECK(commands_bar_layout(w, 4, 4, 2, 40, b) == 4);
    CHECK(commands_bar_layout(w, 4, 4, 2, 39, b) == 3); // AI goes first
    CHECK(b[3].col == 26 && b[3].width == 8); // "more" right after "Aa"
    CHECK(commands_bar_layout(w, 4, 4, 2, 8, b) == 0); // "more" alone
    CHECK(b[0].col == 0 && b[0].width == 8);
    CHECK(commands_bar_layout(w, 4, 4, 2, 7, b) == -1); // not even "more"

    // At either scale every item stays inside the row, and a tap region (the text and a cell
    // either side) never overlaps the next one's.
    for (int32_t scale = 1; scale <= 2; scale++) {
        for (int32_t room = 4 * scale; room <= 70; room++) {
            int32_t kept = commands_bar_layout(w, 4, 4, scale, room, b);
            CHECK(kept >= 0 && kept <= 4);
            if (kept < 0)
                continue;
            CHECK(b[kept].col + b[kept].width <= room);
            CHECK(b[kept].width == 4 * scale);
            for (int32_t i = 0; i < kept; i++) {
                CHECK(b[i].width == w[i] * scale);
                CHECK((b[i + 1].col - 1) - (b[i].col + b[i].width) >= 1);
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
        CHECK(items[i]->bar == 0);
        CHECK(strcmp(items[i]->label, LABELS[i]) == 0);
        CHECK(strcmp(commands_bar_label(items[i]), TEXTS[i]) == 0);
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

    // Every scaled button sits on even cells inside the note, two screen rows to a row of
    // buttons, with tap regions (the text and a cell either side) that never overlap, and the
    // ribbon never takes more rows than the two-cell layout of the same scaled width.
    for (int32_t room = 10; room <= 140; room++) {
        int32_t rows = commands_ribbon_place(items, n, room, 2, s);
        CHECK(rows >= 2 && rows % 2 == 0);
        CHECK(rows / 2 <= commands_ribbon_rows(room / 2, 1));
        for (int32_t i = 0; i < n; i++) {
            CHECK(s[i].col % 2 == 0 && s[i].width % 2 == 0 && s[i].row % 2 == 0);
            CHECK(s[i].col >= 0 && s[i].col + s[i].width <= room);
            CHECK(s[i].row >= 0 && s[i].row < rows);
            if (i > 0 && s[i].row == s[i - 1].row)
                CHECK((s[i].col - 1) - (s[i - 1].col + s[i - 1].width) >= 1);
            if (i > 0)
                CHECK(s[i].row >= s[i - 1].row);
        }
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
    menu_build(&m, all_on);
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
                CHECK(rows[i].tap_label && strcmp(rows[i].tap_label, commands_bar_label(&all[c])) == 0);
            } else if (all[c].bar > 0) {
                CHECK(strcmp(rows[i].tap, "bar") == 0);
                CHECK(rows[i].tap_label && strcmp(rows[i].tap_label, commands_bar_label(&all[c])) == 0);
            } else {
                CHECK(strcmp(rows[i].tap, "menu") == 0);
                CHECK(rows[i].tap_label == NULL);
            }
            int32_t h = i;
            while (h >= 0 && rows[h].kind != HELP_ROW_HEADING)
                h--;
            CHECK(h >= 0 && strcmp(rows[h].text, commands_group_name(all[c].group)) == 0);
        }
        CHECK(found == 1);
    }
    const Command* aa = commands_find(DAWN_KEY_CMD_RIBBON);
    CHECK(aa && aa->bar > 0 && strcmp(commands_bar_label(aa), "Aa") == 0);
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
    test_bar();
    test_ribbon();
    test_menu();
    test_help_rows();
    test_help_paginate();
    test_help_note();
    printf("%d/%d checks passed\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}
