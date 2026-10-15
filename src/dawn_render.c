// dawn_render.c

#include "dawn_render.h"
#include "dawn_block.h"
#include "dawn_gap.h"
#include "dawn_hit.h"
#include "dawn_image.h"
#include "dawn_md.h"
#include "dawn_modal.h"
#include "dawn_notepath.h"
#include "dawn_notice.h"
#include "dawn_search.h"
#include "dawn_session.h"
#include "dawn_status.h"
#include "dawn_theme.h"
#include "dawn_timer.h"
#include "dawn_toc.h"
#include "dawn_utils.h"
#include "dawn_wrap.h"
#include <stdio.h>
#include <string.h>

// #region Platform Output Helpers

static void platform_write_str(const char* str)
{
    DAWN_BACKEND(app)->write_str(str, strlen(str));
}

static void platform_write_char(char c)
{
    DAWN_BACKEND(app)->write_char(c);
}

static void platform_clear_screen(void)
{
    DAWN_BACKEND(app)->clear_screen();
}

static void platform_set_cursor_visible(bool visible)
{
    DAWN_BACKEND(app)->set_cursor_visible(visible);
}

static void platform_set_bold(bool enabled)
{
    DAWN_BACKEND(app)->set_bold(enabled);
}

static void platform_reset_attrs(void)
{
    DAWN_BACKEND(app)->reset_attrs();
}

// #endregion

// #region Utility Functions

void render_clear(void)
{
    set_bg(get_bg());
    platform_clear_screen();
    for (int32_t r = 0; r < app.rows; r++) {
        move_to(r + 1, 1);
        for (int32_t c = 0; c < app.cols; c++)
            platform_write_char(' ');
    }
}

//! The column render_center_text() starts text at
static int32_t center_col(const char* text)
{
    int32_t col = (app.cols - (int32_t)strlen(text) + 2) / 2;
    return col < 1 ? 1 : col;
}

void render_center_text(int32_t row, const char* text, DawnColor fg)
{
    move_to(row, center_col(text));
    set_fg(fg);
    platform_write_str(text);
}

//! render_center_text() for a hint line, whose hints ("[esc] back") are then tappable
static void render_center_hints(int32_t row, const char* text, DawnColor fg)
{
    render_center_text(row, text, fg);
    int32_t col = center_col(text);
    hit_add_hints(col, row, text, app.cols - col + 1, true);
}

void render_popup_box(int32_t width, int32_t height, int32_t* out_top, int32_t* out_left)
{
    int32_t top = (app.rows - height) / 2;
    int32_t left = (app.cols - width) / 2;
    if (top < 1)
        top = 1;
    if (left < 1)
        left = 1;

    DawnColor bg = get_modal_bg();
    image_mask_region(left, top, width, height, bg);
    DawnColor border = get_border();

    // Top border
    move_to(top, left);
    set_bg(bg);
    set_fg(border);
    platform_write_str("╭");
    for (int32_t i = 0; i < width - 2; i++)
        platform_write_str("─");
    platform_write_str("╮");

    // Middle rows
    for (int32_t r = 1; r < height - 1; r++) {
        move_to(top + r, left);
        set_fg(border);
        platform_write_str("│");
        set_fg(get_fg());
        for (int32_t i = 0; i < width - 2; i++)
            platform_write_char(' ');
        set_fg(border);
        platform_write_str("│");
    }

    // Bottom border
    move_to(top + height - 1, left);
    set_fg(border);
    platform_write_str("╰");
    for (int32_t i = 0; i < width - 2; i++)
        platform_write_str("─");
    platform_write_str("╯");

    if (out_top)
        *out_top = top;
    if (out_left)
        *out_left = left;
}

// #endregion

// #region Screen Renderers

static void render_text_at(int32_t row, int32_t col, const char* text, DawnColor fg)
{
    move_to(row, col);
    set_fg(fg);
    platform_write_str(text);
}

void render_welcome(void)
{
    render_clear();

    // Use most of the available space
    int32_t margin_h = app.cols > 100 ? 8 : (app.cols > 60 ? 4 : 2);
    int32_t margin_v = app.rows > 30 ? 3 : 2;
    int32_t content_left = margin_h + 1;
    int32_t content_right = app.cols - margin_h;
    int32_t content_width = content_right - content_left;

    // Vertical layout
    int32_t top_row = margin_v + 1;
    int32_t bottom_row = app.rows - margin_v;
    int32_t center_row = (top_row + bottom_row) / 2;

    // Clean block letter logo
    static const char* logo[] = {
        "█▀▄ ▄▀█ █ █ █ █▄ █",
        "█▄▀ █▀█ ▀▄▀▄▀ █ ▀█",
    };
    int32_t logo_height = 2;
    int32_t logo_width = 19;

    // Center logo vertically - position it above center
    int32_t logo_start = center_row - logo_height - 2;
    if (logo_start < top_row)
        logo_start = top_row;

    set_fg(get_fg());
    for (int32_t i = 0; i < logo_height; i++) {
        int32_t col = (app.cols - logo_width + 2) / 2;
        if (col < 1)
            col = 1;
        move_to(logo_start + i, col);
        platform_write_str(logo[i]);
    }

    // Tagline below logo
    render_center_text(logo_start + logo_height + 1, "draft anything, write now", get_dim());

    // Actions grid - positioned below center
    int32_t actions_row = center_row + 2;
    int32_t col1 = content_left + content_width / 4 - 8;
    int32_t col2 = content_left + content_width / 2 + content_width / 4 - 8;
    if (col1 < content_left + 2)
        col1 = content_left + 2;
    if (col2 < col1 + 20)
        col2 = col1 + 20;

    int32_t row = actions_row;
    render_text_at(row, col1, "enter", get_accent());
    render_text_at(row, col1 + 6, " write", get_dim());
    render_text_at(row, col2, "h", get_accent());
    render_text_at(row, col2 + 2, " history", get_dim());

    row += 2;
    render_text_at(row, col1, "t", get_accent());
    render_text_at(row, col1 + 6, " timer", get_dim());
    render_text_at(row, col2, "d", get_accent());
    render_text_at(row, col2 + 2, " theme", get_dim());

    row += 2;
    render_text_at(row, col1, "q", get_accent());
    render_text_at(row, col1 + 6, " quit", get_dim());
    render_text_at(row, col2, "?", get_accent());
    render_text_at(row, col2 + 2, " help", get_dim());

    // Each action, key and word, is also a tap on that key.
    hit_add(col1, actions_row, 12, 1, HIT_KEY, '\r', 0); // "enter  write"
    hit_add(col2, actions_row, 10, 1, HIT_KEY, 'h', 0); // "h  history"
    hit_add(col1, actions_row + 2, 12, 1, HIT_KEY, 't', 0); // "t      timer"
    hit_add(col2, actions_row + 2, 8, 1, HIT_KEY, 'd', 0); // "d  theme"
    hit_add(col1, actions_row + 4, 11, 1, HIT_KEY, 'q', 0); // "q      quit"
    hit_add(col2, actions_row + 4, 7, 1, HIT_KEY, '?', 0); // "?  help"

    // The status panel (dawn_status) goes in the bottom right of the space under the actions, a
    // blank row below them where there is room, at most four rows. It also says "ai ready" here.
    int32_t panel_last = bottom_row - 1;
    int32_t panel_first = row + 2;
    if (panel_first > panel_last)
        panel_first = row + 1;
    if (panel_first < panel_last - 3)
        panel_first = panel_last - 3;
    int32_t panel_cols = app.cols - 2 < 48 ? app.cols - 2 : 48;
    status_area(panel_first, panel_last, app.cols, panel_cols, true);

    // Bottom status bar - like editor status bar
    move_to(bottom_row, content_left);

    // Left: timer setting
    set_fg(get_dim());
    char timer_str[16];
    if (app.timer_mins == 0)
        snprintf(timer_str, sizeof(timer_str), "no timer");
    else
        snprintf(timer_str, sizeof(timer_str), "%d min", app.timer_mins);
    platform_write_str(timer_str);
    hit_add(content_left, bottom_row, (int32_t)strlen(timer_str), 1, HIT_KEY, 't', 0);

    // Right: theme
    const char* theme_str = app.theme == THEME_DARK ? "dark" : "light";
    int32_t theme_col = content_right - (int32_t)strlen(theme_str);
    move_to(bottom_row, theme_col);
    set_fg(get_dim());
    platform_write_str(theme_str);
    hit_add(theme_col, bottom_row, (int32_t)strlen(theme_str), 1, HIT_KEY, 'd', 0);
}

// #region First-run tour

//! Lines of text on a tour card
#define TOUR_LINES 3

//! One card of the first-run tour: a word for its title and three short lines
typedef struct {
    const char* title;
    const char* lines[TOUR_LINES];
} TourCard;

//! The tour's cards in order. Every line fits a phone held upright (53 columns) without wrapping.
static const TourCard TOUR_CARDS[] = {
    { "write",
        { "press enter on the start screen to begin a note.",
            "dawn saves as you type. esc brings you back.",
            "history (h) lists your notes." } },
    { "format",
        { "tap Formatting at the top: bold, headings, lists,",
            "or type Markdown: **bold**, # heading, - list.",
            "? opens help, with a Markdown cheatsheet." } },
#if HAS_LIBAI
    { "ask",
        { "tap AI to talk about your note, ask for a rewrite,",
            "or have it read aloud.",
            "it can edit the note; ctrl+z undoes." } },
#endif
};

int32_t render_tour_cards(void)
{
    return (int32_t)(sizeof(TOUR_CARDS) / sizeof(TOUR_CARDS[0]));
}

