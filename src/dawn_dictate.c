// dawn_dictate.c - Dictation marks, dawn side: phrases, their spans and glow, the shimmer.
//
// Protocol (dawn-dictation-marks.md v1.1): `listen`/`end` track the mic only; a `phrase` can come
// on its own, usually after `end` (today's launcher types the whole dictation once, at ✓); ids
// are unique per terminal session, so the phrase table lives as long as dawn does; `replace` can
// name any earlier id and is kept for a future streaming launcher.
//
// Spans are note byte ranges. Edits this module makes shift the other spans; edits the writer
// makes aren't observed, so a span is trusted only while the note still holds that phrase's exact
// bytes there (checked before a glow is drawn and before a replace), and a replace otherwise looks
// for the phrase's text elsewhere in the note, taking the occurrence nearest where it was.

#include "dawn_dictate.h"
#include "dawn_block.h"
#include "dawn_gap.h"
#include "dawn_input.h"
#include "dawn_nav.h"
#include "dawn_theme.h"
#include "dawn_utils.h"

//! dawn.c: push the note onto the undo stack (also declared in dawn_chat.h, under HAS_LIBAI).
void save_undo_state(void);

#define DICT_PHRASES 32 //!< Phrases remembered for replace and glow
#define DICT_TRACK_MAX 4096 //!< A longer phrase still lands and glows, but can't be replaced
#define DICT_GLOW_MS 1000 //!< primary -> ink
#define DICT_FADE_IN_MS 250 //!< A replaced wording first fades up from the page
#define DICT_LISTEN_TIMEOUT_MS 60000 //!< No mark for this long: the `end` was missed
#define DICT_SHIMMER_CELLS 8
#define DICT_SHIMMER_PERIOD_MS 2400
#define DICT_SHIMMER_DEPTH 0.22f //!< How far the band leans from the page toward dim text

typedef struct {
    uint32_t id; //!< 0 = empty slot
    size_t start;
    size_t len;
    int64_t landed_ms;
    bool replaced; //!< The wording was swapped in place (cross-fade instead of plain glow)
    bool glow; //!< Still worth checking for a glow
    char* text; //!< The phrase's bytes as inserted, or NULL when too long to track
} Phrase;

static struct {
    Phrase ph[DICT_PHRASES];
    int32_t next; //!< Ring slot the next phrase takes
    bool listening;
    int64_t last_mark_ms;
    int64_t now; //!< This frame's clock, from dictate_tick()
    bool any_glow; //!< Some phrase is inside its glow window this frame
} g_dict;

static int64_t clock_ms(void) { return DAWN_BACKEND(app)->clock(DAWN_CLOCK_MS); }

static bool glow_allowed(void) { return !app.focus_mode && !voice_reduced_motion(); }

// #region Spans

//! The note holds exactly p's bytes at p's span.
static bool span_intact(const Phrase* p)
{
    if (!p->text || p->len == 0)
        return false;
    size_t len = gap_len(&app.text);
    if (p->start > len || p->len > len - p->start)
        return false;
    for (size_t i = 0; i < p->len; i++)
        if (gap_at(&app.text, p->start + i) != p->text[i])
            return false;
    return true;
}

//! The occurrence of p's text in the note nearest p->start, or SIZE_MAX.
static size_t span_relocate(const Phrase* p)
{
    if (!p->text || p->len == 0)
        return SIZE_MAX;
    char* all = gap_to_str(&app.text);
    if (!all)
        return SIZE_MAX;
    size_t len = gap_len(&app.text);
    size_t best = SIZE_MAX;
    size_t best_dist = SIZE_MAX;
    for (size_t i = 0; p->len <= len && i <= len - p->len; i++) {
        const char* hit = memchr(all + i, p->text[0], len - p->len - i + 1);
        if (!hit)
            break;
        i = (size_t)(hit - all);
        if (memcmp(hit, p->text, p->len) == 0) {
            size_t dist = i > p->start ? i - p->start : p->start - i;
            if (dist < best_dist) {
                best = i;
                best_dist = dist;
            }
        }
    }
    free(all);
    return best;
}

