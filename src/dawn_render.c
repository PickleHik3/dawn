// dawn_render.c

#include "dawn_render.h"
#include "dawn_block.h"
#include "dawn_gap.h"
#include "dawn_image.h"
#include "dawn_modal.h"
#include "dawn_notepath.h"
#include "dawn_notice.h"
#include "dawn_search.h"
#include "dawn_session.h"
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

void render_center_text(int32_t row, const char* text, DawnColor fg)
{
    int32_t len = (int32_t)strlen(text);
    int32_t col = (app.cols - len + 2) / 2;
    if (col < 1)
        col = 1;
    move_to(row, col);
    set_fg(fg);
    platform_write_str(text);
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

#if HAS_LIBAI
    if (app.ai_ready && session_ai_configured()) {
        row += 2;
        render_center_text(row, "✦ ai ready", get_accent());
    }
#endif

    // Bottom status bar - like editor status bar
    move_to(bottom_row, content_left);

    // Left: timer setting
    set_fg(get_dim());
    if (app.timer_mins == 0) {
        platform_write_str("no timer");
    } else {
        char timer_str[16];
        snprintf(timer_str, sizeof(timer_str), "%d min", app.timer_mins);
        platform_write_str(timer_str);
    }

    // Right: theme
    const char* theme_str = app.theme == THEME_DARK ? "dark" : "light";
    int32_t theme_col = content_right - (int32_t)strlen(theme_str);
    move_to(bottom_row, theme_col);
    set_fg(get_dim());
    platform_write_str(theme_str);
}

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
    }

    render_center_text(app.rows - 2, "[j/k] select   [enter] confirm   [esc] back", get_dim());
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
    }

    render_center_text(app.rows - 2, "[j/k] select   [enter] confirm   [esc] back", get_dim());
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

//! MODE_HELP's second page: the activity list (notice_history()), newest first, with relative
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

    move_to(top + height - 2, left + (width - 26) / 2);
    set_fg(get_dim());
    platform_write_str("[<-] shortcuts   [esc] close");
}

void render_help(void)
{
    int32_t width = 44;
    int32_t height = 26;
    int32_t top, left;
    render_popup_box(width, height, &top, &left);

    if (app.help_page == 1) {
        render_help_activity(top, left, width, height);
        return;
    }

    int32_t col1 = left + 4;
    int32_t col2 = left + 20;

    set_bg(get_modal_bg());

    // Title
    move_to(top + 2, left + width / 2 - 9);
    set_fg(get_fg());
    platform_set_bold(true);
    platform_write_str("KEYBOARD SHORTCUTS");
    platform_reset_attrs();
    set_bg(get_modal_bg());

    int32_t cy = top + 4;
    move_to(cy++, col1);
    set_fg(get_accent());
    platform_set_bold(true);
    platform_write_str("NAVIGATION");
    platform_reset_attrs();
    set_bg(get_modal_bg());
    set_fg(get_dim());

    move_to(cy, col1);
    platform_write_str("arrows");
    move_to(cy++, col2);
    platform_write_str("move cursor");
    move_to(cy, col1);
    platform_write_str("opt+arrows");
    move_to(cy++, col2);
    platform_write_str("word jump");
    move_to(cy, col1);
    platform_write_str("pgup/pgdn");
    move_to(cy++, col2);
    platform_write_str("scroll page");
    move_to(cy, col1);
    platform_write_str("^L");
    move_to(cy++, col2);
    platform_write_str("table of contents");
    move_to(cy, col1);
    platform_write_str("^S");
    move_to(cy++, col2);
    platform_write_str("search document");

    cy++;
    move_to(cy++, col1);
    set_fg(get_accent());
    platform_set_bold(true);
    platform_write_str("EDITING");
    platform_reset_attrs();
    set_bg(get_modal_bg());
    set_fg(get_dim());

    move_to(cy, col1);
    platform_write_str("^C ^X ^V");
    move_to(cy++, col2);
    platform_write_str("copy/cut/paste");
    move_to(cy, col1);
    platform_write_str("^Z ^Y");
    move_to(cy++, col2);
    platform_write_str("undo/redo");
    move_to(cy, col1);
    platform_write_str("^W ^D");
    move_to(cy++, col2);
    platform_write_str("delete word/elem");
    move_to(cy, col1);
    platform_write_str("tab shift+tab");
    move_to(cy++, col2);
    platform_write_str("indent list");

    cy++;
    move_to(cy++, col1);
    set_fg(get_accent());
    platform_set_bold(true);
    platform_write_str("FEATURES");
    platform_reset_attrs();
    set_bg(get_modal_bg());
    set_fg(get_dim());

    move_to(cy, col1);
    platform_write_str("^F");
    move_to(cy++, col2);
    platform_write_str("focus mode");
    move_to(cy, col1);
    platform_write_str("^R");
    move_to(cy++, col2);
    platform_write_str("plain text mode");
    move_to(cy, col1);
    platform_write_str("^G ^E");
    move_to(cy++, col2);
    platform_write_str("edit title/image");
    move_to(cy, col1);
    platform_write_str("^P ^T");
    move_to(cy++, col2);
    platform_write_str("pause/timer");
#if HAS_LIBAI
    move_to(cy, col1);
    platform_write_str("^/");
    move_to(cy++, col2);
    platform_write_str("AI chat");
#endif

    // Footer
    move_to(top + height - 2, left + (width - 30) / 2);
    set_fg(get_dim());
    platform_write_str("[tab] activity   [esc] close");
}

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
        render_center_text(app.rows / 2 + 2, "[esc] back", get_dim());
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

    render_center_text(cy + 4, "[c] continue   [enter] new   [esc] menu", get_dim());
    render_center_text(cy + 5, "[o] finder   [q] quit", get_dim());