void render_tour(void)
{
    render_clear();

    int32_t cards = render_tour_cards();
    int32_t card = app.tour_card < 0 ? 0 : (app.tour_card >= cards ? cards - 1 : app.tour_card);
    const TourCard* c = &TOUR_CARDS[card];
    bool last = card == cards - 1;

    // The welcome screen's margins. The card sits centred above the footer: title, text, dots,
    // with a blank row between them where there is room; below 16 rows (a phone with its keyboard
    // up has about 12) the blank rows go, never the text.
    int32_t margin_v = app.rows > 30 ? 3 : 2;
    int32_t top_row = margin_v + 1;
    int32_t footer_row = app.rows - margin_v;
    int32_t gap = app.rows >= 16 ? 1 : 0;
    int32_t height = 1 + gap + TOUR_LINES + gap + 1;
    int32_t row = top_row + (footer_row - 1 - top_row - height) / 2;
    if (row < 1)
        row = 1;
    if (footer_row < row + height)
        footer_row = row + height;

    // Title, in the welcome logo's colour
    move_to(row, center_col(c->title));
    set_fg(get_fg());
    platform_set_bold(true);
    platform_write_str(c->title);
    platform_reset_attrs();
    set_bg(get_bg());

    // The text, dim, as one block centred on its widest line (cut at the screen's edge)
    int32_t room = app.cols - 2 > 1 ? app.cols - 2 : 1;
    int32_t widest = 0;
    for (int32_t i = 0; i < TOUR_LINES; i++) {
        int32_t w = (int32_t)strlen(c->lines[i]);
        if (w > widest)
            widest = w;
    }
    if (widest > room)
        widest = room;
    int32_t left = (app.cols - widest + 2) / 2;
    if (left < 1)
        left = 1;
    row += 1 + gap;
    set_fg(get_dim());
    for (int32_t i = 0; i < TOUR_LINES; i++) {
        size_t len = strlen(c->lines[i]);
        move_to(row + i, left);
        DAWN_BACKEND(app)->write_str(c->lines[i], len < (size_t)room ? len : (size_t)room);
    }

    // Dots, the card shown filled: "• ○ ○"
    row += TOUR_LINES + gap;
    int32_t dots_col = (app.cols - (cards * 2 - 1) + 2) / 2;
    move_to(row, dots_col < 1 ? 1 : dots_col);
    for (int32_t i = 0; i < cards; i++) {
        if (i > 0)
            platform_write_char(' ');
        set_fg(i == card ? get_accent() : get_dim());
        platform_write_str(i == card ? "•" : "○");
    }

    // The footer's hints are taps on their keys. Opened again from a command, esc closes it and
    // the last card's enter goes back to the note.
    const char* hint;
    if (last)
        hint = app.tour_pushed ? "[enter] done" : "[enter] start writing";
    else
        hint = app.tour_pushed ? "[enter] next   [esc] close" : "[enter] next   [esc] skip";
    render_center_hints(footer_row, hint, get_dim());
}

// #endregion

void render_timer_select(void)
{
    render_clear();
    int32_t cy = app.rows / 2;

    render_center_text(cy - 5, "select timer", get_fg());

    for (size_t i = 0; i < NUM_PRESETS; i++) {
        char buf[32];
        if (TIMER_PRESETS[i] == 0) {
            snprintf(buf, sizeof(buf), "%s no timer %s",
                (int32_t)i == app.preset_idx ? ">" : " ",
                (int32_t)i == app.preset_idx ? "<" : " ");
        } else {
            snprintf(buf, sizeof(buf), "%s %d min %s",
                (int32_t)i == app.preset_idx ? ">" : " ",
                TIMER_PRESETS[i],
                (int32_t)i == app.preset_idx ? "<" : " ");
        }
        render_center_text(cy - 2 + (int32_t)i, buf,
            (int32_t)i == app.preset_idx ? get_accent() : get_dim());
        // A tap anywhere on the row picks it and confirms, as j/k then enter would.
        hit_add(1, cy - 2 + (int32_t)i, app.cols, 1, HIT_ROW, '\r', (int32_t)i);
    }

    render_center_hints(app.rows - 2, "[j/k] select   [enter] confirm   [esc] back", get_dim());
}

void render_style_select(void)
{
    render_clear();
    int32_t cy = app.rows / 2;

    render_center_text(cy - 4, "select style", get_fg());

    const char* names[] = { "minimal", "typewriter", "elegant" };
    const char* descs[] = { "clean focus", "monospace feel", "italic grace" };

    for (int32_t i = 0; i < 3; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%s %s %s",
            i == (int32_t)app.style ? ">" : " ",
            names[i],
            i == (int32_t)app.style ? "<" : " ");
        render_center_text(cy - 1 + i * 2, buf, i == (int32_t)app.style ? get_accent() : get_dim());
        render_center_text(cy + i * 2, descs[i], get_dim());
        hit_add(1, cy - 1 + i * 2, app.cols, 2, HIT_ROW, '\r', i); // the name and its line
    }

    render_center_hints(app.rows - 2, "[j/k] select   [enter] confirm   [esc] back", get_dim());
}

//! "2 min ago" / "3 hr ago" / "just now" for the activity list. then_sec/now_sec are both
//! DAWN_CLOCK_SEC wall-clock values (notice_history()'s posted_sec came from the same clock).
static void format_relative_time(int64_t then_sec, int64_t now_sec, char* buf, size_t n)
{
    int64_t d = now_sec - then_sec;
    if (d < 0)
        d = 0;
    if (d < 60) {
        snprintf(buf, n, "just now");
    } else if (d < 3600) {
        int64_t mins = d / 60;
        snprintf(buf, n, "%lld min ago", (long long)mins);
    } else if (d < 86400) {
        int64_t hrs = d / 3600;
        snprintf(buf, n, "%lld hr ago", (long long)hrs);
    } else {
        int64_t days = d / 86400;
        snprintf(buf, n, "%lld d ago", (long long)days);
    }
}

// #region Help

//! The help box's widest: room for the shortcut pages' three columns with two spaces around each
#define HELP_WIDTH 54

//! Tallest the help box grows
#define HELP_HEIGHT 26

//! What tab says it turns to, by page ("[tab] markdown")
static const char* const HELP_PAGE_NAMES[HELP_PAGE_COUNT] = {
    [HELP_PAGE_SHORTCUTS] = "shortcuts",
    [HELP_PAGE_CHEATSHEET] = "markdown",
    [HELP_PAGE_ACTIVITY] = "activity",
    [HELP_PAGE_MEANING] = "meaning",
};

//! Display columns of a UTF-8 string, counting each code point as one column
static int32_t help_cols(const char* s)
{
    int32_t n = 0;
    for (; *s; s++) {
        if (((unsigned char)*s & 0xC0) != 0x80)
            n++;
    }
    return n;
}

bool render_help_neighbour(int32_t dir, HelpPage* page, int32_t* screen)
{
    int32_t p = (int32_t)app.help_page;
    int32_t n = render_help_screens(app.help_page);
    int32_t s = (app.help_screen < n ? app.help_screen : n - 1) + dir;
    for (;;) {
        if (s >= 0 && s < n) {
            *page = (HelpPage)p;
            *screen = s;
            return true;
        }
        p += dir;
        if (p < 0 || p >= HELP_PAGE_COUNT)
            return false;
        n = render_help_screens((HelpPage)p);
        s = dir > 0 ? 0 : n - 1;
    }
}

//! A help page's footer on row, centred in the box: [<-] back where there is a page before, [tab]
//! and the page it turns to where there is one after ("next" within a page), and [esc] close.
//! Each item has a slot of its own, the same on every screen and left blank where the item has
//! nothing to do, so a finger tapping one spot turns screen after screen. Its hints are tappable
//! up to the box's right border.
static void help_footer(int32_t row, int32_t left, int32_t width)
{
    HelpPage page;
    int32_t screen;
    bool back = render_help_neighbour(-1, &page, &screen);
    bool next = render_help_neighbour(1, &page, &screen);
    const char* next_name = !next ? "" : page == app.help_page ? "next" : HELP_PAGE_NAMES[page];

    // The tab slot holds the longest name tab can turn to (any page but the first).
    int32_t name_w = (int32_t)strlen("next");
    for (int32_t p = 1; p < HELP_PAGE_COUNT; p++) {
        if ((int32_t)strlen(HELP_PAGE_NAMES[p]) > name_w)
            name_w = (int32_t)strlen(HELP_PAGE_NAMES[p]);
    }
    char tab[32] = "";
    if (next)
        snprintf(tab, sizeof(tab), "[tab] %s", next_name);

    char footer[96];
    snprintf(footer, sizeof(footer), "%-11s%-*s[esc] close", back ? "[<-] back" : "", (int)(6 + name_w + 2), tab);
    if ((int32_t)strlen(footer) > width - 4) // too narrow for the words: the keys alone still turn and close
        snprintf(footer, sizeof(footer), "%-6s%-7s[esc]", back ? "[<-]" : "", next ? "[tab]" : "");

    int32_t col = left + (width - (int32_t)strlen(footer)) / 2;
    if (col < left + 1)
        col = left + 1;
    move_to(row, col);
    set_fg(get_dim());
    platform_write_str(footer);
    hit_add_hints(col, row, footer, left + width - 1 - col, true);
}

//! HELP_PAGE_ACTIVITY: the activity list (notice_history()), newest first, with relative
//! times - where a notice can be found again after it has faded off the status line.
static void render_help_activity(int32_t top, int32_t left, int32_t width, int32_t height)
{
    int32_t col1 = left + 4;

    set_bg(get_modal_bg());

    move_to(top + 2, left + width / 2 - 7);
    set_fg(get_fg());
    platform_set_bold(true);
    platform_write_str("ACTIVITY");
    platform_reset_attrs();
    set_bg(get_modal_bg());

    #define NOTICE_HELP_MAX 18
    const char* texts[NOTICE_HELP_MAX];
    int64_t times[NOTICE_HELP_MAX];
    int32_t n = notice_history(texts, times, NOTICE_HELP_MAX);
    int64_t now_sec = DAWN_BACKEND(app)->clock(DAWN_CLOCK_SEC);

    int32_t cy = top + 4;
    int32_t max_rows = height - 6;
    if (n == 0) {
        move_to(cy, col1);
        set_fg(get_dim());
        platform_write_str("nothing yet");
    } else {
        for (int32_t i = 0; i < n && i < max_rows; i++) {
            char when[24];
            format_relative_time(times[i], now_sec, when, sizeof(when));

            move_to(cy + i, col1);
            set_fg(get_dim());
            char line[64];
            snprintf(line, sizeof(line), "%s", texts[i]);
            platform_write_str(line);

            int32_t when_col = left + width - 4 - (int32_t)strlen(when);
            if (when_col > col1 + (int32_t)strlen(line) + 1) {
                move_to(cy + i, when_col);
                platform_write_str(when);
            }
        }
    }
    #undef NOTICE_HELP_MAX

    // Where there is a meaning page, the footer's tab points on to it.
    help_footer(top + height - 2, left, width);
}

static int32_t hist_write_fit(const char* s, int32_t max_cols);
static int32_t conflict_wrap(const char* text, int32_t width, size_t* starts, size_t* lens, int32_t max);