//! This module removed `removed` bytes at pos and put `added` there: move the spans after it,
//! forget the ones it cut into. `except` is the phrase doing the edit.
static void spans_shift(size_t pos, size_t removed, size_t added, const Phrase* except)
{
    for (int32_t i = 0; i < DICT_PHRASES; i++) {
        Phrase* p = &g_dict.ph[i];
        if (!p->id || p == except)
            continue;
        if (p->start >= pos + removed) {
            p->start = p->start - removed + added;
        } else if (p->start + p->len > pos) {
            free(p->text); // cut into: it can no longer be replaced, or glow
            p->text = NULL;
            p->glow = false;
        }
    }
}

static Phrase* phrase_find(uint32_t id)
{
    for (int32_t i = 0; i < DICT_PHRASES; i++)
        if (g_dict.ph[i].id == id)
            return &g_dict.ph[i];
    return NULL;
}

//! Keep the phrase's bytes for later checks, or NULL when it is too long to bother.
static char* track_copy(const char* text, size_t len)
{
    if (len == 0 || len > DICT_TRACK_MAX)
        return NULL;
    char* copy = malloc(len);
    if (copy)
        memcpy(copy, text, len);
    return copy;
}

// #endregion

// #region Events

//! Make pasted bytes safe to insert: CRLF to LF, and no control bytes but newline and tab.
static size_t sanitize(char* text, size_t len)
{
    len = normalize_line_endings(text, len);
    size_t out = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x20 && c != '\n' && c != '\t')
            continue;
        if (c == 0x7f)
            continue;
        text[out++] = (char)c;
    }
    return out;
}

//! The chat's input line has focus: dictation goes there, as the keystrokes it replaced would
//! have, on one line.
static void insert_into_chat(const char* text, size_t len)
{
    if (app.ai_input_len + 1 >= MAX_AI_INPUT)
        return;
    if (len > MAX_AI_INPUT - app.ai_input_len - 1) {
        len = MAX_AI_INPUT - app.ai_input_len - 1;
        while (len > 0 && ((unsigned char)text[len] & 0xC0) == 0x80)
            len--; // don't split a character at the cut
    }
    if (app.ai_input_cursor > app.ai_input_len)
        app.ai_input_cursor = app.ai_input_len;
    memmove(app.ai_input + app.ai_input_cursor + len, app.ai_input + app.ai_input_cursor,
        app.ai_input_len - app.ai_input_cursor);
    for (size_t i = 0; i < len; i++)
        app.ai_input[app.ai_input_cursor + i] = (text[i] == '\n' || text[i] == '\t') ? ' ' : text[i];
    app.ai_input_len += len;
    app.ai_input_cursor += len;
    app.ai_input[app.ai_input_len] = '\0';
}

static void phrase_land(uint32_t id, char* text, size_t len)
{
    if (app.mode != MODE_WRITING || app.preview_mode || len == 0)
        return;
    if (app.ai_open && app.ai_focused) {
        insert_into_chat(text, len);
        return;
    }
    size_t doc_len = gap_len(&app.text);
    if (doc_len >= MAX_TEXT_SIZE)
        return;
    if (len > MAX_TEXT_SIZE - doc_len)
        len = MAX_TEXT_SIZE - doc_len;

    save_undo_state();
    if (has_selection()) { // typed text replaces a selection; so does dictation
        size_t s, e;
        get_selection(&s, &e);
        gap_delete(&app.text, s, e - s);
        spans_shift(s, e - s, 0, NULL);
        app.cursor = s;
    }
    app.selecting = false;
    if (app.cursor > gap_len(&app.text))
        app.cursor = gap_len(&app.text);

    size_t at = app.cursor;
    gap_insert_str(&app.text, at, text, len);
    spans_shift(at, 0, len, NULL);
    app.cursor = at + len;
    app.dirty = true;
    app.view_detached = false;
    if (app.block_cache)
        block_cache_invalidate((BlockCache*)app.block_cache);

    // An id seen again (a launcher restart within the session) takes over the old slot.
    Phrase* p = phrase_find(id);
    if (!p) {
        p = &g_dict.ph[g_dict.next];
        g_dict.next = (g_dict.next + 1) % DICT_PHRASES;
    }
    free(p->text);
    *p = (Phrase) {
        .id = id,
        .start = at,
        .len = len,
        .landed_ms = clock_ms(),
        .replaced = false,
        .glow = true,
        .text = track_copy(text, len),
    };
}

