// dawn_status.c - The status panel: what the model and the meaning index are doing, bottom right.
//
// The writer sees the model's background work only here: TAI loading a model, the warm session
// reading the note, a reply coming while the chat is closed, a live title being asked for, the
// chat being compacted, read-aloud, the mic, and the meaning index embedding notes (dawn_embed).
// Each is one row: an icon (Nerd Font only), a few words, and at the right end a determinate bar
// (indexing: notes done of notes in this pass), a sweeping bar (loading: TAI gives no fraction,
// so the row also counts the seconds) or a spinner. The most relevant rows come first: the model
// loading or reading the note (it blocks everything else), an answer, quiet model work, the voice
// helpers, then indexing. The welcome screen has room for up to four rows, a longer bar and the
// note being indexed; inside a note (not in focus mode, and never over a dialog: render() decides)
// the panel is one or two rows above the status line, or one row above the chat when it is a
// bottom sheet, and keeps off the cursor's row, hiding for a frame when it cannot. While the chat is open the model's own rows stay out: the chat says the same.
// Nothing is shown while nothing runs: an index that is current, off, failed (a notice says so),
// waiting for TAI or without an embedder shows nothing.
//
// Motion: the panel slides in from the right edge over SLIDE_MS when something starts, and once
// everything has ended it lingers LINGER_MS with its last rows dimmed, then slides out. The
// frame loop renders about every 7 ms (main_term.c waits at most that long for input), so the
// slide needs no wake-ups of its own. With DAWN_REDUCED_MOTION (voice_reduced_motion()) it just
// appears and disappears, the spinner is a still ellipsis and the sweeping bar is left out.
//
// TAI's runtime state is read here at most every 10 s, like the session's idle poll (each read
// may start a GET /v1/ai/runtime), except during the first 30 s after launch and while a model is
// loading or waking, when every frame reads the cached answer and TAI is asked every 3 s. Nothing
// is read while no endpoint is configured (session_ai_configured()).

#include "dawn_status.h"
#include "dawn_dictate.h"
#include "dawn_embed.h"
#include "dawn_image.h"
#include "dawn_session.h"
#include "dawn_speak.h"
#include "dawn_theme.h"
#include "dawn_title.h"
#include "dawn_voice.h"
#include <stdio.h>
#include <string.h>

#define SLIDE_MS 200 //!< How long the panel takes to slide in or out
#define LINGER_MS 1500 //!< How long it stays once everything it shows has ended
#define LINES_MAX 4 //!< Rows the panel can hold (the welcome screen's most)
#define CELLS_MAX 64 //!< Widest the panel can be
#define BAR_NOTE 8 //!< Bar cells inside a note
#define BAR_WELCOME 12 //!< Bar cells on the welcome screen
#define RUNTIME_IDLE_MS 10000 //!< How often TAI's runtime state is read when nothing is loading
#define RUNTIME_EAGER_MS 30000 //!< After launch, how long it is read every frame

// #region Rows

typedef enum { ICON_MODEL, ICON_CHAT, ICON_TITLE, ICON_QUIET, ICON_SPEAK, ICON_LISTEN, ICON_INDEX } StatusIcon;

typedef enum { BAR_NONE, BAR_FRACTION, BAR_SWEEP } StatusBar;

//! One thing going on, as one row of the panel.
typedef struct {
    StatusIcon icon;
    StatusBar bar;
    float frac; //!< For BAR_FRACTION, 0..1
    char text[96]; //!< A few words; cut with an ellipsis when the panel is narrow
    char detail[EMBED_TITLE_MAX]; //!< Dim words after the text, dropped first when there is no room ("" none)
} StatusLine;