//! What the meaning index is doing, in words, for the help page the writer went to: unlike the
//! search box it also says when there is no embedder yet and how to get one.
static void help_meaning_phase(const EmbedStatus* st, char* out, size_t cap)
{
    switch (st->phase) {
    case EMBED_PHASE_OFF:
        snprintf(out, cap, "%s", embed_enabled() ? "starting" : "off");
        break;
    case EMBED_PHASE_DISCOVERING:
        snprintf(out, cap, "looking for an embedder");
        break;
    case EMBED_PHASE_NO_EMBEDDER:
        snprintf(out, cap, "no embedder installed \xE2\x80\x94 get EmbeddingGemma in Termux Launcher's AI settings");
        break;
    case EMBED_PHASE_INDEXING:
        if (st->current_title[0])
            snprintf(out, cap, "indexing %d/%d \xC2\xB7 %s", st->notes_done, st->notes_total, st->current_title);
        else
            snprintf(out, cap, "indexing %d/%d", st->notes_done, st->notes_total);
        break;
    case EMBED_PHASE_PAUSED:
        snprintf(out, cap, "paused (chat busy)");
        break;
    case EMBED_PHASE_WAITING: {
        int64_t wait = st->retry_at_ms - DAWN_BACKEND(app)->clock(DAWN_CLOCK_MS);
        if (wait > 0)
            snprintf(out, cap, "waiting for TAI \xC2\xB7 %llds", (long long)((wait + 999) / 1000));
        else
            snprintf(out, cap, "waiting for TAI");
    } break;
    case EMBED_PHASE_IDLE:
        snprintf(out, cap, "index ready \xC2\xB7 %d note%s", st->notes_indexed, st->notes_indexed == 1 ? "" : "s");
        break;
    case EMBED_PHASE_FAILED:
        snprintf(out, cap, "failed: %s", st->error[0] ? st->error : "unknown error");
        break;
    }
}

//! HELP_PAGE_MEANING (only where DAWN_EMBED_LIVE): the meaning index's state, model and
//! size, then its switch (m) and its rebuild (r, twice), each also a row to tap. It has its own
//! height; short screens lose the blank rows around the actions first.
static void render_help_meaning(int32_t width)
{
    EmbedStatus st;
    embed_status(&st);
    bool on = embed_enabled();

    int32_t text_w = width - 6;
    int32_t label_w = 7; // "status " / "model  " / "notes  "
    int32_t value_w = text_w - label_w > 8 ? text_w - label_w : 8;

    char phase[192];
    if (on)
        help_meaning_phase(&st, phase, sizeof(phase));
    else
        snprintf(phase, sizeof(phase), "off");
    enum { PHASE_LINES = 4 };
    size_t starts[PHASE_LINES], lens[PHASE_LINES];
    int32_t phase_n = conflict_wrap(phase, value_w, starts, lens, PHASE_LINES);
    if (phase_n < 1) {
        phase_n = 1;
        starts[0] = lens[0] = 0;
    }

    // Rows: border, blank, title, blank, status lines, model, notes, blank, toggle, gap, rebuild,
    // blank, footer, border.
    int32_t gap = 1, tail = 1;
    int32_t height = 12 + phase_n + gap + tail;
    if (height > app.rows) {
        gap = tail = 0;
        height = 12 + phase_n;
    }

    int32_t top, left;
    render_popup_box(width, height, &top, &left);
    hit_add(left, top, width, height, HIT_INERT, 0, 0);
    set_bg(get_modal_bg());

    int32_t col = left + 3;
    move_to(top + 2, left + (width - 13) / 2);
    set_fg(get_fg());
    platform_set_bold(true);
    platform_write_str("MEANING INDEX");
    platform_reset_attrs();
    set_bg(get_modal_bg());

    int32_t row = top + 4;
    for (int32_t i = 0; i < phase_n; i++) {
        if (i == 0) {
            move_to(row, col);
            set_fg(get_dim());
            platform_write_str("status");
        }
        move_to(row++, col + label_w);
        set_fg(get_fg());
        DAWN_BACKEND(app)->write_str(phase + starts[i], lens[i]);
    }

    move_to(row, col);
    set_fg(get_dim());
    platform_write_str("model");
    move_to(row++, col + label_w);
    set_fg(st.model[0] ? get_fg() : get_dim());
    hist_write_fit(st.model[0] ? st.model : "none yet", value_w);

    move_to(row, col);
    set_fg(get_dim());
    platform_write_str("notes");
    move_to(row++, col + label_w);
    set_fg(get_fg());
    char notes[32];
    snprintf(notes, sizeof(notes), "%d indexed", st.notes_indexed);
    platform_write_str(notes);
    row++;

    // Each action owns its row across the box's width, so a tap anywhere on it counts.
    hit_add(left, row, width, 1, HIT_KEY, 'm', 0);
    move_to(row, col);
    set_fg(get_accent());
    platform_write_str("[m] ");
    set_fg(get_fg());
    platform_write_str(on ? "meaning search: on" : "meaning search: off");
    row += 1 + gap;

    // Off, a rebuild would wait unseen until the index is switched on again, so it is not offered.
    hit_add(left, row, width, 1, HIT_KEY, 'r', 0);
    bool armed = app.help_rebuild_armed
        && DAWN_BACKEND(app)->clock(DAWN_CLOCK_MS) - app.help_rebuild_armed < HELP_REBUILD_CONFIRM_MS;
    move_to(row, col);
    set_fg(on ? get_accent() : get_dim());
    platform_write_str("[r] ");
    set_fg(!on ? get_dim() : armed ? get_accent() : get_fg());
    hist_write_fit(!on ? "rebuild (turn it on first)" : armed ? "press r again to rebuild" : "rebuild the index", text_w - 4);

    help_footer(top + height - 2, left, width);
}

//! Where a paginated help page's parts go: the shortcut and cheatsheet pages share this box
typedef struct {
    int32_t top, left, width, height;
    int32_t title_row; //!< The page's name
    int32_t body_row; //!< Its first line
    int32_t per_page; //!< Lines the body holds
    int32_t footer_row;
} HelpFrame;

//! The help box's width: HELP_WIDTH, or the screen less a column each side when it is narrower
//! (a phone held upright).
static int32_t help_width(void)
{
    int32_t width = app.cols - 2 < HELP_WIDTH ? app.cols - 2 : HELP_WIDTH;
    if (width < 24)
        width = app.cols < 24 ? app.cols : 24;
    return width;
}

//! The paginated pages' box: HELP_HEIGHT rows, or the screen's height when it is shorter, so the
//! footer and its taps stay on screen. Below 16 rows (a phone with its keyboard up has about 12)
//! the blank rows around the title and above the footer go, leaving the body the room.
//! top and left are set once the box is drawn (help_frame_place).
static HelpFrame help_frame(void)
{
    HelpFrame f = { 0 };
    f.width = help_width();
    f.height = app.rows < HELP_HEIGHT ? app.rows : HELP_HEIGHT;
    f.per_page = f.height - (f.height >= 16 ? 7 : 4);
    if (f.per_page < 1)
        f.per_page = 1;
    return f;
}

//! Draw f's box and place its rows; a tap inside it on none of its hints does nothing, outside it
//! closes help.
static void help_frame_place(HelpFrame* f)
{
    render_popup_box(f->width, f->height, &f->top, &f->left);
    hit_add(f->left, f->top, f->width, f->height, HIT_INERT, 0, 0);
    bool roomy = f->height >= 16;
    f->title_row = f->top + (roomy ? 2 : 1);
    f->body_row = f->top + (roomy ? 4 : 2);
    f->footer_row = f->top + f->height - 2;
    set_bg(get_modal_bg());
}

//! A paginated page's name, centred, with "2/4" after it when it takes several screens
static void help_title(const HelpFrame* f, const char* title, int32_t screen, int32_t screens)
{
    char count[32] = "";
    if (screens > 1)
        snprintf(count, sizeof(count), " %d/%d", screen + 1, screens);
    int32_t w = (int32_t)(strlen(title) + strlen(count));
    move_to(f->title_row, f->left + (f->width - w) / 2);
    set_fg(get_fg());
    platform_set_bold(true);
    platform_write_str(title);
    platform_reset_attrs();
    set_bg(get_modal_bg());
    set_fg(get_dim());
    platform_write_str(count);
}

//! app.help_screen held within a page of screens screens
static int32_t help_screen_in(int32_t screens)
{
    if (app.help_screen >= screens)
        return screens > 0 ? screens - 1 : 0;
    return app.help_screen < 0 ? 0 : app.help_screen;
}

//! The shortcut pages' lines, paginated for f
//! @return the number of pages
static int32_t help_shortcut_pages(const HelpFrame* f, HelpRow* rows, int32_t* n, HelpSpan* pages)
{
    *n = help_rows(rows, HELP_ROWS_MAX);
    return help_paginate(rows, *n, f->per_page, pages, HELP_ROWS_MAX);
}

//! Where the shortcut pages' three columns start and how wide each may be drawn
typedef struct {
    int32_t label, key, tap; //!< Screen columns
    int32_t label_w, key_w, tap_w, heading_w;
} HelpColumns;

//! One line of a shortcut page on row: a group's heading, or what to do, its key and its tap
static void help_shortcut_line(const HelpRow* r, int32_t row, const HelpColumns* c)
{
    if (r->kind == HELP_ROW_HEADING) {
        move_to(row, c->label);
        set_fg(get_accent());
        platform_set_bold(true);
        hist_write_fit(r->text, c->heading_w);
        platform_reset_attrs();
        set_bg(get_modal_bg());
        return;
    }
    if (r->kind != HELP_ROW_ITEM)
        return;
    move_to(row, c->label);
    set_fg(get_fg());
    hist_write_fit(r->text, c->label_w);
    if (r->key[0]) {
        move_to(row, c->key);
        set_fg(get_accent());
        hist_write_fit(r->key, c->key_w);
    }
    if (r->tap[0] && c->tap_w > 0) {
        move_to(row, c->tap);
        set_fg(get_dim());
        hist_write_fit(r->tap, c->tap_w);
    }
}

//! HELP_PAGE_SHORTCUTS: every command by task, read from the command table (dawn_commands): what
//! it does, its key, and how to reach it by touch (the ribbon it is on, "File ribbon"), under the
//! table's group headings.
static void render_help_shortcuts(HelpFrame* f)
{
    HelpRow rows[HELP_ROWS_MAX];
    HelpSpan pages[HELP_ROWS_MAX];
    int32_t n = 0;
    int32_t count = help_shortcut_pages(f, rows, &n, pages);
    int32_t screen = help_screen_in(count);
    help_title(f, "SHORTCUTS", screen, count);

    // Each column is as wide as its widest entry; the room around them shrinks to fit the box, and
    // only then are the labels cut.
    int32_t label_w = 0, key_w = 0, tap_w = 0;
    for (int32_t i = 0; i < n; i++) {
        if (rows[i].kind != HELP_ROW_ITEM)
            continue;
        int32_t lw = help_cols(rows[i].text), kw = help_cols(rows[i].key);
        int32_t tw = help_cols(rows[i].tap);
        label_w = lw > label_w ? lw : label_w;
        key_w = kw > key_w ? kw : key_w;
        tap_w = tw > tap_w ? tw : tap_w;
    }
    // Left pad, label to key, key to tap, right pad
    static const int8_t SPACING[][4] = { { 2, 2, 2, 2 }, { 1, 2, 2, 1 }, { 1, 2, 1, 1 }, { 1, 1, 1, 1 } };
    enum { SPACINGS = sizeof(SPACING) / sizeof(SPACING[0]) };
    int32_t inner = f->width - 2;
    int32_t sp = 0;
    while (sp < SPACINGS - 1
        && SPACING[sp][0] + label_w + SPACING[sp][1] + key_w + SPACING[sp][2] + tap_w + SPACING[sp][3] > inner)
        sp++;
    int32_t over = SPACING[sp][0] + label_w + SPACING[sp][1] + key_w + SPACING[sp][2] + tap_w + SPACING[sp][3] - inner;
    if (over > 0)
        label_w = label_w - over > 4 ? label_w - over : 4;
    HelpColumns cols = {
        .label = f->left + 1 + SPACING[sp][0],
        .label_w = label_w,
        .key_w = key_w,
    };
    cols.key = cols.label + label_w + SPACING[sp][1];
    cols.tap = cols.key + key_w + SPACING[sp][2];
    cols.tap_w = f->left + f->width - 1 - cols.tap;
    cols.heading_w = f->left + f->width - 1 - cols.label;

    if (count > 0) {
        // A page that opens inside a group shows its heading again first.
        const HelpSpan* pg = &pages[screen];
        int32_t row = f->body_row;
        if (pg->heading >= 0)
            help_shortcut_line(&rows[pg->heading], row++, &cols);
        for (int32_t i = pg->first; i < pg->end; i++)
            help_shortcut_line(&rows[i], row++, &cols);
    }

    help_footer(f->footer_row, f->left, f->width);
}