static void phrase_replace(uint32_t id, char* text, size_t len)
{
    Phrase* p = phrase_find(id);
    if (!p || !p->text || app.mode != MODE_WRITING || app.preview_mode || len == 0)
        return;
    if (!span_intact(p)) {
        size_t at = span_relocate(p);
        if (at == SIZE_MAX)
            return; // the writer changed it: their words win
        p->start = at;
    }
    size_t start = p->start, old_len = p->len;
    if (gap_len(&app.text) - old_len + len > MAX_TEXT_SIZE)
        return;

    // One undo step, the same shape as the chat's edits (dawn_chat.c apply_edit()): a snapshot
    // on both sides, so one Ctrl+Z lands exactly on the wording from before.
    save_undo_state();
    gap_delete(&app.text, start, old_len);
    gap_insert_str(&app.text, start, text, len);
    save_undo_state();
    spans_shift(start, old_len, len, p);
    if (app.cursor >= start + old_len)
        app.cursor = app.cursor - old_len + len;
    else if (app.cursor > start)
        app.cursor = start + len;
    app.selecting = false;
    app.dirty = true;
    if (app.block_cache)
        block_cache_invalidate((BlockCache*)app.block_cache);

    free(p->text);
    p->text = track_copy(text, len);
    p->len = len;
    p->landed_ms = clock_ms();
    p->replaced = true;
    p->glow = true;
}

void dictate_event(DawnDictEvent* ev)
{
    if (!ev)
        return;
    int64_t now = clock_ms();
    switch (ev->verb) {
    case DAWN_DICT_LISTEN:
        g_dict.listening = true;
        g_dict.last_mark_ms = now;
        break;
    case DAWN_DICT_END:
    case DAWN_DICT_CANCEL:
        g_dict.listening = false;
        break;
    case DAWN_DICT_PHRASE:
    case DAWN_DICT_REPLACE:
        g_dict.last_mark_ms = now;
        if (ev->text && ev->len > 0) {
            size_t len = sanitize(ev->text, ev->len);
            if (ev->verb == DAWN_DICT_PHRASE)
                phrase_land(ev->id, ev->text, len);
            else
                phrase_replace(ev->id, ev->text, len);
        }
        break;
    }
    free(ev->text);
    ev->text = NULL;
    ev->len = 0;
}

void dictate_drain(void)
{
    DawnDictEvent ev;
    while (input_take_dictation(&ev))
        dictate_event(&ev);
}

// #endregion

// #region Frame

bool dictate_tick(int64_t now_ms)
{
    g_dict.now = now_ms;
    if (g_dict.listening && now_ms - g_dict.last_mark_ms > DICT_LISTEN_TIMEOUT_MS)
        g_dict.listening = false;

    g_dict.any_glow = false;
    for (int32_t i = 0; i < DICT_PHRASES; i++) {
        Phrase* p = &g_dict.ph[i];
        if (!p->id || !p->glow)
            continue;
        int64_t window = DICT_GLOW_MS + (p->replaced ? DICT_FADE_IN_MS : 0);
        // A phrase too long to track has no bytes to check against; it glows on trust.
        if (now_ms - p->landed_ms >= window || (p->text && !span_intact(p))) {
            p->glow = false;
            continue;
        }
        g_dict.any_glow = true;
    }
    bool shimmer = g_dict.listening && glow_allowed() && app.mode == MODE_WRITING;
    return (g_dict.any_glow && glow_allowed()) || shimmer;
}