//! A Nerd Font (Font Awesome range) icon for each kind of row.
static const char* icon_glyph(StatusIcon icon)
{
    switch (icon) {
    case ICON_MODEL:
        return "\xEF\x8B\x9B"; // U+F2DB microchip
    case ICON_CHAT:
        return "\xEF\x82\x86"; // U+F086 comments
    case ICON_TITLE:
        return "\xEF\x81\x80"; // U+F040 pencil
    case ICON_QUIET:
        return "\xEF\x80\x93"; // U+F013 cog
    case ICON_SPEAK:
        return "\xEF\x80\xA8"; // U+F028 volume-up
    case ICON_LISTEN:
        return "\xEF\x84\xB0"; // U+F130 microphone
    case ICON_INDEX:
        return "\xEF\x87\x80"; // U+F1C0 database
    }
    return " ";
}

static int32_t add_line(StatusLine* out, int32_t n, StatusIcon icon, StatusBar bar, float frac, const char* text,
    const char* detail)
{
    if (n >= LINES_MAX)
        return n;
    StatusLine* l = &out[n];
    l->icon = icon;
    l->bar = bar;
    l->frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
    snprintf(l->text, sizeof(l->text), "%s", text);
    snprintf(l->detail, sizeof(l->detail), "%s", detail ? detail : "");
    return n + 1;
}

// #endregion

// #region State

typedef enum { SLIDE_HIDDEN, SLIDE_IN, SLIDE_SHOWN, SLIDE_OUT } SlideState;

static struct {
    // Given by the screen this frame
    bool area;
    int32_t top, bottom, right, max_cols;
    bool welcome;
    bool cursor;
    int32_t cursor_row, cursor_col;

    // What the panel holds: the latest rows while something runs, the last ones while lingering
    StatusLine lines[LINES_MAX];
    int32_t count;
    int32_t width; //!< Only grows while the panel is out, so ticking numbers don't shake its edge

    SlideState slide;
    int64_t slide_from; //!< When the slide in or out began (DAWN_CLOCK_MS)
    float progress; //!< How far out the panel is, 0..1
    int64_t last_active; //!< The last frame anything was going on
    bool visible; //!< Drawn last frame

    int64_t started; //!< The first frame (DAWN_CLOCK_MS)
#if HAS_LIBAI
    ai_runtime_info_t info; //!< TAI's runtime state as last read
    int64_t info_read; //!< When it was read (DAWN_CLOCK_MS), 0 = never
    int64_t loading_since; //!< When a load was first seen, for the seconds counter; 0 = none
    bool configured; //!< session_ai_configured(), as last looked at
    int64_t configured_read; //!< When that was (DAWN_CLOCK_MS), 0 = never
#endif
} g;

void status_area(int32_t top, int32_t bottom, int32_t right_col, int32_t max_cols, bool welcome)
{
    g.area = top >= 1 && bottom >= top && right_col >= 1 && max_cols > 0;
    g.top = top;
    g.bottom = bottom;
    g.right = right_col;
    g.max_cols = max_cols < CELLS_MAX ? max_cols : CELLS_MAX;
    g.welcome = welcome;
}

void status_cursor(int32_t row, int32_t col)
{
    g.cursor = true;
    g.cursor_row = row;
    g.cursor_col = col;
}

bool status_visible(void)
{
    return g.visible;
}

// #endregion

// #region What is going on