//! How a cheatsheet row's "you get" side is drawn
typedef enum {
    CHEAT_PLAIN,
    CHEAT_BOLD,
    CHEAT_ITALIC,
    CHEAT_STRIKE,
    CHEAT_CODE, //!< The note's code colour (md_apply MD_CODE)
    CHEAT_H1, //!< The note's heading look (md_apply), without the larger size
    CHEAT_H2,
    CHEAT_QUOTE, //!< A dim bar before the text
    CHEAT_LINK, //!< Underlined
    CHEAT_DIM,
    CHEAT_TABLE, //!< Cells apart by a dim bar
} CheatLook;

//! The cheatsheet's rows: what you type, what you get, and how that looks
static const struct {
    const char* typed;
    const char* got;
    CheatLook look;
} CHEATS[] = {
    { "**bold**", "bold", CHEAT_BOLD },
    { "*italic*", "italic", CHEAT_ITALIC },
    { "~~struck~~", "struck", CHEAT_STRIKE },
    { "`code`", "code", CHEAT_CODE },
    { "# heading", "heading", CHEAT_H1 },
    { "## smaller heading", "smaller heading", CHEAT_H2 },
    { "- item", "• item", CHEAT_PLAIN },
    { "1. item", "1. item", CHEAT_PLAIN },
    { "- [ ] task", "☐ task", CHEAT_PLAIN },
    { "- [x] done", "☑ done", CHEAT_PLAIN },
    { "> quote", "quote", CHEAT_QUOTE },
    { "[text](url)", "text", CHEAT_LINK },
    { "![alt](image.png)", "the picture", CHEAT_DIM },
    { "---", "──────────", CHEAT_DIM },
    { "```", "a code block", CHEAT_CODE },
    { "| a | b |", "a │ b", CHEAT_TABLE },
};

#define CHEAT_COUNT ((int32_t)(sizeof(CHEATS) / sizeof(CHEATS[0])))

//! The line under the cheatsheet's rows
#define CHEAT_NOTE "the Formatting tab's ribbon types these"

//! Draw a cheatsheet row's "you get" side at the cursor, in at most room columns
static void help_cheat_got(int32_t i, int32_t room)
{
    const char* got = CHEATS[i].got;
    switch (CHEATS[i].look) {
    case CHEAT_PLAIN:
        set_fg(get_fg());
        break;
    case CHEAT_BOLD:
        set_fg(get_fg());
        set_bold(true);
        break;
    case CHEAT_ITALIC:
        set_fg(get_fg());
        set_italic(true);
        break;
    case CHEAT_STRIKE:
        set_fg(get_fg());
        set_strikethrough(true);
        break;
    case CHEAT_CODE:
        md_apply(MD_CODE);
        break;
    case CHEAT_H1:
        md_apply(MD_H1);
        set_bg(get_modal_bg());
        break;
    case CHEAT_H2:
        md_apply(MD_H2);
        set_bg(get_modal_bg());
        break;
    case CHEAT_QUOTE:
        set_fg(get_dim());
        platform_write_str("│ ");
        set_fg(get_fg());
        room -= 2;
        break;
    case CHEAT_LINK:
        set_fg(get_fg());
        set_underline(UNDERLINE_STYLE_SINGLE);
        break;
    case CHEAT_DIM:
        set_fg(get_dim());
        break;
    case CHEAT_TABLE: {
        // Drawn in parts so the bar between the cells is dim; got only gave the width.
        set_fg(get_fg());
        platform_write_str("a ");
        set_fg(get_dim());
        platform_write_str("│");
        set_fg(get_fg());
        platform_write_str(" b");
        got = "";
        break;
    }
    }
    hist_write_fit(got, room);
    // md_apply(0) also puts the text size back to one cell; the box's own ground comes back after.
    md_apply(0);
    set_bg(get_modal_bg());
}

//! HELP_PAGE_CHEATSHEET: Markdown for someone who has never written it, what you type on the left
//! in dim, what the note shows on the right in the note's own styles, and under them that the
//! ribbon types it for you. Split over screens like the shortcuts when the box is short.
static void render_help_cheatsheet(HelpFrame* f)
{
    int32_t rows_per = 0;
    bool blank = false;
    int32_t screens = help_pages_with_note(CHEAT_COUNT, f->per_page, &rows_per, &blank);
    int32_t screen = help_screen_in(screens);
    help_title(f, "MARKDOWN", screen, screens);

    int32_t typed_w = 0, got_w = 0;
    for (int32_t i = 0; i < CHEAT_COUNT; i++) {
        int32_t tw = help_cols(CHEATS[i].typed), gw = help_cols(CHEATS[i].got);
        typed_w = tw > typed_w ? tw : typed_w;
        got_w = gw > got_w ? gw : got_w;
    }
    int32_t gap = 3;
    int32_t inner = f->width - 2;
    if (typed_w + gap + got_w + 2 > inner)
        gap = 1;
    int32_t used = typed_w + gap + got_w;
    int32_t c_typed = f->left + 1 + (inner - used > 2 ? (inner - used) / 2 : 1);
    int32_t c_got = c_typed + typed_w + gap;
    int32_t got_room = f->left + f->width - 1 - c_got;

    int32_t first = screen * rows_per;
    int32_t end = first + rows_per < CHEAT_COUNT ? first + rows_per : CHEAT_COUNT;
    if (screen == screens - 1)
        end = CHEAT_COUNT;
    int32_t row = f->body_row;
    for (int32_t i = first; i < end; i++, row++) {
        move_to(row, c_typed);
        set_fg(get_dim());
        hist_write_fit(CHEATS[i].typed, typed_w);
        if (got_room > 0) {
            move_to(row, c_got);
            help_cheat_got(i, got_room);
        }
    }

    if (screen == screens - 1) {
        if (blank)
            row++;
        int32_t note_w = help_cols(CHEAT_NOTE);
        int32_t c_note = f->left + 1 + (inner - note_w > 2 ? (inner - note_w) / 2 : 1);
        move_to(row, c_note);
        set_fg(get_dim());
        hist_write_fit(CHEAT_NOTE, f->left + f->width - 1 - c_note);
    }

    help_footer(f->footer_row, f->left, f->width);
}

int32_t render_help_screens(HelpPage page)
{
    HelpFrame f = help_frame();
    switch (page) {
    case HELP_PAGE_SHORTCUTS: {
        HelpRow rows[HELP_ROWS_MAX];
        HelpSpan pages[HELP_ROWS_MAX];
        int32_t n = 0;
        int32_t count = help_shortcut_pages(&f, rows, &n, pages);
        return count > 0 ? count : 1;
    }
    case HELP_PAGE_CHEATSHEET: {
        int32_t rows_per = 0;
        bool blank = false;
        return help_pages_with_note(CHEAT_COUNT, f.per_page, &rows_per, &blank);
    }
    case HELP_PAGE_ACTIVITY:
        return 1;
    case HELP_PAGE_MEANING:
        return DAWN_EMBED_LIVE ? 1 : 0;
    default:
        return 0;
    }
}

void render_help(void)
{
    if (app.help_page == HELP_PAGE_MEANING && DAWN_EMBED_LIVE) {
        render_help_meaning(help_width());
        return;
    }

    HelpFrame f = help_frame();
    help_frame_place(&f);
    switch (app.help_page) {
    case HELP_PAGE_ACTIVITY:
        render_help_activity(f.top, f.left, f.width, f.height);
        break;
    case HELP_PAGE_CHEATSHEET:
        render_help_cheatsheet(&f);
        break;
    default:
        render_help_shortcuts(&f);
        break;
    }
}

// #endregion

//! Display columns of a UTF-8 string, counting each code point as one column
static int32_t hist_cols(const char* s, size_t len)
{
    int32_t n = 0;
    for (size_t i = 0; i < len; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80)
            n++;
    }
    return n;
}

//! Write s cut to max_cols columns, ending in an ellipsis when it was cut
//! @return columns written
static int32_t hist_write_fit(const char* s, int32_t max_cols)
{
    size_t len = strlen(s);
    if (max_cols <= 0)
        return 0;
    if (hist_cols(s, len) <= max_cols) {
        platform_write_str(s);
        return hist_cols(s, len);
    }
    int32_t keep_cols = max_cols - 1;
    size_t i = 0;
    int32_t n = 0;
    while (i < len) {
        size_t j = i + 1;
        while (j < len && ((unsigned char)s[j] & 0xC0) == 0x80)
            j++;
        if (n + 1 > keep_cols)
            break;
        i = j;
        n++;
    }
    char buf[256];
    if (i > sizeof(buf) - 4)
        i = sizeof(buf) - 4;
    memcpy(buf, s, i);
    memcpy(buf + i, "\xE2\x80\xA6", 4); // U+2026 and the terminator
    platform_write_str(buf);
    return n + 1;
}

