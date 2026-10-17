// test_commands.c - host tests for the command table (dawn_commands.c): every entry complete,
// keys unique, groups in display order, every command on a ribbon or in the menu, the UI scale from the pane's
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
        // A ribbon's buttons are all as wide as its widest caption: seven columns at most on a
        // ribbon, so six fit a row on the phone; a command on none has a little more.
        CHECK(c->caption && c->caption[0] && cols(c->caption) <= (ribbon_tab_of(c) >= 0 ? 7 : 9));
        for (const char* p = c->caption; *p; p++)
            CHECK(!(*p >= 'A' && *p <= 'Z'));
        // Every button has a face of its own, short, and never bare Markdown syntax.
        CHECK(c->glyph && c->glyph[0] && cols(c->glyph) <= 2);
        CHECK(!strchr(c->glyph, '`') && !strchr(c->glyph, '[') && !strchr(c->glyph, '#'));
        CHECK(strcmp(c->glyph, ">") != 0 && strcmp(c->glyph, "-") != 0 && strcmp(c->glyph, "*") != 0);
        CHECK(!strstr(c->glyph, "--") && !strstr(c->glyph, "**"));
        CHECK(c->icon == 0 || (c->icon >= 0xF0001 && c->icon <= 0xF1AF0)); // nf-md-*
        CHECK(commands_find(c->key) == c);
        CHECK(tabs_from_key(c->key) < 0); // the tabs are not commands
        CHECK(c->key != DAWN_KEY_RIBBON_MORE);
        // Every command is on some ribbon or in the full menu: nothing is reachable from the
        // keyboard alone.
        CHECK(ribbon_tab_of(c) >= 0 || commands_in_menu(c));
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
    // A 1080-pixel-wide phone: the chrome's row is about 1080 / 26 = 41 pixels whatever the font.
    CHECK(commands_ui_scale(53, 20, 44) == 1); // the normal font: 41 / 44 rounds to 1
    CHECK(commands_ui_scale(120, 9, 19) == 2); // pinched small: 41 / 19 rounds to 2
    CHECK(commands_ui_scale(160, 7, 13) == 3); // pinched smaller: 43 / 13 rounds to 3
    CHECK(commands_ui_scale(30, 36, 80) == 1); // a large font: 41 / 80 is held at 1
    CHECK(commands_ui_scale(300, 5, 8) == UI_SCALE_MAX); // 7, held at the most
    CHECK(commands_ui_scale(10, 10, 200) == 1); // never below 1
    // Rounding: exactly half way rounds up, just under rounds down.
    CHECK(commands_ui_scale(26, 3, 2) == 2); // 1.5
    CHECK(commands_ui_scale(26, 29, 20) == 1); // 1.45
    // No pixel size: 1.
    CHECK(commands_ui_scale(53, 0, 0) == 1);
    CHECK(commands_ui_scale(53, 20, 0) == 1);
    CHECK(commands_ui_scale(0, 20, 44) == 1);
    // A 1080-pixel pane at any font: below the cap the chrome's row on screen (scale x cell
    // height) is within half a cell of 1/UI_SCALE_DIVISOR of the width.
    for (int32_t cell_h = 8; cell_h <= 80; cell_h++) {
        int32_t cell_w = cell_h / 2;
        int32_t cols = 1080 / cell_w;
        int32_t scale = commands_ui_scale(cols, cell_w, cell_h);
        CHECK(scale >= 1 && scale <= UI_SCALE_MAX);
        int32_t px = scale * cell_h;
        int32_t want = cols * cell_w / UI_SCALE_DIVISOR;
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

//! Whether the packed rows of a laid-out ribbon (s, n slots, b their buttons) reach right: every
//! row ends there, all of its buttons one width, unless it is a shorter row of a run of rows that
//! hold one section only, which keeps the columns and the width of the fullest row of that run.
static void check_spread(const RibbonButton* b, const RibbonSlot* s, int32_t n, int32_t right)
{
    int32_t first[RIBBON_MAX], count[RIBBON_MAX]; // each row's first slot and its buttons
    bool one[RIBBON_MAX];
    int32_t rows = 0;
    for (int32_t i = 0; i < n; i++) {
        if (i == 0 || s[i].page != s[i - 1].page || s[i].row != s[i - 1].row) {
            first[rows] = i;
            count[rows] = 0;
            one[rows] = true;
            rows++;
        }
        int32_t r = rows - 1;
        if (count[r] > 0) {
            CHECK(s[i].width == s[i - 1].width); // one width a row
            one[r] = one[r] && b[i].section == b[i - 1].section;
        }
        count[r]++;
    }
    for (int32_t r = 0; r < rows; r++) {
        int32_t last = first[r] + count[r] - 1;
        if (s[last].col + s[last].width == right)
            continue;
        // Short of the edge: a single-section row with a fuller one in its run, set as that one.
        CHECK(one[r]);
        int32_t sec = b[first[r]].section;
        int32_t lo = r, hi = r;
        while (lo > 0 && one[lo - 1] && b[first[lo - 1]].section == sec)
            lo--;
        while (hi < rows - 1 && one[hi + 1] && b[first[hi + 1]].section == sec)
            hi++;
        int32_t full = r;
        for (int32_t q = lo; q <= hi; q++)
            full = count[q] > count[full] ? q : full;
        CHECK(count[full] > count[r]);
        int32_t fl = first[full] + count[full] - 1;
        CHECK(s[fl].col + s[fl].width == right);
        for (int32_t j = 0; j < count[r]; j++) {
            CHECK(s[first[r] + j].col == s[first[full] + j].col);
            CHECK(s[first[r] + j].width == s[first[full] + j].width);
        }
    }
}

//! Whether every button of ribbon tab is placed inside room at scale, in order, apart, inside the
//! frame, at least as wide as the ribbon's one width, a section either going on after the one
//! before it behind a divider or starting a row of its own under a rule, every row spread to the
//! right edge (check_spread), and in no more rows than every section on rows of its own would
//! take; or, when the layout said no, that it truly could not.
static void check_ribbon(RibbonTab tab, int32_t room, int32_t scale, bool icons, bool sized, int32_t max_height)
{
    RibbonButton b[RIBBON_MAX];
    RibbonSlot s[RIBBON_MAX];
    RibbonFrame f;
    int32_t n = ribbon_buttons(tab, scale, icons, sized, b, RIBBON_MAX);
    CHECK(n > 0);
    int32_t h = ribbon_button_rows(scale, sized);
    int32_t gap = ribbon_row_gap(scale); // between two rows on a page
    int32_t rule = RIBBON_RULE > gap ? RIBBON_RULE : gap; // the same, a rule drawn in it
    bool ok = ribbon_layout(b, n, room, scale, sized, max_height, s, &f);
    int32_t widest = 0;
    for (int32_t i = 0; i < n; i++) {
        CHECK(b[i].width == b[0].width); // one width: the buttons form a grid
        CHECK(b[i].width >= ribbon_button_cols(b[i].cmd, scale, icons, sized));
        widest = b[i].width > widest ? b[i].width : widest;
    }
    if (!ok) {
        CHECK(f.height == 0 && f.pages == 0);
        CHECK(max_height < h || widest > room - ribbon_more_cols(scale, sized) - RIBBON_GAP);
        return;
    }
    CHECK(f.pages >= 1 && f.rows >= 1);
    CHECK(f.height >= f.rows * h + (f.rows - 1) * gap && f.height <= max_height);
    int32_t right = room;
    if (f.pages > 1) {
        CHECK(f.more_width == ribbon_more_cols(scale, sized) && f.more_col == room - f.more_width);
        right = f.more_col - RIBBON_GAP;
    } else {
        CHECK(f.more_width == 0);
    }
    int32_t section_start = 0; // the first button of the section i is in
    for (int32_t i = 0; i < n; i++) {
        CHECK(s[i].width >= b[i].width);
        CHECK(s[i].col >= 0 && s[i].col + s[i].width <= right);
        CHECK(s[i].page >= 0 && s[i].page < f.pages);
        CHECK(s[i].row >= 0 && s[i].row + h <= f.height);
        CHECK(!s[i].rule_above || (s[i].col == 0 && s[i].row > 0));
        if (i == 0) {
            CHECK(s[i].row == 0 && s[i].col == 0 && !s[i].rule_above);
            continue;
        }
        bool opens = b[i].section != b[i - 1].section;
        CHECK(opens ? b[i].section == b[i - 1].section + 1 : b[i].section == b[i - 1].section);
        bool same_page = s[i].page == s[i - 1].page;
        bool same = same_page && s[i].row == s[i - 1].row;
        if (same) {
            CHECK(s[i].col >= s[i - 1].col + s[i - 1].width + (opens ? RIBBON_DIVIDER : RIBBON_GAP));
            CHECK(!s[i].rule_above);
        } else {
            CHECK(s[i].col == 0);
            CHECK(s[i].page > s[i - 1].page || (same_page && s[i].row > s[i - 1].row));
            if (!same_page) {
                CHECK(s[i].page == s[i - 1].page + 1 && s[i].row == 0 && !s[i].rule_above);
            } else if (opens) {
                // A section on a row of its own, under a rule (in the gap between the rows).
                CHECK(s[i].rule_above && s[i].row == s[i - 1].row + h + rule);
            } else {
                // A section wrapping: its next row under the gap, from the left edge, with no rule
                // (a rule only where a section starts a row).
                CHECK(!s[i].rule_above && s[i].row == s[i - 1].row + h + gap);
            }
        }
        if (opens)
            section_start = i;
    }
    (void)section_start;
    check_spread(b, s, n, right);
    // Going on after a section never costs a row: no more rows than each section on rows of its
    // own, spread evenly, would take.
    if (f.pages == 1) {
        int32_t fit = (room + RIBBON_GAP) / (widest + RIBBON_GAP);
        int32_t blocks = 0, count = 0;
        for (int32_t i = 0; i < n; i++) {
            count++;
            if (i == n - 1 || b[i + 1].section != b[i].section) {
                blocks += (count + fit - 1) / fit;
                count = 0;
            }
        }
        CHECK(f.rows <= blocks);
    }
    // A page that pages on was full: its next row would not have fit under it.
    for (int32_t i = 1; i < n; i++) {
        if (s[i].page != s[i - 1].page) {
            int32_t need = s[i - 1].row + h + h + (b[i].section != b[i - 1].section ? rule : gap);
            CHECK(need > max_height);
        }
    }
}

//! How far the text of a glyph block placed at p sits from the centre of a box w cells wide, in
//! units of 1 / (2 den) cells (0: exactly centred): the block is block cells, its text drawn
//! block * num / den wide at its left edge (h=0), right edge (h=1) or centred (h=2)
static int64_t glyph_err(int32_t w, int32_t block, int32_t num, int32_t den, RibbonGlyphPlace p)
{
    int64_t off = p.halign == 2 ? (int64_t)block * den
        : p.halign == 0         ? (int64_t)block * num
                                : 2 * (int64_t)block * den - (int64_t)block * num;
    int64_t err = 2 * (int64_t)den * p.col + off - (int64_t)w * den;
    return err < 0 ? -err : err;
}

static void test_ribbons(void)
{
    // Captions sit under glyphs at half their scale, so a button is 2 rows at scale 1, 3 at 2
    // (about 43dp on the phone), 4 at 3 and 6 at 4.
    CHECK(ribbon_caption_scale(1) == 1 && ribbon_caption_scale(2) == 1 && ribbon_caption_scale(3) == 1);
    CHECK(ribbon_caption_scale(4) == 2);
    CHECK(ribbon_button_rows(1, false) == 2 && ribbon_button_rows(2, false) == 3 && ribbon_button_rows(3, false) == 4);
    CHECK(ribbon_button_rows(4, false) == 6);
    // A blank row between two rows of buttons where the chrome is scaled; none at scale 1.
    CHECK(ribbon_row_gap(1) == 0 && ribbon_row_gap(2) == 1 && ribbon_row_gap(4) == 1);
    // Where the terminal sizes text a button's box is 2 rows even at scale 1 (its glyph sized and
    // centred in them), so a button is 3 rows there; from scale 2 on the box is scale rows as without.
    CHECK(ribbon_glyph_rows(1, false) == 1 && ribbon_glyph_rows(1, true) == 2);
    CHECK(ribbon_button_rows(1, true) == 3);
    for (int32_t sc = 2; sc <= UI_SCALE_MAX; sc++)
        CHECK(ribbon_glyph_rows(sc, true) == sc && ribbon_button_rows(sc, true) == ribbon_button_rows(sc, false));
    // A glyph is half a row less than its box: 3/4 of 2 rows, 5/6 of 3, 7/8 of 4; plain in one row.
    int32_t gn = -1, gd = -1;
    ribbon_glyph_frac(1, &gn, &gd);
    CHECK(gn == 0 && gd == 0);
    ribbon_glyph_frac(2, &gn, &gd);
    CHECK(gn == 3 && gd == 4);
    ribbon_glyph_frac(3, &gn, &gd);
    CHECK(gn == 5 && gd == 6);
    ribbon_glyph_frac(4, &gn, &gd);
    CHECK(gn == 7 && gd == 8);
    // An icon's block (2 cells at the box's scale g, the icon drawn 2g - 1 cells wide) is centred
    // exactly in a box of either parity: in 6 cells the block from cell 1, the icon centred in it;
    // in 7 the block from cell 2, the icon at its left edge (cells 2 to 5, its centre 3.5).
    RibbonGlyphPlace gp = ribbon_glyph_place(6, 4, 3, 4);
    CHECK(gp.col == 1 && gp.halign == 2);
    gp = ribbon_glyph_place(7, 4, 3, 4);
    CHECK(gp.col == 2 && gp.halign == 0);
    // A one-column text glyph ("B") at 3/4 of 2 rows in 7 cells: a quarter cell right of centre.
    gp = ribbon_glyph_place(7, 2, 3, 4);
    CHECK(gp.col == 3 && gp.halign == 0 && glyph_err(7, 2, 3, 4, gp) == 2);
    // Every box scale and width: inside the box; an icon or a two-column glyph exactly centred, a
    // one-column glyph within a quarter cell; full size (0, 0) centred as the cells allow.
    for (int32_t g = 2; g <= UI_SCALE_MAX; g++) {
        ribbon_glyph_frac(g, &gn, &gd);
        for (int32_t w = 2 * g + 2; w <= 40; w++) {
            for (int32_t k = 1; k <= 2; k++) {
                gp = ribbon_glyph_place(w, k * g, gn, gd);
                CHECK(gp.col >= 0 && gp.col + k * g <= w && gp.halign >= 0 && gp.halign <= 2);
                CHECK(glyph_err(w, k * g, gn, gd, gp) <= (k == 2 ? 0 : gd / 2));
            }
            gp = ribbon_glyph_place(w, 2 * g, 0, 0);
            CHECK(gp.halign == 2 && gp.col == (w - 2 * g) / 2);
        }
    }
    // A box narrower than its block: the block at its left edge.
    gp = ribbon_glyph_place(3, 4, 3, 4);
    CHECK(gp.col == 0 && gp.halign == 2);

    // The Formatting ribbon: the format group, one button per format key in FormatOp order, then help.
    RibbonButton b[RIBBON_MAX];
    int32_t n = ribbon_buttons(RIBBON_TAB_FORMAT, 2, true, true, b, RIBBON_MAX);
    int32_t styles = DAWN_KEY_FMT_CODE_BLOCK - DAWN_KEY_FMT_BOLD + 1;
    CHECK(n == styles + 4);
    for (int32_t i = 0; i < n; i++) {
        CHECK(b[i].section == (i < styles ? 0 : 1));
        if (i < styles)
            CHECK(b[i].cmd->key == DAWN_KEY_FMT_BOLD + i && b[i].cmd->group == CMD_GROUP_FORMAT);
        else
            CHECK(b[i].cmd->group == CMD_GROUP_HELP);
        // Every button as wide as the widest: "bullets" and "numbers" under their icons.
        CHECK(b[i].width == 7);
    }
    CHECK(b[styles].cmd->key == 15 && b[n - 1].cmd->key == DAWN_KEY_CMD_MENU);
    // What each button needs: an icon at scale 2 is 4 cells and a cell either side; a longer
    // caption widens it.
    CHECK(ribbon_button_cols(b[0].cmd, 2, true, true) == 6); // bold: icon 6, caption 4
    CHECK(ribbon_button_cols(b[8].cmd, 2, true, true) == 7); // numbered list: its caption
    // A glyph is given two columns with the Nerd Font (the icon and a space, one OSC 66 block) and
    // without it, so the buttons are as wide either way.
    CHECK(ribbon_glyph_cols(b[0].cmd, true) == 2 && ribbon_glyph_cols(b[0].cmd, false) == 2);
    for (int32_t i = 0; i < n; i++)
        CHECK(ribbon_button_cols(b[i].cmd, 3, true, true) == ribbon_button_cols(b[i].cmd, 3, false, true));
    CHECK(ribbon_button_cols(b[0].cmd, 1, false, false) == 4); // "bold" under "B" and its two cells
    CHECK(ribbon_button_cols(b[3].cmd, 2, false, true) == 6); // "<>" at 2, and a cell either side
    CHECK(ribbon_button_cols(b[0].cmd, 4, true, true) == 10 && ribbon_button_cols(b[8].cmd, 4, true, true) == 14);

    // File: the dozen most used. Details, pause, close; undo, redo, cut, copy, paste; find,
    // contents, focus, light or dark.
    n = ribbon_buttons(RIBBON_TAB_FILE, 1, false, false, b, RIBBON_MAX);
    CHECK(n == 3 + 5 + 4);
    CHECK(b[0].cmd->key == 7 && b[0].section == 0 && b[1].cmd->key == 16 && b[2].cmd->key == '\x1b');
    CHECK(b[3].cmd->key == 26 && b[3].section == 1 && b[7].cmd->key == 22 && b[7].section == 1);
    CHECK(b[8].cmd->key == 19 && b[8].section == 2 && b[11].cmd->key == DAWN_KEY_CMD_THEME);
    // AI: the chat and the model, then the actions; the model's caption is its state.
    n = ribbon_buttons(RIBBON_TAB_AI, 2, true, true, b, RIBBON_MAX);
    CHECK(n == 6);
    CHECK(b[0].cmd->key == 31 && b[1].cmd->key == DAWN_KEY_AI_MODEL && b[1].section == 0);
    CHECK(b[2].cmd->key == 26 && b[2].section == 1 && b[3].cmd->key == DAWN_KEY_AI_TITLE);
    CHECK(b[4].cmd->key == DAWN_KEY_AI_FORMAT && b[5].cmd->key == 17);
    CHECK(b[0].width == RIBBON_MODEL_STATE_COLS && b[1].width == RIBBON_MODEL_STATE_COLS); // "not loaded"
    CHECK(ribbon_tab_of(commands_find(26)) == RIBBON_TAB_FILE); // undo's first ribbon is File's
    CHECK(ribbon_tab_of(commands_find(DAWN_KEY_FMT_BOLD)) == RIBBON_TAB_FORMAT);
    CHECK(ribbon_tab_of(commands_find(15)) == RIBBON_TAB_FORMAT);
    CHECK(ribbon_tab_of(commands_find(17)) == RIBBON_TAB_AI);
    CHECK(ribbon_tab_of(NULL) == -1);
    // Off the File ribbon, in the full menu: select all, the deletes, indent, plain text.
    CHECK(ribbon_tab_of(commands_find(1)) == -1 && commands_in_menu(commands_find(1)));
    CHECK(ribbon_tab_of(commands_find(21)) == -1 && ribbon_tab_of(commands_find(18)) == -1);
    CHECK(!commands_in_menu(commands_find(DAWN_KEY_FMT_BOLD)) && !commands_in_menu(commands_find(DAWN_KEY_CMD_MENU)));

    // The phone at scale 2 (51 cells): six 7-cell buttons a row, so the format group's sixteen
    // take three rows, and help (which cannot sit beside a section of three rows) starts a fourth
    // under a rule; a blank row between two rows, the rule drawn in it.
    RibbonSlot s[RIBBON_MAX];
    RibbonFrame f;
    n = ribbon_buttons(RIBBON_TAB_FORMAT, 2, true, true, b, RIBBON_MAX);
    CHECK(ribbon_layout(b, n, 51, 2, true, 40, s, &f));
    CHECK(f.rows == 4 && f.height == 4 * 3 + 2 + RIBBON_RULE && f.pages == 1 && f.more_width == 0);
    // Spread to the full 51 cells: four cells to spare, too few to widen six buttons, go to the
    // gaps (the last four a cell wider); the last row of four keeps the columns above it, and
    // help's four, a row of their own, widen to twelve.
    CHECK(s[0].col == 0 && s[1].col == 8 && s[2].col == 17 && s[5].col == 44 && s[5].row == 0);
    CHECK(s[5].width == 7 && s[5].col + s[5].width == 51);
    CHECK(s[6].col == 0 && s[6].row == 4 && !s[6].rule_above && s[styles - 1].row == 8);
    CHECK(s[styles - 1].col == 26 && s[styles - 1].width == 7);
    CHECK(s[styles].col == 0 && s[styles].row == 11 + RIBBON_RULE && s[styles].rule_above);
    CHECK(s[styles].width == 12 && s[n - 1].row == s[styles].row && s[n - 1].col == 39);
    // With the keyboard up, room for two rows and the gap: the ribbon pages, a "more" button at the
    // right.
    CHECK(ribbon_layout(b, n, 51, 2, true, 7, s, &f));
    CHECK(f.rows == 2 && f.height == 7 && f.pages == 3 && f.more_width == 4 && f.more_col == 47);
    CHECK(s[4].page == 0 && s[4].row == 4);
    // Four a row in the 46 cells left of it, each three cells wider and the gaps two.
    CHECK(s[3].col == 36 && s[3].width == 10 && s[1].col == 12);
    CHECK(s[styles].page == 2 && s[styles].row == 0 && !s[styles].rule_above);
    // Room for two rows but not the gap: a row a page.
    CHECK(ribbon_layout(b, n, 51, 2, true, 6, s, &f) && f.rows == 1 && f.height == 3 && f.pages == 5);
    // Not even one row: no ribbon.
    CHECK(!ribbon_layout(b, n, 51, 2, true, 2, s, &f) && f.height == 0);
    // A wide pane holds the whole ribbon on one row, help beside the styles after a divider.
    n = ribbon_buttons(RIBBON_TAB_FORMAT, 1, false, false, b, RIBBON_MAX);
    CHECK(ribbon_layout(b, n, 200, 1, false, 40, s, &f) && f.rows == 1 && f.height == 2);
    CHECK(s[styles].row == 0 && s[styles].col == s[styles - 1].col + s[styles - 1].width + RIBBON_DIVIDER);
    // A wrapping section spreads evenly: in 45 cells five 7-cell buttons fit a row, so the sixteen
    // styles go four rows of four rather than leave one alone on a fourth.
    CHECK(ribbon_layout(b, n, 45, 1, false, 40, s, &f) && f.rows == 5 && f.pages == 1);
    CHECK(s[3].row == 0 && s[3].col == 35 && s[3].width == 10 && s[4].row == 2 && s[4].col == 0);
    CHECK(s[1].col == 11 && s[2].col == 23); // the two cells left over widen the last two gaps
    CHECK(s[styles - 1].row == 6 && s[styles - 1].col == 35 && s[styles].row == 8 + RIBBON_RULE);
    // The phone at scale 1 with text sized (53 cells): the same four rows, each a row taller (a
    // 2-row box and its caption), no gap between them but the rule above help's.
    for (int32_t sized = 0; sized < 2; sized++) {
        n = ribbon_buttons(RIBBON_TAB_FORMAT, 1, true, sized != 0, b, RIBBON_MAX);
        CHECK(n == styles + 4 && b[0].width == 7);
        CHECK(ribbon_layout(b, n, 53, 1, sized != 0, 40, s, &f) && f.rows == 4 && f.pages == 1);
        int32_t bh = sized ? 3 : 2;
        CHECK(f.height == 4 * bh + RIBBON_RULE && s[6].row == bh && s[styles].row == 3 * bh + RIBBON_RULE);
    }

    // File on the phone (51 or 53 cells, six buttons a row): 3 | 5 | 4 go on two rows with no rule,
    // a blank row apart, the clipboard going on after the note's three and wrapping, the finding
    // four after it; as blocks they would take three.
    n = ribbon_buttons(RIBBON_TAB_FILE, 2, true, true, b, RIBBON_MAX);
    // Spread to the edge: at 51 the two spare cells widen the second and fourth gaps of each row,
    // at 53 each of the four gaps takes one (dividers stay three cells).
    for (int32_t room = 51; room <= 53; room += 2) {
        int32_t d = room == 53 ? 1 : 0;
        CHECK(ribbon_layout(b, n, room, 2, true, 40, s, &f));
        CHECK(f.rows == 2 && f.height == 2 * 3 + 1 && f.pages == 1);
        CHECK(s[1].col == 8 + d && s[2].row == 0 && s[2].col == 17 + d && s[3].row == 0 && s[3].col == 27 + d);
        CHECK(s[5].col == 44 + 2 * d && s[5].col + s[5].width == room);
        CHECK(s[6].row == 4 && s[6].col == 0 && !s[6].rule_above && s[7].col == 8 + d);
        CHECK(s[8].row == 4 && s[8].col == 18 + d && s[11].row == 4 && s[11].col == 44 + 2 * d);
    }
    // Without the font the same.
    n = ribbon_buttons(RIBBON_TAB_FILE, 2, false, true, b, RIBBON_MAX);
    CHECK(ribbon_layout(b, n, 53, 2, true, 40, s, &f) && f.rows == 2);
    // The help four would split two and two going on after the styles' last row, taking no fewer
    // rows, so they keep a row of their own (the Formatting case above, at 53 too).
    n = ribbon_buttons(RIBBON_TAB_FORMAT, 2, true, true, b, RIBBON_MAX);
    CHECK(ribbon_layout(b, n, 53, 2, true, 40, s, &f) && f.rows == 4 && s[styles].rule_above && s[styles].col == 0);
    // AI: its buttons are ten wide ("not loaded"), four a row at 53, so the chat and the model, then
    // the four actions under a rule (drawn in the gap, so no taller); one row from 67 cells (six and a divider), where at 65 and 66
    // (six a row, but not with the divider) the actions keep a row of their own.
    n = ribbon_buttons(RIBBON_TAB_AI, 2, true, true, b, RIBBON_MAX);
    CHECK(ribbon_layout(b, n, 53, 2, true, 40, s, &f) && f.rows == 2 && f.height == 2 * 3 + RIBBON_RULE);
    // Spread: the chat and the model take half the row each, the four actions twelve cells each.
    CHECK(s[1].col == 27 && s[1].width == 26 && s[2].width == 12);
    CHECK(s[2].rule_above && s[2].row == 3 + RIBBON_RULE && s[2].col == 0 && s[5].row == s[2].row && s[5].col == 41);
    CHECK(ribbon_layout(b, n, 67, 2, true, 40, s, &f) && f.rows == 1 && f.height == 3);
    CHECK(s[2].row == 0 && s[2].col == 24 && s[5].col == 57 && s[5].width == 10);
    CHECK(ribbon_layout(b, n, 66, 2, true, 40, s, &f) && f.rows == 2 && s[2].rule_above && s[5].col == 51);
    CHECK(s[1].col == 34 && s[1].width == 32);

    // Every tab at every scale, width and height, with and without the Nerd Font and text sizing.
    RibbonTab tabs[RIBBON_TAB_COUNT];
    int32_t nt = tabs_items(tabs, RIBBON_TAB_COUNT);
    static const int32_t HEIGHTS[] = { 1, 3, 6, 7, 9, 14, 40 };
    for (int32_t t = 0; t < nt; t++)
        for (int32_t scale = 1; scale <= UI_SCALE_MAX; scale++)
            for (int32_t room = 30; room <= 120; room++)
                for (int32_t icons = 0; icons < 2; icons++)
                    for (int32_t sized = 0; sized < 2; sized++)
                        for (size_t hi = 0; hi < sizeof(HEIGHTS) / sizeof(HEIGHTS[0]); hi++)
                            check_ribbon(tabs[t], room, scale, icons != 0, sized != 0, HEIGHTS[hi]);
    // The phone's widths at the phone's scale always show the ribbon with room for one row.
    for (int32_t t = 0; t < nt; t++)
        for (int32_t room = 30; room <= 120; room++) {
            n = ribbon_buttons(tabs[t], 2, true, true, b, RIBBON_MAX);
            CHECK(ribbon_layout(b, n, room, 2, true, 3, s, &f));
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

    // Every command is listed once, under its group's heading, with the ribbon it is on, or the
    // menu when it is on none.
    int32_t count = 0;
    const Command* all = commands_all(&count);
    for (int32_t c = 0; c < count; c++) {
        int32_t found = 0;
        for (int32_t i = 0; i < n; i++) {
            if (rows[i].kind != HELP_ROW_ITEM || rows[i].text != all[c].label)
                continue;
            found++;
            CHECK(strcmp(rows[i].key, all[c].shortcut) == 0);
            int32_t tab = ribbon_tab_of(&all[c]);
            CHECK(strcmp(rows[i].tap, tab >= 0 ? tabs_tap((RibbonTab)tab) : "menu") == 0);
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
        if (strcmp(rows[i].text, "select all") == 0 || strcmp(rows[i].text, "plain text") == 0)
            CHECK(strcmp(rows[i].tap, "menu") == 0);
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
