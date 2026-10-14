// test_commands.c - host tests for the command table (dawn_commands.c): every entry complete,
// keys unique, groups in display order, the action bar's items and fit, and the menu's lines,
// stepping and scrolling. Built with USE_LIBAI and DAWN_HAS_SPEAK so the fullest table is checked.

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
    return key == DAWN_KEY_CMD_MENU || key == DAWN_KEY_CMD_RIBBON || key == DAWN_KEY_CMD_CHEATSHEET;
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
    // Every command but the menu's own, plus a heading per group.
    CHECK(m.count == n - 1 + CMD_GROUP_COUNT);
    CHECK(m.lines[0].cmd == NULL && m.lines[0].group == CMD_GROUP_NOTE);
    CHECK(m.selected == 1);
    for (int32_t i = 0; i < m.count; i++)
        CHECK(!m.lines[i].cmd || m.lines[i].cmd->key != DAWN_KEY_CMD_MENU);

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

int main(void)
{
    test_entries();
    test_bar();
    test_menu();
    printf("%d/%d checks passed\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}