void render_history(void)
{
    render_clear();

    if (app.hist_count == 0) {
        render_center_text(app.rows / 2, "no history yet", get_dim());
        render_center_hints(app.rows / 2 + 2, "[esc] back", get_dim());
        return;
    }

    move_to(2, 4);
    set_fg(get_fg());
    platform_write_str("history");

    int32_t visible = app.rows - 6;
    int32_t start = 0;
    if (app.hist_sel >= visible)
        start = app.hist_sel - visible + 1;

    for (int32_t i = 0; i < visible && start + i < app.hist_count; i++) {
        int32_t idx = start + i;
        HistoryEntry* entry = &app.history[idx];
        // A tap on the row (the width its highlight spans) opens that note, as enter would.
        hit_add(2, 4 + i, app.cols - 2, 1, HIT_ROW, '\r', idx);

        if (idx == app.hist_sel) {
            // The selected row sits one rung above the page (surface_container_highest)
            move_to(4 + i, 2);
            set_bg(get_row_select_bg());
            for (int32_t c = 2; c < app.cols; c++)
                platform_write_char(' ');
        }
        move_to(4 + i, 4);
        if (idx == app.hist_sel) {
            set_fg(get_accent());
            platform_write_str("> ");
        } else {
            set_fg(get_dim());
            platform_write_str("  ");
        }

        // Display title (or "Untitled") followed by date
        const char* title = entry->title ? entry->title : "Untitled";
        // Below ~70 columns the date loses its "at HH:MM", and the title is cut with an ellipsis
        // so the whole row fits on one line.
        const char* date = entry->date_str ? entry->date_str : "";
        char date_short[64];
        if (app.cols < 70) {
            const char* at = strstr(date, " at ");
            size_t dn = at ? (size_t)(at - date) : strlen(date);
            if (dn >= sizeof(date_short))
                dn = sizeof(date_short) - 1;
            memcpy(date_short, date, dn);
            date_short[dn] = '\0';
            date = date_short;
        }
        int32_t date_w = hist_cols(date, strlen(date));
        int32_t title_w = 30;
        int32_t avail = app.cols - 6 - 2 - date_w - 1;
        if (title_w > avail)
            title_w = avail;
        if (title_w < 4) {
            title_w = 4;
            date = "";
        }
        int32_t shown = hist_write_fit(title, title_w);
        for (int32_t c = shown; c < title_w; c++)
            platform_write_char(' ');
        platform_write_str("  ");
        set_fg(get_dim());
        platform_write_str(date);
        set_bg(get_bg());
    }

    // Key hints, least important first to go when the row would wrap
    static const char* const hints[] = { "[j/k] select", "[o] open", "[t] title", "[d] delete", "[e] finder", "[esc] back" };
    bool keep[6] = { true, true, true, true, true, true };
    int32_t hint_room = app.cols - 4 - 1;
    for (int32_t drop = 0; drop < 3; drop++) {
        int32_t total = 0;
        for (int32_t h = 0; h < 6; h++) {
            if (keep[h])
                total += (int32_t)strlen(hints[h]) + 3;
        }
        if (total - 3 <= hint_room)
            break;
        if (drop == 0)
            keep[4] = false; // [e] finder
        else if (drop == 1)
            keep[2] = false; // [t] title
        else
            keep[3] = false; // [d] delete
    }
    char hint_buf[128];
    size_t hn = 0;
    hint_buf[0] = '\0';
    for (int32_t h = 0; h < 6; h++) {
        if (!keep[h])
            continue;
        hn += (size_t)snprintf(hint_buf + hn, sizeof(hint_buf) - hn, "%s%s", hn ? "   " : "", hints[h]);
    }
    move_to(app.rows - 1, 4);
    set_fg(get_dim());
    hist_write_fit(hint_buf, hint_room);
    hit_add_hints(4, app.rows - 1, hint_buf, hint_room, true);
}

void render_finished(void)
{
    render_clear();
    int32_t cy = app.rows / 2;

    render_center_text(cy - 3, "done.", get_fg());
    render_center_text(cy - 1, "your writing is saved.", get_dim());

    int32_t words = count_words(&app.text);

    char stats[64];
    if (app.timer_start > 0) {
        int64_t now = DAWN_BACKEND(app)->clock(DAWN_CLOCK_SEC);
        int64_t elapsed_secs;
        if (app.timer_paused) {
            elapsed_secs = app.timer_mins * 60 - app.timer_paused_at;
        } else {
            elapsed_secs = now - app.timer_start;
        }
        int32_t elapsed_mins = (int32_t)(elapsed_secs / 60);
        if (elapsed_mins < 1)
            elapsed_mins = 1;
        snprintf(stats, sizeof(stats), "%d words in %d min", words, elapsed_mins);
    } else {
        snprintf(stats, sizeof(stats), "%d words", words);
    }
    render_center_text(cy + 1, stats, get_accent());

    render_center_hints(cy + 4, "[c] continue   [enter] new   [esc] menu", get_dim());
    render_center_hints(cy + 5, "[o] finder   [q] quit", get_dim());
#if HAS_LIBAI
    if (app.ai_ready) {
        render_center_hints(cy + 7, "[/] reflect with ai", get_dim());
    }
#endif
}

void render_fm_edit(void)
{
    int32_t box_width = 70;
    if (box_width > app.cols - 4)
        box_width = app.cols - 4;
    int32_t content_width = box_width - 4;

    // Calculate actual row count including wrapped lines for current string field
    int32_t field_rows = 0;
    for (int32_t i = 0; i < app.fm_edit.field_count; i++) {
        FmEditField* field = &app.fm_edit.fields[i];
        bool is_current = (i == app.fm_edit.current_field);

        if (is_current && field->kind == FM_FIELD_STRING && field->str.len > 0) {
            int32_t wrap_width = content_width - (int32_t)strlen(field->key) - 3;
            if (wrap_width < 10)
                wrap_width = 10;
            WrapResult wr;
            wrap_init(&wr);
            wrap_string(field->str.value, field->str.len, wrap_width, &wr);
            field_rows += wr.count > 0 ? wr.count : 1;
            wrap_free(&wr);
        } else {
            field_rows++;
        }
    }
    if (app.fm_edit.adding_field)
        field_rows++;
    if (field_rows < 1)
        field_rows = 1;

    int32_t box_height = 6 + field_rows;
    if (box_height > app.rows - 4)
        box_height = app.rows - 4;

    int32_t top, left;
    render_popup_box(box_width, box_height, &top, &left);

    int32_t content_left = left + 2;
    int32_t content_top = top + 1;

    set_bg(get_modal_bg());

    move_to(content_top, content_left);
    set_fg(get_dim());
    platform_write_str("Edit Frontmatter");

    int32_t row = content_top + 2;
    int32_t cursor_row = row;
    int32_t cursor_col = content_left;

    for (int32_t i = 0; i < app.fm_edit.field_count && row < top + box_height - 3; i++) {
        FmEditField* field = &app.fm_edit.fields[i];
        bool is_current = (i == app.fm_edit.current_field);
        int32_t field_row = row;

        move_to(row, content_left);
        FM_KEY_LABEL(field->key, is_current);

        int32_t value_start = content_left + (int32_t)strlen(field->key) + 2;
        int32_t max_val_width = content_width - (int32_t)strlen(field->key) - 3;

        switch (field->kind) {
        case FM_FIELD_BOOL:
            FM_BOOL_VALUE(field->boolean.value, is_current, cursor_row, cursor_col, value_start, row);
            break;

        case FM_FIELD_DATETIME: {
            char buf[8];
            FmFieldDatetime* dt = &field->datetime;
            int32_t p = dt->part;

            FM_DT_PART(buf, "%04d", dt->d.year, is_current, 0, p);
            FM_DT_SEP("-");
            FM_DT_PART(buf, "%02d", dt->d.mon, is_current, 1, p);
            FM_DT_SEP("-");
            FM_DT_PART(buf, "%02d", dt->d.mday, is_current, 2, p);

            if (dt->d.has_time) {
                FM_DT_SEP("T");
                FM_DT_PART(buf, "%02d", dt->d.hour, is_current, 3, p);
                FM_DT_SEP(":");
                FM_DT_PART(buf, "%02d", dt->d.min, is_current, 4, p);
                FM_DT_SEP(":");
                FM_DT_PART(buf, "%02d", dt->d.sec, is_current, 5, p);

                if (dt->d.ms > 0) {
                    FM_DT_SEP(".");
                    set_fg(get_fg());
                    snprintf(buf, sizeof(buf), "%03d", dt->d.ms);
                    platform_write_str(buf);
                }
                if (dt->d.has_tz)
                    FM_HINT(dt->d.tz);
            }

            if (is_current) {
                FM_HINT("  [</>:part +/-:adj]");
                int32_t offsets[] = { 0, 5, 8, 11, 14, 17 };
                FM_CURSOR_SET(cursor_row, cursor_col, row, value_start + offsets[p < 6 ? p : 0]);
            }
            break;
        }

        case FM_FIELD_LIST: {
            FmFieldList* lst = &field->list;
            int32_t col = value_start;
            platform_write_str("[");
            col++;
            for (int32_t j = 0; j < lst->count; j++) {
                if (j > 0) {
                    platform_write_str(", ");
                    col += 2;
                }
                bool item_sel = (is_current && j == lst->selected);
                if (item_sel)
                    set_fg(get_accent());
                platform_write_str("\"");
                col++;
                int32_t item_start_col = col;
                for (size_t k = 0; k < lst->item_lens[j]; k++) {
                    platform_write_char(lst->items[j][k]);
                    col++;
                }
                platform_write_str("\"");
                col++;
                if (item_sel) {
                    set_fg(get_fg());
                    FM_CURSOR_SET(cursor_row, cursor_col, row, item_start_col + (int32_t)lst->cursor);
                }
            }
            platform_write_str("]");
            if (is_current && lst->count == 0) {
                FM_CURSOR_SET(cursor_row, cursor_col, row, value_start + 1);
            }
            break;
        }

        case FM_FIELD_STRING:
        default: {
            int32_t wrap_width = max_val_width > 0 ? max_val_width : 10;
            int32_t max_lines = top + box_height - 3 - row;
            if (!is_current)
                max_lines = 1;

            WrapResult wr;
            wrap_init(&wr);
            wrap_string(field->str.value, field->str.len, wrap_width, &wr);

            int32_t first_value_row = row;
            for (int32_t ln = 0; ln < wr.count && ln < max_lines; ln++) {
                if (ln > 0) {
                    row++;
                    move_to(row, value_start);
                }
                WrapLine* wl = &wr.lines[ln];
                for (size_t j = wl->start; j < wl->end; j++) {
                    char c = field->str.value[j];
                    if (c != '\n')
                        platform_write_char(c);
                }
            }

            if (is_current) {
                size_t cur = field->str.cursor;
                int32_t cursor_line;
                FM_FIND_CURSOR_LINE(cur, &wr, field->str.len, cursor_line);
                int32_t line_row = first_value_row + cursor_line;
                if (line_row <= first_value_row + max_lines - 1) {
                    FM_CURSOR_SET(cursor_row, cursor_col, line_row,
                        value_start + (int32_t)(cur - wr.lines[cursor_line].start));
                }
            }

            if (!is_current && wr.count > 1)
                FM_HINT("...");

            wrap_free(&wr);
            break;
        }
        }
        // A tap on a field's rows makes it the current field, as tab would.
        hit_add(content_left, field_row, content_width, row - field_row + 1, HIT_ROW, 0, i);
        row++;
    }

    if (app.fm_edit.adding_field && row < top + box_height - 3) {
        move_to(row, content_left);
        set_fg(get_accent());
        platform_write_str("+ ");
        set_fg(get_fg());
        for (size_t i = 0; i < app.fm_edit.new_key_len; i++) {
            platform_write_char(app.fm_edit.new_key[i]);
        }
        platform_write_str(": ");
        FM_CURSOR_SET(cursor_row, cursor_col, row, content_left + 2 + (int32_t)app.fm_edit.new_key_len);
    }

    move_to(top + box_height - 2, content_left);
    const char* hint;
    if (app.fm_edit.adding_field) {
        hint = "enter:add  esc:cancel";
    } else if (app.fm_edit.current_field >= 0 && app.fm_edit.current_field < app.fm_edit.field_count) {
        FmEditField* f = &app.fm_edit.fields[app.fm_edit.current_field];
        switch (f->kind) {
        case FM_FIELD_BOOL:
            hint = "tab:next  space:toggle  enter:save  esc:cancel";
            break;
        case FM_FIELD_DATETIME:
            hint = "tab:next  </>:part  +/-:adj  enter:save  esc:cancel";
            break;
        case FM_FIELD_LIST:
            hint = "tab:next  ^N:add  ^D/bksp:del  ^←/→:item  enter:save  esc:cancel";
            break;
        default:
            hint = "tab:next  +:add  ^S:save  esc:cancel";
            break;
        }
    } else {
        hint = "tab:next  +:add field  enter:save  esc:cancel";
    }
    FM_HINT(hint);
    hit_add_hints(content_left, top + box_height - 2, hint, app.cols - content_left + 1, false);

    move_to(cursor_row, cursor_col);
    platform_set_cursor_visible(true);
}