#if HAS_LIBAI
    if (app.ai_ready) {
        render_center_text(cy + 7, "[/] reflect with ai", get_dim());
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
    if (app.fm_edit.adding_field) {
        FM_HINT("enter:add  esc:cancel");
    } else if (app.fm_edit.current_field >= 0 && app.fm_edit.current_field < app.fm_edit.field_count) {
        FmEditField* f = &app.fm_edit.fields[app.fm_edit.current_field];
        switch (f->kind) {
        case FM_FIELD_BOOL:
            FM_HINT("tab:next  space:toggle  enter:save  esc:cancel");
            break;
        case FM_FIELD_DATETIME:
            FM_HINT("tab:next  </>:part  +/-:adj  enter:save  esc:cancel");
            break;
        case FM_FIELD_LIST:
            FM_HINT("tab:next  ^N:add  ^D/bksp:del  ^←/→:item  enter:save  esc:cancel");
            break;
        default:
            FM_HINT("tab:next  +:add  ^S:save  esc:cancel");
            break;
        }
    } else {
        FM_HINT("tab:next  +:add field  enter:save  esc:cancel");
    }

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

    // Scroll indicators
    if (toc->scroll > 0) {
        move_to(list_start, content_right);
        set_fg(get_dim());
        platform_write_str("↑");
    }
    if (toc->scroll + visible < toc->filtered_count) {
        move_to(list_start + visible - 1, content_right);
        set_fg(get_dim());
        platform_write_str("↓");
    }

    // Footer
    move_to(top + height - 2, content_left);
    set_fg(get_dim());
    platform_write_str("↑↓:nav  enter:jump  esc:close");

    // Position cursor at filter
    move_to(filter_row, content_left + 8 + toc->filter_len);
    platform_set_cursor_visible(true);
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
    int32_t text_cols = hist_cols(text, strlen(text));
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

    // Search input
    int32_t search_row = top + 4;
    move_to(search_row, content_left);
    set_fg(get_dim());
    platform_write_str("find: ");
    set_fg(get_accent());
    for (int32_t i = 0; i < search->query_len && i < content_width - 8; i++) {
        platform_write_char(search->query[i]);
    }
    set_fg(get_fg());
    platform_write_char('_');

    // Results count: the exact matches and the "by meaning" rows
    char count_str[32];
    int32_t found = search->count + search->meaning_count;
    if (search->count >= SEARCH_MAX_RESULTS) {
        snprintf(count_str, sizeof(count_str), "%d+ matches", found);
    } else {
        snprintf(count_str, sizeof(count_str), "%d match%s", found, found == 1 ? "" : "es");
    }
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
    move_to(search_row, content_left + 6 + search->query_len);
    platform_set_cursor_visible(true);
}
//! Where MODE_CONFLICT's choices were drawn last, for render_conflict_hit().
static struct {
    bool shown;
    int32_t left, width; //!< The dialog's columns
    int32_t rows[CONFLICT_CHOICES]; //!< Each choice's row
} conflict_geo;

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

    conflict_geo.shown = true;
    conflict_geo.left = left;
    conflict_geo.width = width;
    for (int32_t c = 0; c < CONFLICT_CHOICES; c++) {
        conflict_geo.rows[c] = row;
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

int32_t render_conflict_hit(int32_t row, int32_t col)
{
    if (!conflict_geo.shown || col < conflict_geo.left || col >= conflict_geo.left + conflict_geo.width)
        return -1;
    for (int32_t c = 0; c < CONFLICT_CHOICES; c++)
        if (row == conflict_geo.rows[c])
            return c;
    return -1;
}

// #endregion