#if HAS_LIBAI
//! The model's rows: loading (or waking for a job), reading the note, answering, then quiet work.
static int32_t collect_model(StatusLine* out, int32_t n, int64_t now)
{
    if (!app.ai_ready)
        return n;

    int64_t waking = session_waking_since();
    bool eager = now - g.started < RUNTIME_EAGER_MS || g.loading_since || waking;
    if (!g.info_read || eager || now - g.info_read >= RUNTIME_IDLE_MS) {
        // Nothing is asked of an endpoint that is not configured (looked at every 10 s).
        if (!g.configured_read || now - g.configured_read >= RUNTIME_IDLE_MS) {
            g.configured = session_ai_configured();
            g.configured_read = now;
        }
        if (g.configured)
            ai_runtime_info(&g.info);
        else
            memset(&g.info, 0, sizeof(g.info));
        g.info_read = now;
    }

    // The chat says all of this itself while it is open.
    bool chat_shown = app.mode == MODE_WRITING && app.ai_open;
    bool loading = g.info.loading || waking > 0;
    if (!loading)
        g.loading_since = 0;
    else if (!g.loading_since)
        g.loading_since = now;

    if (!chat_shown) {
        if (loading) {
            const char* model = session_model_id();
            char text[96], secs[24] = "";
            if (model[0])
                snprintf(text, sizeof(text), "loading %s", model);
            else
                snprintf(text, sizeof(text), "loading the model");
            // Counted from the first frame that saw the load, on dawn's own clock.
            int64_t elapsed = (now - g.loading_since) / 1000;
            if (elapsed >= 1)
                snprintf(secs, sizeof(secs), "%llds", (long long)elapsed);
            n = add_line(out, n, ICON_MODEL, BAR_SWEEP, 0, text, secs);
        } else if (session_reading()) {
            n = add_line(out, n, ICON_MODEL, BAR_NONE, 0, "reading the note", NULL);
        }
        if (app.ai_thinking || strcmp(session_job_name(), "answering") == 0)
            n = add_line(out, n, ICON_CHAT, BAR_NONE, 0, "answering", NULL);
    }

    const char* job = session_job_name();
    if (title_busy())
        n = add_line(out, n, ICON_TITLE, BAR_NONE, 0, "naming the note", NULL);
    else if (strcmp(job, "summarising the chat") == 0 || strcmp(job, "rebuilding context") == 0)
        n = add_line(out, n, ICON_CHAT, BAR_NONE, 0, job, NULL);
    else if (strcmp(job, "quiet work") == 0)
        n = add_line(out, n, ICON_QUIET, BAR_NONE, 0, "working in the background", NULL);
    return n;
}
#endif

//! Everything going on now, most relevant first; 0 when nothing is.
static int32_t collect(StatusLine* out, int64_t now)
{
    int32_t n = 0;
#if HAS_LIBAI
    n = collect_model(out, n, now);
#else
    (void)now;
#endif
    if (speak_active())
        n = add_line(out, n, ICON_SPEAK, BAR_NONE, 0, "reading aloud", NULL);
    if (dictate_listening())
        n = add_line(out, n, ICON_LISTEN, BAR_NONE, 0, "listening", NULL);

    // Only real progress: an index that is current, off, failed, waiting or without an embedder
    // has nothing to show here (the help's meaning page says all of that).
    EmbedStatus st;
    embed_status(&st);
    if (st.phase == EMBED_PHASE_INDEXING && st.notes_total > 0) {
        char text[48];
        snprintf(text, sizeof(text), "indexing %d/%d", st.notes_done, st.notes_total);
        n = add_line(out, n, ICON_INDEX, BAR_FRACTION, (float)st.notes_done / (float)st.notes_total, text,
            st.current_title);
    }
    return n;
}

// #endregion

// #region Drawing

typedef enum {
    ROLE_CAP, //!< The panel's rounded left edge
    ROLE_PAD, //!< Blank panel
    ROLE_ICON,
    ROLE_TEXT,
    ROLE_DETAIL,
    ROLE_SPIN,
    ROLE_FILL, //!< A bar's filled part
    ROLE_TRACK, //!< A bar's empty part: a thin line, not a background (see draw())
} CellRole;

typedef struct {
    char glyph[5]; //!< One UTF-8 code point
    uint8_t role;
} Cell;

typedef struct {
    Cell cells[CELLS_MAX];
    int32_t n;
} CellRow;

static void put(CellRow* row, const char* glyph, CellRole role)
{
    if (row->n >= CELLS_MAX)
        return;
    Cell* c = &row->cells[row->n++];
    size_t len = strlen(glyph);
    if (len > sizeof(c->glyph) - 1)
        len = sizeof(c->glyph) - 1;
    memcpy(c->glyph, glyph, len);
    c->glyph[len] = '\0';
    c->role = (uint8_t)role;
}

//! Code points in s (one column each, as elsewhere in dawn).
static int32_t cols_of(const char* s)
{
    int32_t n = 0;
    for (; *s; s++)
        if (((unsigned char)*s & 0xC0) != 0x80)
            n++;
    return n;
}