static void render_block_edit_image(void)
{
    MODAL_BEGIN("Edit Image", 60, 13);

    MODAL_TEXT_FIELD(0, "Alt:    ", app.block_edit.image.alt,
        app.block_edit.image.alt_len, _modal_content_width - 10, 0);
    MODAL_TEXT_FIELD(1, "Title:  ", app.block_edit.image.title,
        app.block_edit.image.title_len, _modal_content_width - 10, 1);

    MODAL_SIZE_FIELD(3, "Width:  ", app.block_edit.image.width,
        app.block_edit.image.width_len, app.block_edit.image.width_pct, 2);
    MODAL_SIZE_FIELD(4, "Height: ", app.block_edit.image.height,
        app.block_edit.image.height_len, app.block_edit.image.height_pct, 3);

    MODAL_HELP(9, "tab:field  p:%/px  enter:save  esc:cancel");

    // A tap on a field's row makes it the current field; the hints are taps on their keys, but
    // not p, which alt and title would take as text.
    static const int32_t field_rows[] = { 0, 1, 3, 4 };
    for (int32_t f = 0; f < 4; f++)
        hit_add(_modal_content_left, _modal_field_row + field_rows[f], _modal_content_width, 1, HIT_ROW, 0, f);
    hit_add_hints(_modal_content_left, _modal_content_top + 9, "tab:field  p:%/px  enter:save  esc:cancel",
        _modal_content_width, false);

    MODAL_END();
}

void render_block_edit(void)
{
    switch (app.block_edit.type) {
    case BLOCK_IMAGE:
        render_block_edit_image();
        break;
    // Future: case BLOCK_CODE: render_block_edit_code(); break;
    default:
        break;
    }
}

void render_toc(void)
{
    TocState* toc = (TocState*)app.toc_state;
    if (!toc)
        return;

    // Calculate dimensions
    int32_t width = app.cols > 80 ? 70 : app.cols - 6;
    int32_t max_height = app.rows - 6;
    int32_t list_height = max_height - 7; // Space for header, filter, footer
    if (list_height < 3)
        list_height = 3;
    int32_t height = list_height + 7;

    int32_t top, left;
    render_popup_box(width, height, &top, &left);

    int32_t content_left = left + 3;
    int32_t content_right = left + width - 3;
    int32_t content_width = content_right - content_left;

    set_bg(get_modal_bg());

    // Title
    move_to(top + 2, left + width / 2 - 8);
    set_fg(get_fg());
    platform_set_bold(true);
    platform_write_str("TABLE OF CONTENTS");
    platform_reset_attrs();
    set_bg(get_modal_bg());

    // Filter input
    int32_t filter_row = top + 4;
    move_to(filter_row, content_left);
    set_fg(get_dim());
    platform_write_str("filter: ");
    set_fg(get_accent());
    for (int32_t i = 0; i < toc->filter_len && i < content_width - 10; i++) {
        platform_write_char(toc->filter[i]);
    }
    // Cursor indicator
    set_fg(get_fg());
    platform_write_char('_');

    // Results count
    char count_str[32];
    snprintf(count_str, sizeof(count_str), "%d/%d", toc->filtered_count, toc->count);
    move_to(filter_row, content_right - (int32_t)strlen(count_str));
    set_fg(get_dim());
    platform_write_str(count_str);

    // Separator
    move_to(top + 5, content_left);
    set_fg(get_border());
    for (int32_t i = 0; i < content_width; i++)
        platform_write_str("─");

    // TOC entries
    int32_t list_start = top + 6;
    int32_t visible = list_height;

    // Adjust scroll to keep selection visible
    if (toc->selected < toc->scroll)
        toc->scroll = toc->selected;
    if (toc->selected >= toc->scroll + visible)
        toc->scroll = toc->selected - visible + 1;

    for (int32_t i = 0; i < visible; i++) {
        int32_t idx = toc->scroll + i;
        if (idx >= toc->filtered_count)
            break;

        int32_t entry_idx = toc->filtered[idx];
        TocEntry* entry = &toc->entries[entry_idx];
        // A tap on the row (the width its highlight spans) jumps to that heading, as enter would.
        hit_add(left + 1, list_start + i, width - 2, 1, HIT_ROW, '\r', idx);

        // The selected row sits one rung above the modal (surface_container_highest)
        if (idx == toc->selected) {
            move_to(list_start + i, left + 1);
            set_bg(get_row_select_bg());
            for (int32_t j = 0; j < width - 2; j++)
                platform_write_char(' ');
        }
        move_to(list_start + i, content_left);

        // Selection indicator
        if (idx == toc->selected) {
            set_fg(get_accent());
            platform_write_str("▸ ");
        } else {
            platform_write_str("  ");
        }

        // Indentation based on hierarchy depth
        int32_t indent = entry->depth * 2;
        for (int32_t j = 0; j < indent && j < 12; j++)
            platform_write_char(' ');

        // Header text
        set_fg(idx == toc->selected ? get_fg() : get_dim());
        if (idx == toc->selected)
            platform_set_bold(true);

        // Truncate if needed
        int32_t max_text = content_width - 4 - indent;
        int32_t text_len = entry->text_len;
        if (text_len > max_text)
            text_len = max_text;

        for (int32_t j = 0; j < text_len; j++) {
            platform_write_char(entry->text[j]);
        }
        if (entry->text_len > max_text) {
            set_fg(get_dim());
            platform_write_str("...");
        }

        platform_reset_attrs();
        set_bg(get_modal_bg());
    }

    // Scroll indicators, each a tap that pages that way (two columns, room for a finger)
    if (toc->scroll > 0) {
        move_to(list_start, content_right);
        set_fg(get_dim());
        platform_write_str("↑");
        hit_add(content_right - 1, list_start, 2, 1, HIT_KEY, DAWN_KEY_PGUP, 0);
    }
    if (toc->scroll + visible < toc->filtered_count) {
        move_to(list_start + visible - 1, content_right);
        set_fg(get_dim());
        platform_write_str("↓");
        hit_add(content_right - 1, list_start + visible - 1, 2, 1, HIT_KEY, DAWN_KEY_PGDN, 0);
    }

    // Footer
    move_to(top + height - 2, content_left);
    set_fg(get_dim());
    platform_write_str("↑↓:nav  enter:jump  esc:close");
    hit_add_hints(content_left, top + height - 2, "↑↓:nav  enter:jump  esc:close", content_width, true);

    // Position cursor at filter
    move_to(filter_row, content_left + 8 + toc->filter_len);
    platform_set_cursor_visible(true);
}

//! The full menu's title, with the key that opens it without a finger
#define MENU_TITLE "commands"
#define MENU_TITLE_KEY "  ^]"

//! The fewest lines a menu shows before it scrolls, however little room is left
#define MENU_MIN_LINES 3

//! Draw the open menu on the popup's surface at scale s, with no border: lines of s screen rows
//! from top, cols scaled columns of text with a cell of padding either side from left, at most
//! rows screen rows tall. The title first when title is set; headings in the accent; the selected
//! line on the row highlight. When the lines do not all fit, a first and a last line say "↑ more"
//! and "↓ more", each a tap that pages that way. Sets m's scroll and page.
static void render_menu_surface(MenuState* m, int32_t s, int32_t top, int32_t left, int32_t cols, int32_t rows, bool title)
{
    int32_t width = cols * s + 2;
    int32_t list = rows / s - (title ? 1 : 0);
    bool scrolls = list < m->count;
    if (scrolls)
        list -= 2;
    if (list < MENU_MIN_LINES)
        list = m->count < MENU_MIN_LINES ? m->count : MENU_MIN_LINES;
    if (list < 1)
        list = 1;
    menu_scroll_to(m, list);
    m->page = list;
    int32_t shown = m->count - m->scroll < list ? m->count - m->scroll : list;
    int32_t height = ((title ? 1 : 0) + (scrolls ? 2 : 0) + shown) * s;

    DawnColor modal = get_modal_bg();
    image_mask_region(left, top, width, height, modal);
    set_bg(modal);
    for (int32_t r = 0; r < height; r++) {
        move_to(top + r, left);
        for (int32_t c = 0; c < width; c++)
            platform_write_char(' ');
    }
    // A tap inside on none of its lines does nothing; outside it closes the menu.
    hit_add(left, top, width, height, HIT_INERT, 0, 0);

    int32_t text = left + 1;
    int32_t row = top;
    if (title) {
        move_to(row, text);
        set_fg(get_fg());
        print_scaled_str(MENU_TITLE, strlen(MENU_TITLE), s);
        set_fg(get_dim());
        print_scaled_str(MENU_TITLE_KEY, strlen(MENU_TITLE_KEY), s);
        row += s;
    }
    if (scrolls) {
        move_to(row, text);
        set_fg(m->scroll > 0 ? get_dim() : get_border());
        print_scaled_str("↑ more", strlen("↑ more"), s);
        if (m->scroll > 0)
            hit_add(left, row, width, s, HIT_KEY, DAWN_KEY_PGUP, 0);
        row += s;
    }
    for (int32_t i = 0; i < shown; i++, row += s) {
        int32_t idx = m->scroll + i;
        const MenuLine* line = &m->lines[idx];
        if (!line->cmd) {
            const char* name = commands_group_name(line->group);
            move_to(row, text);
            set_fg(get_accent());
            print_scaled_str(name, strlen(name), s);
            continue;
        }
        bool sel = idx == m->selected;
        if (sel) {
            // The selected line sits one rung above the surface (surface_container_highest)
            set_bg(get_row_select_bg());
            for (int32_t r = 0; r < s; r++) {
                move_to(row + r, left);
                for (int32_t c = 0; c < width; c++)
                    platform_write_char(' ');
            }
        }
        if (line->enabled)
            hit_add(left, row, width, s, HIT_ROW, '\r', idx);
        const char* label = line->cmd->label;
        const char* sc = line->cmd->shortcut;
        int32_t sc_w = hist_cols(sc, strlen(sc));
        move_to(row, text);
        set_fg(!line->enabled ? get_border() : sel ? get_fg() : get_dim());
        if (s == 1)
            hist_write_fit(label, cols - (sc_w > 0 ? sc_w + 2 : 0));
        else
            print_scaled_str(label, strlen(label), s); // the width was chosen to hold it
        if (sc_w > 0) {
            move_to(row, text + (cols - sc_w) * s);
            set_fg(line->enabled ? get_dim() : get_border());
            print_scaled_str(sc, strlen(sc), s);
        }
        set_bg(modal);
    }
    if (scrolls) {
        bool more = m->scroll + list < m->count;
        move_to(row, text);
        set_fg(more ? get_dim() : get_border());
        print_scaled_str("↓ more", strlen("↓ more"), s);
        if (more)
            hit_add(left, row, width, s, HIT_KEY, DAWN_KEY_PGDN, 0);
    }
    platform_set_cursor_visible(false);
}

