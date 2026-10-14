// test_commands.c - host tests for the command table (dawn_commands.c): every entry complete,
// keys unique, groups in display order, the action bar's items and fit, and the menu's lines,
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
    CHECK(commands_ribbon_rows(80) == 1);

    // The phone pane's note is 45 columns: the inserts go down whole, the rest stays up.
    CHECK(commands_ribbon_layout(items, n, 45, s) == 2);
    for (int32_t i = 0; i < n; i++)
        CHECK(s[i].row == (i >= 11 ? 1 : 0));
    CHECK(s[10].col + s[10].width == 37);
    CHECK(s[11].col == 0);
    CHECK(commands_ribbon_rows(65) == 2);

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