//! Put s cut to max cells, ending in an ellipsis when it was cut. Returns the cells used.
static int32_t put_text(CellRow* row, const char* s, int32_t max, CellRole role)
{
    if (max <= 0)
        return 0;
    bool cut = cols_of(s) > max;
    int32_t keep = cut ? max - 1 : max;
    int32_t used = 0;
    while (*s && used < keep) {
        char g[5] = { 0 };
        size_t len = 1;
        while (s[len] && ((unsigned char)s[len] & 0xC0) == 0x80 && len < 4)
            len++;
        memcpy(g, s, len);
        put(row, g, role);
        s += len;
        used++;
    }
    if (cut) {
        put(row, "\xE2\x80\xA6", role);
        used++;
    }
    return used;
}

//! The bar's cells, or 0 when this row has none (a sweep under reduced motion).
static int32_t bar_cells(const StatusLine* l, bool still)
{
    if (l->bar == BAR_NONE || (l->bar == BAR_SWEEP && still))
        return 0;
    return g.welcome ? BAR_WELCOME : BAR_NOTE;
}

//! The cells after the text: a space and a bar or spinner, then the panel's right padding.
static int32_t tail_cells(const StatusLine* l, bool still)
{
    int32_t bar = bar_cells(l, still);
    return 1 + (bar > 0 ? bar : 1) + 1;
}

//! The width a row wants: cap, pad, icon and gap, text, detail, tail.
static int32_t natural_width(const StatusLine* l, bool still)
{
    int32_t w = 2 + (app.nerd_font ? 2 : 0) + cols_of(l->text) + tail_cells(l, still);
    if (l->detail[0] && g.welcome)
        w += 3 + cols_of(l->detail); // " · detail"
    else if (l->detail[0] && l->icon == ICON_MODEL)
        w += 1 + cols_of(l->detail); // the seconds
    return w;
}

static void put_bar(CellRow* row, const StatusLine* l, int32_t cells, bool live, int64_t now)
{
    // A sweep: three filled cells running back and forth.
    int32_t seg_at = -1;
    if (l->bar == BAR_SWEEP) {
        int32_t span = (app.nerd_font ? cells : cells - 2) - 3;
        int32_t step = span > 0 ? (int32_t)((now / 70) % (2 * span)) : 0;
        seg_at = step < span ? step : 2 * span - step;
    }

    if (!app.nerd_font) {
        // [####----]
        int32_t inner = cells - 2;
        int32_t filled = (int32_t)(l->frac * (float)inner + 0.5f);
        put(row, "[", ROLE_DETAIL);
        for (int32_t i = 0; i < inner; i++) {
            bool on = l->bar == BAR_SWEEP ? (live && i >= seg_at && i < seg_at + 3) : i < filled;
            put(row, on ? "#" : "-", on ? ROLE_FILL : ROLE_DETAIL);
        }
        put(row, "]", ROLE_DETAIL);
        return;
    }

    static const char* const eighths[] = {
        "\xE2\x96\x8F", "\xE2\x96\x8E", "\xE2\x96\x8D", "\xE2\x96\x8C", "\xE2\x96\x8B", "\xE2\x96\x8A", "\xE2\x96\x89",
    };
    int32_t total = (int32_t)(l->frac * (float)(cells * 8) + 0.5f);
    for (int32_t i = 0; i < cells; i++) {
        if (l->bar == BAR_SWEEP) {
            bool on = live && i >= seg_at && i < seg_at + 3;
            put(row, on ? "\xE2\x96\x88" : "\xE2\x94\x80", on ? ROLE_FILL : ROLE_TRACK);
            continue;
        }
        int32_t e = total - i * 8;
        if (e >= 8)
            put(row, "\xE2\x96\x88", ROLE_FILL);
        else if (e <= 0)
            put(row, "\xE2\x94\x80", ROLE_TRACK); // U+2500
        else
            put(row, eighths[e - 1], ROLE_FILL);
    }
}