void render_menu(MenuState* m, const MenuPlace* p)
{
    int32_t scale = p && p->scale > 1 ? p->scale : 1;
    int32_t need = menu_list_cols(m);

    // The full menu at the chrome's scale where its widest line fits the screen less a column each
    // side, centred, a row clear above and below; the list scrolls inside.
    int32_t title_cols = (int32_t)(strlen(MENU_TITLE) + strlen(MENU_TITLE_KEY));
    int32_t cols = need > title_cols ? need : title_cols;
    int32_t s = scale;
    while (s > 1 && cols * s + 2 > app.cols - 2)
        s--;
    if (s > 1) {
        int32_t width = cols * s + 2;
        int32_t room = (app.rows - 2) - (app.rows - 2) % s;
        int32_t want = (m->count + 1) * s;
        int32_t height = want < room ? want : room;
        int32_t top = (app.rows - height) / 2 + 1;
        int32_t left = (app.cols - width) / 2 + 1;
        render_menu_surface(m, s, top, left, cols, height, true);
        return;
    }

    // At scale 1: as wide as help (44 columns, or the screen less a column each side on a phone),
    // and as tall as the list, never taller than the screen: with the keyboard up a phone pane is
    // about 12 rows, so the list scrolls inside the box. Rows: border, title, the list, hints,
    // border.
    int32_t width = app.cols - 2 < 44 ? app.cols - 2 : 44;
    if (width < 24)
        width = app.cols < 24 ? app.cols : 24;
    int32_t height = m->count + 4;
    if (height > app.rows)
        height = app.rows;
    int32_t visible = height - 4;
    if (visible < 1)
        visible = 1;
    menu_scroll_to(m, visible);
    m->page = visible;

    int32_t top, left;
    render_popup_box(width, height, &top, &left);
    // A tap inside the box on none of its rows does nothing; outside it closes the menu.
    hit_add(left, top, width, height, HIT_INERT, 0, 0);

    int32_t content_left = left + 2;
    int32_t content_right = left + width - 3; // last column text may use
    DawnColor modal = get_modal_bg();
    set_bg(modal);

    // Title, with the key that opens the menu without a finger
    move_to(top + 1, content_left);
    set_fg(get_fg());
    platform_write_str(MENU_TITLE);
    set_fg(get_dim());
    platform_write_str(MENU_TITLE_KEY);

    int32_t list_top = top + 2;
    for (int32_t i = 0; i < visible; i++) {
        int32_t idx = m->scroll + i;
        if (idx >= m->count)
            break;
        const MenuLine* line = &m->lines[idx];
        int32_t row = list_top + i;
        if (!line->cmd) {
            move_to(row, content_left);
            set_fg(get_accent());
            platform_write_str(commands_group_name(line->group));
            continue;
        }

        bool sel = idx == m->selected;
        if (line->enabled)
            hit_add(left + 1, row, width - 2, 1, HIT_ROW, '\r', idx);
        if (sel) {
            // The selected row sits one rung above the modal (surface_container_highest)
            move_to(row, left + 1);
            set_bg(get_row_select_bg());
            for (int32_t c = 0; c < width - 2; c++)
                platform_write_char(' ');
        }

        const char* sc = line->cmd->shortcut;
        int32_t sc_w = hist_cols(sc, strlen(sc));
        DawnColor text = !line->enabled ? get_border() : sel ? get_fg() : get_dim();
        move_to(row, content_left);
        if (sel) {
            set_fg(get_accent());
            platform_write_str("> ");
        } else {
            platform_write_str("  ");
        }
        set_fg(text);
        hist_write_fit(line->cmd->label, content_right - content_left - 2 - sc_w - 1);
        if (sc_w > 0) {
            move_to(row, content_right - sc_w + 1);
            set_fg(line->enabled ? get_dim() : get_border());
            platform_write_str(sc);
        }
        set_bg(modal);
    }

    // More above or below, each a tap that pages that way (the mark and a cell beside it)
    if (m->scroll > 0) {
        move_to(top + 1, content_right - 5);
        set_fg(get_dim());
        platform_write_str("↑ more");
        hit_add(content_right - 6, top + 1, 8, 1, HIT_KEY, DAWN_KEY_PGUP, 0);
    }
    if (m->scroll + visible < m->count) {
        move_to(top + height - 2, content_right - 5);
        set_fg(get_dim());
        platform_write_str("↓ more");
        hit_add(content_right - 6, top + height - 2, 8, 1, HIT_KEY, DAWN_KEY_PGDN, 0);
    }

    // Footer hints, tappable; left of the "↓ more" mark
    const char* footer = "enter:run  esc:close";
    move_to(top + height - 2, content_left);
    set_fg(get_dim());
    platform_write_str(footer);
    hit_add_hints(content_left, top + height - 2, footer, content_right - 7 - content_left, true);
    platform_set_cursor_visible(false);
}

//! Where render_search() drew its result rows last, for render_search_hit().
static struct {
    int32_t first_row, rows; //!< The list's first screen row and how many rows it filled
    int32_t left, width; //!< The box's columns
} search_geo;

//! One "by meaning" row's text after its indent, in max_cols columns: this note's piece is its
//! heading or first line; another note's is that note's title, then " · " and the same.
static void render_meaning_row(const SearchState* search, int32_t m, bool sel, int32_t max_cols)
{
    const char* text = search->meaning_text[m];
    if (m < search->meaning_here) {
        hist_write_fit(text, max_cols);
        return;
    }
    // The title keeps at least half the row when both don't fit, so the row says which note.
    const char* title = search->meaning[m].note_title;
    int32_t title_cols = hist_cols(title, strlen(title));
    // A piece that is the note's own H1 repeats the title: say it once, in the whole row.
    int32_t text_cols = strcasecmp(text, title) == 0 ? 0 : hist_cols(text, strlen(text));
    int32_t title_max = max_cols;
    if (text_cols > 0 && title_cols + 3 + text_cols > max_cols) {
        title_max = max_cols - 3 - text_cols;
        if (title_max < max_cols / 2)
            title_max = max_cols / 2;
    }
    int32_t used = hist_write_fit(title, title_max);
    int32_t sep = used > 0 ? 3 : 0;
    if (text_cols > 0 && max_cols - used - sep >= 2) {
        if (sep) {
            set_fg(get_dim());
            platform_write_str(" · ");
            set_fg(sel ? get_fg() : get_dim());
        }
        hist_write_fit(text, max_cols - used - sep);
    }
}

int32_t render_search_hit(int32_t row, int32_t col)
{
    const SearchState* search = (const SearchState*)app.search_state;
    if (!search || row < search_geo.first_row || row >= search_geo.first_row + search_geo.rows
        || col <= search_geo.left || col >= search_geo.left + search_geo.width - 1)
        return -1;
    int32_t idx = search->scroll + row - search_geo.first_row;
    if (idx < search->count)
        return idx;
    int32_t m = idx - search->count - 1; // past the "by meaning" label, which is no result
    return m >= 0 && m < search->meaning_count ? search->count + m : -1;
}

//! What the meaning index is doing, in a few words for the search box; "" while it is off, before
//! an embedder exists and on builds without it.
static void search_meaning_status(char* out, size_t cap)
{
    out[0] = '\0';
    if (!DAWN_EMBED_LIVE)
        return;
    EmbedStatus st;
    embed_status(&st);
    switch (st.phase) {
    case EMBED_PHASE_OFF:
    case EMBED_PHASE_DISCOVERING:
    case EMBED_PHASE_NO_EMBEDDER:
        break; // until a model exists (or while switched off) the box says nothing about meaning
    case EMBED_PHASE_INDEXING:
        snprintf(out, cap, "indexing %d/%d", st.notes_done, st.notes_total);
        break;
    case EMBED_PHASE_PAUSED:
        snprintf(out, cap, "paused (chat busy)");
        break;
    case EMBED_PHASE_WAITING: {
        int64_t left = st.retry_at_ms - DAWN_BACKEND(app)->clock(DAWN_CLOCK_MS);
        if (left > 0)
            snprintf(out, cap, "waiting for TAI · %llds", (long long)((left + 999) / 1000));
        else
            snprintf(out, cap, "waiting for TAI");
    } break;
    case EMBED_PHASE_IDLE:
        snprintf(out, cap, "index ready · %d note%s", st.notes_indexed, st.notes_indexed == 1 ? "" : "s");
        break;
    case EMBED_PHASE_FAILED:
        snprintf(out, cap, "failed: %s", st.error[0] ? st.error : "unknown error");
        break;
    }
}