bool dictate_listening(void) { return g_dict.listening; }

//! Ease out: quick at first, settling gently.
static float ease_out(float t)
{
    if (t <= 0.0f)
        return 0.0f;
    if (t >= 1.0f)
        return 1.0f;
    float u = 1.0f - t;
    return 1.0f - u * u * u;
}

//! The shimmer's strength in cell k (0 = the cursor's) of the band, this frame.
static float shimmer_at(int32_t k)
{
    float phase = (float)(g_dict.now % DICT_SHIMMER_PERIOD_MS) / (float)DICT_SHIMMER_PERIOD_MS;
    float peak = phase * (float)(DICT_SHIMMER_CELLS + 4) - 2.0f;
    float d = (float)k - peak;
    if (d < 0.0f)
        d = -d;
    float s = 1.0f - d / 3.0f;
    if (s <= 0.0f)
        return 0.0f;
    float taper = 1.0f - 0.5f * (float)k / (float)DICT_SHIMMER_CELLS;
    return s * taper * DICT_SHIMMER_DEPTH;
}

static bool shimmer_on(void)
{
    return g_dict.listening && glow_allowed() && app.mode == MODE_WRITING && !(app.ai_open && app.ai_focused);
}

bool dictate_style_at(size_t pos, VoiceStyle* out)
{
    if (!glow_allowed())
        return false;

    if (g_dict.any_glow) {
        for (int32_t i = 0; i < DICT_PHRASES; i++) {
            const Phrase* p = &g_dict.ph[i];
            if (!p->id || !p->glow || pos < p->start || pos >= p->start + p->len)
                continue;
            int64_t age = g_dict.now - p->landed_ms;
            DawnColor ink = get_fg();
            DawnColor primary = get_accent();
            if (p->replaced && age < DICT_FADE_IN_MS) {
                out->fg = color_lerp(get_bg(), primary, ease_out((float)age / (float)DICT_FADE_IN_MS));
            } else {
                if (p->replaced)
                    age -= DICT_FADE_IN_MS;
                out->fg = color_lerp(primary, ink, ease_out((float)age / (float)DICT_GLOW_MS));
            }
            out->has_fg = true;
            out->has_bg = false;
            return true;
        }
    }

    // The shimmer over text that follows the cursor on its line (blank cells: the overlay)
    if (shimmer_on() && pos >= app.cursor && pos < app.cursor + DICT_SHIMMER_CELLS) {
        for (size_t q = app.cursor; q < pos; q++)
            if (gap_at(&app.text, q) == '\n')
                return false;
        float s = shimmer_at((int32_t)(pos - app.cursor));
        if (s <= 0.0f)
            return false;
        out->has_fg = false;
        out->has_bg = true;
        out->bg = color_lerp(get_bg(), get_dim(), s);
        return true;
    }
    return false;
}

void dictate_draw_overlay(int32_t row, int32_t col, int32_t cols_free)
{
    if (!shimmer_on() || cols_free <= 0 || row < 1 || col < 1)
        return;
    size_t len = gap_len(&app.text);
    if (app.cursor < len && gap_at(&app.text, app.cursor) != '\n')
        return; // text follows: dictate_style_at() tints it instead
    int32_t cells = cols_free < DICT_SHIMMER_CELLS ? cols_free : DICT_SHIMMER_CELLS;
    move_to(row, col);
    for (int32_t k = 0; k < cells; k++) {
        set_bg(color_lerp(get_bg(), get_dim(), shimmer_at(k)));
        out_char(' ');
    }
    set_bg(get_bg());
}

// #endregion