//! Lay one row out in width cells. live: the work is still going (spinner turns, bar sweeps);
//! otherwise the row is the panel lingering and draws dim and still.
static void layout_row(CellRow* row, const StatusLine* l, int32_t width, bool live, bool still, int64_t now)
{
    row->n = 0;
    put(row, app.nerd_font ? "\xEE\x82\xB6" : "\xE2\x96\x90", ROLE_CAP); // U+E0B6 or a right half block
    put(row, " ", ROLE_PAD);
    if (app.nerd_font) {
        put(row, icon_glyph(l->icon), ROLE_ICON);
        put(row, " ", ROLE_PAD); // the icon spreads over this one
    }

    int32_t tail = tail_cells(l, still);
    int32_t room = width - row->n - tail;
    int32_t used = put_text(row, l->text, room, ROLE_TEXT);

    // The detail: the note being indexed (welcome screen) or the seconds a load has taken.
    bool want_detail = l->detail[0] && (g.welcome || l->icon == ICON_MODEL);
    if (want_detail) {
        const char* sep = g.welcome && l->icon != ICON_MODEL ? " \xC2\xB7 " : " ";
        int32_t sep_cols = cols_of(sep);
        if (room - used - sep_cols >= 3) {
            put_text(row, sep, sep_cols, ROLE_DETAIL);
            used += sep_cols + put_text(row, l->detail, room - used - sep_cols, ROLE_DETAIL);
        }
    }
    while (row->n < width - tail)
        put(row, " ", ROLE_PAD);

    put(row, " ", ROLE_PAD);
    int32_t bar = bar_cells(l, still);
    if (bar > 0) {
        put_bar(row, l, bar, live, now);
    } else if (!live) {
        put(row, " ", ROLE_PAD);
    } else if (still) {
        put(row, "\xE2\x80\xA6", ROLE_SPIN);
    } else if (app.nerd_font) {
        static const char* const frames[] = {
            "\xE2\xA0\x8B", "\xE2\xA0\x99", "\xE2\xA0\xB9", "\xE2\xA0\xB8", "\xE2\xA0\xBC",
            "\xE2\xA0\xB4", "\xE2\xA0\xA6", "\xE2\xA0\xA7", "\xE2\xA0\x87", "\xE2\xA0\x8F",
        };
        put(row, frames[(now / 80) % 10], ROLE_SPIN);
    } else {
        static const char* const frames[] = { "|", "/", "-", "\\" };
        put(row, frames[(now / 120) % 4], ROLE_SPIN);
    }
    put(row, " ", ROLE_PAD);
}

static void cell_colors(CellRole role, bool live, DawnColor* fg, DawnColor* bg)
{
    DawnColor panel = get_modal_bg();
    *fg = get_fg();
    *bg = panel;
    switch (role) {
    case ROLE_CAP:
        *fg = panel;
        *bg = get_bg();
        break;
    case ROLE_PAD:
    case ROLE_TEXT:
        *fg = live ? get_fg() : get_dim();
        break;
    case ROLE_DETAIL:
        *fg = get_dim();
        break;
    case ROLE_ICON:
    case ROLE_SPIN:
        *fg = live ? get_accent() : get_dim();
        break;
    case ROLE_FILL:
        *fg = live ? get_accent() : get_dim();
        break;
    case ROLE_TRACK:
        *fg = get_border();
        break;
    }
}

//! Draw the leftmost `shown` cells of each row, right-aligned at g.right: sliding out from the
//! edge, the panel's left side shows first.
static void draw(int32_t first, int32_t rows, int32_t shown, bool live, bool still, int64_t now)
{
    int32_t top = g.bottom - rows + 1;
    int32_t left = g.right - shown + 1;
    // The mask sits above cell backgrounds (kitty draws z=-1 over non-default ones), so every
    // cell under it reads as the panel's colour: the cap, half page and half panel, stays out of
    // it, and nothing inside relies on a background of its own (a bar's track is a line).
    if (shown > 1)
        image_mask_region(left + 1, top, shown - 1, rows, get_modal_bg());
    for (int32_t r = 0; r < rows; r++) {
        CellRow row;
        layout_row(&row, &g.lines[first + r], g.width, live, still, now);
        move_to(top + r, left);
        for (int32_t c = 0; c < shown && c < row.n; c++) {
            DawnColor fg, bg;
            cell_colors((CellRole)row.cells[c].role, live, &fg, &bg);
            set_bg(bg);
            set_fg(fg);
            out_str(row.cells[c].glyph);
        }
    }
    reset_attrs();
}