void render_search(void)
{
    SearchState* search = (SearchState*)app.search_state;
    if (!search)
        return;

    // Calculate dimensions
    int32_t width = app.cols > 90 ? 80 : app.cols - 6;
    int32_t max_height = app.rows - 6;
    int32_t list_height = max_height - 8;
    if (list_height < 3)
        list_height = 3;
    int32_t height = list_height + 8;

    int32_t top, left;
    render_popup_box(width, height, &top, &left);

    int32_t content_left = left + 3;
    int32_t content_right = left + width - 3;
    int32_t content_width = content_right - content_left;

    set_bg(get_modal_bg());

    // Title
    move_to(top + 2, left + width / 2 - 3);
    set_fg(get_fg());
    platform_set_bold(true);
    platform_write_str("SEARCH");
    platform_reset_attrs();
    set_bg(get_modal_bg());

    // Results count: the exact matches and the "by meaning" rows
    char count_str[32];
    int32_t found = search->count + search->meaning_count;
    if (search->count >= SEARCH_MAX_RESULTS) {
        snprintf(count_str, sizeof(count_str), "%d+ matches", found);
    } else {
        snprintf(count_str, sizeof(count_str), "%d match%s", found, found == 1 ? "" : "es");
    }

    // Search input: a query too long for the room left of the count shows its end, where the
    // typing is
    int32_t search_row = top + 4;
    int32_t query_room = content_width - 6 - 1 - 1 - (int32_t)strlen(count_str); // "find: ", "_", a gap
    size_t from = 0, qlen = (size_t)search->query_len;
    while (from < qlen && hist_cols(search->query + from, qlen - from) > query_room) {
        from++;
        while (from < qlen && ((unsigned char)search->query[from] & 0xC0) == 0x80)
            from++; // not in the middle of a character
    }
    int32_t query_cols = hist_cols(search->query + from, qlen - from);
    move_to(search_row, content_left);
    set_fg(get_dim());
    platform_write_str("find: ");
    set_fg(get_accent());
    for (size_t i = from; i < qlen; i++)
        platform_write_char(search->query[i]);
    set_fg(get_fg());
    platform_write_char('_');

    move_to(search_row, content_right - (int32_t)strlen(count_str));
    set_fg(get_dim());
    platform_write_str(count_str);

    // Separator
    move_to(top + 5, content_left);
    set_fg(get_border());
    for (int32_t i = 0; i < content_width; i++)
        platform_write_str("─");

    // Search results with context, then the dim "by meaning" group under a label row. Scrolling
    // counts rows: the exact results, the label, the meaning rows. The list's last row goes to
    // the meaning index's status when there is one.
    char status[EMBED_ERROR_MAX + 32];
    search_meaning_status(status, sizeof(status));
    int32_t list_start = top + 6;
    int32_t visible = status[0] ? list_height - 1 : list_height;
    int32_t total_rows = search->count + (search->meaning_count > 0 ? search->meaning_count + 1 : 0);
    int32_t sel_row = search->selected < search->count ? search->selected : search->selected + 1;

    // Adjust scroll
    if (sel_row < search->scroll)
        search->scroll = sel_row;
    if (sel_row >= search->scroll + visible)
        search->scroll = sel_row - visible + 1;

    search_geo.first_row = list_start;
    search_geo.rows = total_rows - search->scroll < visible ? total_rows - search->scroll : visible;
    search_geo.left = left;
    search_geo.width = width;

    for (int32_t i = 0; i < visible; i++) {
        int32_t idx = search->scroll + i;
        if (idx >= total_rows)
            break;

        if (idx == search->count) {
            move_to(list_start + i, content_left + 2);
            set_fg(get_dim());
            platform_write_str("by meaning");
            continue;
        }
        if (idx > search->count) {
            int32_t m = idx - search->count - 1;
            bool sel = search->selected == search->count + m;
            if (sel) {
                move_to(list_start + i, left + 1);
                set_bg(get_row_select_bg());
                for (int32_t j = 0; j < width - 2; j++)
                    platform_write_char(' ');
            }
            move_to(list_start + i, content_left);
            set_fg(get_accent());
            platform_write_str(sel ? "▸       " : "        "); // under the line numbers
            set_fg(sel ? get_fg() : get_dim());
            render_meaning_row(search, m, sel, content_width - 10);
            set_bg(get_modal_bg());
            continue;
        }

        SearchResult* r = &search->results[idx];

        DawnColor row_bg = idx == search->selected ? get_row_select_bg() : get_modal_bg();
        if (idx == search->selected) {
            move_to(list_start + i, left + 1);
            set_bg(row_bg);
            for (int32_t j = 0; j < width - 2; j++)
                platform_write_char(' ');
        }
        move_to(list_start + i, content_left);

        // Selection indicator
        if (idx == search->selected) {
            set_fg(get_accent());
            platform_write_str("▸ ");
        } else {
            platform_write_str("  ");
        }

        // Line number
        char line_str[16];
        snprintf(line_str, sizeof(line_str), "%4d: ", r->line_num);
        set_fg(get_dim());
        platform_write_str(line_str);

        // Context with highlighted match
        int32_t max_ctx = content_width - 10;

        for (int32_t j = 0; j < r->context_len && j < max_ctx; j++) {
            // Highlight the match
            if (j >= r->match_start && j < r->match_start + r->match_len) {
                set_fg(get_accent());
                if (idx == search->selected)
                    platform_set_bold(true);
            } else {
                set_fg(idx == search->selected ? get_fg() : get_dim());
            }
            platform_write_char(r->context[j]);
            platform_reset_attrs();
            set_bg(row_bg);
        }

        if (r->context_len > max_ctx) {
            set_fg(get_dim());
            platform_write_str("...");
        }
        set_bg(get_modal_bg());
    }

    // Scroll indicators
    if (search->scroll > 0) {
        move_to(list_start, content_right);
        set_fg(get_dim());
        platform_write_str("↑");
    }
    if (search->scroll + visible < total_rows) {
        move_to(list_start + visible - 1, content_right);
        set_fg(get_dim());
        platform_write_str("↓");
    }

    // The meaning index's status, dim and right-aligned under the list
    if (status[0]) {
        int32_t cols = hist_cols(status, strlen(status));
        if (cols > content_width)
            cols = content_width;
        move_to(list_start + visible, content_right - cols);
        set_fg(get_dim());
        hist_write_fit(status, content_width);
    }

    // Footer
    move_to(top + height - 2, content_left);
    set_fg(get_dim());
    hist_write_fit("↑↓:nav  enter:jump  ^n/^p:next/prev  esc:close", content_width);

    // Position cursor at search
    move_to(search_row, content_left + 6 + query_cols);
    platform_set_cursor_visible(true);
}

//! Break text into lines of at most width code points, at spaces where a word fits and inside a
//! word that alone is wider. Returns the number of lines (at most max).
static int32_t conflict_wrap(const char* text, int32_t width, size_t* starts, size_t* lens, int32_t max)
{
    size_t len = strlen(text);
    size_t pos = 0;
    int32_t n = 0;
    while (pos < len && n < max) {
        while (pos < len && text[pos] == ' ')
            pos++;
        if (pos >= len)
            break;
        size_t i = pos, last_space = 0;
        int32_t cols = 0;
        while (i < len && cols < width) {
            if (text[i] == ' ')
                last_space = i;
            i++;
            while (i < len && ((unsigned char)text[i] & 0xC0) == 0x80)
                i++;
            cols++;
        }
        size_t end = i;
        if (i < len && text[i] != ' ' && last_space > pos)
            end = last_space; // break at the last space rather than inside a word
        starts[n] = pos;
        lens[n] = end - pos;
        n++;
        pos = end;
    }
    return n;
}

void render_conflict(void)
{
    static const char* const labels[CONFLICT_CHOICES] = {
        [CONFLICT_RELOAD] = "[r] Reload theirs",
        [CONFLICT_OVERWRITE] = "[o] Overwrite with mine",
        [CONFLICT_KEEP] = "[k] Keep editing",
    };
    static const char* const hints[CONFLICT_CHOICES] = {
        [CONFLICT_RELOAD] = "ctrl+z brings yours back",
        [CONFLICT_OVERWRITE] = "theirs goes to versions",
        [CONFLICT_KEEP] = "saving stays paused",
    };

    int32_t width = app.cols - 4 < 60 ? app.cols - 4 : 60;
    if (width < 24)
        width = app.cols < 24 ? app.cols : 24;
    int32_t text_w = width - 6;

    char body[3 * PATH_MAX];
    const char* name = app.session_path ? notepath_base(app.session_path, NULL) : "this note";
    if (app.conflict_path)
        snprintf(body, sizeof(body), "%s was changed outside dawn while you had unsaved edits here. Your text is safe in %s. Saving is paused until you choose.",
            name, notepath_base(app.conflict_path, NULL));
    else
        snprintf(body, sizeof(body), "%s was changed outside dawn while you had unsaved edits here. A copy of your text could not be written, so keep the note open. Saving is paused until you choose.",
            name);

    // Rows: border, blank, title, blank, body, blank, the choices with a blank between (easier to
    // tap), blank, border. Short screens lose the blanks between choices, then body lines.
    enum { BODY_MAX = 16 };
    size_t starts[BODY_MAX], lens[BODY_MAX];
    int32_t body_n = conflict_wrap(body, text_w > 8 ? text_w : 8, starts, lens, BODY_MAX);
    int32_t gap = 1;
    int32_t fixed = 7 + CONFLICT_CHOICES + (CONFLICT_CHOICES - 1) * gap;
    if (fixed + body_n > app.rows) {
        gap = 0;
        fixed = 7 + CONFLICT_CHOICES;
    }
    if (fixed + body_n > app.rows)
        body_n = app.rows - fixed > 1 ? app.rows - fixed : 1;
    int32_t height = fixed + body_n;

    int32_t top, left;
    render_popup_box(width, height, &top, &left);
    set_bg(get_modal_bg());

    int32_t col = left + 3;
    int32_t row = top + 2;
    move_to(row, col);
    set_fg(get_fg());
    platform_set_bold(true);
    platform_write_str("CHANGED ELSEWHERE");
    platform_reset_attrs();
    set_bg(get_modal_bg());
    row += 2;

    set_fg(get_fg());
    for (int32_t i = 0; i < body_n; i++) {
        move_to(row++, col);
        DAWN_BACKEND(app)->write_str(body + starts[i], lens[i]);
    }
    row++;

    // Each choice owns its row across the dialog's width, so a tap anywhere on it is its key.
    static const int32_t keys[CONFLICT_CHOICES] = {
        [CONFLICT_RELOAD] = 'r',
        [CONFLICT_OVERWRITE] = 'o',
        [CONFLICT_KEEP] = 'k',
    };
    for (int32_t c = 0; c < CONFLICT_CHOICES; c++) {
        hit_add(left, row, width, 1, HIT_KEY, keys[c], 0);
        move_to(row, col);
        set_fg(get_accent());
        platform_write_str(labels[c]);
        int32_t used = (int32_t)strlen(labels[c]);
        int32_t hint_len = (int32_t)strlen(hints[c]);
        if (used + 3 + hint_len <= text_w) {
            set_fg(get_dim());
            platform_write_str(" · ");
            platform_write_str(hints[c]);
        }
        row += 1 + gap;
    }
}

// #endregion