// #endregion

// #region Frame

//! Advance the slide. active: something is going on this frame.
static void slide_tick(bool active, bool still, int64_t now)
{
    switch (g.slide) {
    case SLIDE_HIDDEN:
        if (active) {
            g.slide = SLIDE_IN;
            g.slide_from = now;
        }
        break;
    case SLIDE_IN:
    case SLIDE_SHOWN:
        if (!active && now - g.last_active >= LINGER_MS) {
            g.slide = SLIDE_OUT;
            g.slide_from = now - (int64_t)((1.0f - g.progress) * SLIDE_MS); // from where it is
        }
        break;
    case SLIDE_OUT:
        if (active) {
            g.slide = SLIDE_IN;
            g.slide_from = now - (int64_t)(g.progress * SLIDE_MS);
        }
        break;
    }

    float t = still ? 1.0f : (float)(now - g.slide_from) / (float)SLIDE_MS;
    if (t > 1)
        t = 1;
    if (t < 0)
        t = 0;
    if (g.slide == SLIDE_IN) {
        g.progress = t;
        if (t >= 1)
            g.slide = SLIDE_SHOWN;
    } else if (g.slide == SLIDE_SHOWN) {
        g.progress = 1;
    } else if (g.slide == SLIDE_OUT) {
        g.progress = 1 - t;
        if (t >= 1) {
            g.slide = SLIDE_HIDDEN;
            g.progress = 0;
            g.count = 0;
            g.width = 0;
        }
    } else {
        g.progress = 0;
    }
}

void status_frame(bool show)
{
    int64_t now = DAWN_BACKEND(app)->clock(DAWN_CLOCK_MS);
    if (!g.started)
        g.started = now;
    bool still = voice_reduced_motion();

    StatusLine now_lines[LINES_MAX];
    int32_t n = collect(now_lines, now);
    bool active = n > 0;
    if (active) {
        memcpy(g.lines, now_lines, sizeof(StatusLine) * (size_t)n);
        g.count = n;
        g.last_active = now;
    }
    slide_tick(active, still, now);

    bool drew = false;
    if (show && g.area && g.count > 0 && g.progress > 0 && app.ctx.mode != DAWN_MODE_PRINT) {
        // Rows: inside a note one or two, on the welcome screen what the area holds (at most four).
        int32_t rows = g.bottom - g.top + 1;
        if (rows > g.count)
            rows = g.count;

        int32_t want = 0;
        for (int32_t i = 0; i < rows; i++) {
            int32_t w = natural_width(&g.lines[i], still);
            if (w > want)
                want = w;
        }
        if (want > g.width)
            g.width = want;
        if (g.width > g.max_cols)
            g.width = g.max_cols;

        // Off the cursor's row: drop rows from the top, or skip this frame when the cursor sits
        // on the bottom row itself.
        if (g.cursor && g.cursor_col > g.right - g.width && g.cursor_col <= g.right) {
            if (g.cursor_row == g.bottom)
                rows = 0;
            else if (g.cursor_row >= g.bottom - rows + 1 && g.cursor_row < g.bottom)
                rows = g.bottom - g.cursor_row;
        }

        // Ease out: quick to arrive, gentle to settle.
        float p = 1 - g.progress;
        int32_t shown = (int32_t)((1 - p * p * p) * (float)g.width + 0.5f);
        if (shown > g.width)
            shown = g.width;
        if (rows > 0 && shown > 0 && g.width >= 8) {
            draw(0, rows, shown, active, still, now);
            drew = true;
            if (g.cursor)
                move_to(g.cursor_row, g.cursor_col);
        }
    }
    g.visible = drew;
    g.area = false;
    g.cursor = false;
}

// #endregion
