// dawn_hit.c - hit regions: how a tap on a phone becomes the key it stands for.
//
// Every frame starts empty (hit_frame_begin() at the top of render()). Whatever draws something
// tappable registers a rectangle of cells for it while it draws: a welcome-screen action, a hint
// like "[esc] back", a list row, a dialog's choice, a scroll pill's track. A later region wins
// where two overlap, so an overlay drawn over the note registers after it and takes the cells it
// covers. The table therefore always describes the frame on screen, which is the one the finger
// saw when it tapped.
//
// handle_input() in dawn.c looks every press (DAWN_KEY_MOUSE_CLICK) up here before the mode's own
// switch. A HIT_KEY region turns the press into its key, which then runs through the mode's
// normal key handling, so a tap and the key it shows share one path and cannot drift apart.
// HIT_ROW picks a row of the screen's list first (the history, the TOC, an editor's fields) and
// then presses its key, Enter for lists that open what is picked. HIT_PILL starts a drag of a
// scroll pill, and HIT_INERT swallows the tap (a box with nothing to tap at that cell). A press
// that hits nothing reaches the mode unchanged: in the note that is the cursor, double tap and
// drag selection of dawn_touch, which is why nothing in the note's text registers a region.
//
// Pure: no backend calls and no state beyond its own table, so tests/test_hit.c links it alone.

#include "dawn_hit.h"
#include "dawn_backend.h"

#include <string.h>

typedef struct {
    int32_t col, row, w, h;
    HitTarget target;
} HitRegion;

static HitRegion regions[HIT_MAX];
static int32_t region_count = 0;

void hit_frame_begin(void)
{
    region_count = 0;
}

void hit_add(int32_t col, int32_t row, int32_t w, int32_t h, HitKind kind, int32_t key, int32_t arg)
{
    if (w < 1 || h < 1 || region_count >= HIT_MAX)
        return;
    regions[region_count++] = (HitRegion) {
        .col = col,
        .row = row,
        .w = w,
        .h = h,
        .target = { .kind = kind, .key = key, .arg = arg },
    };
}

bool hit_find(int32_t col, int32_t row, HitTarget* out)
{
    for (int32_t i = region_count - 1; i >= 0; i--) {
        const HitRegion* r = &regions[i];
        if (col >= r->col && col < r->col + r->w && row >= r->row && row < r->row + r->h) {
            if (out)
                *out = r->target;
            return true;
        }
    }
    return false;
}

int32_t hit_count(void)
{
    return region_count;
}

// #region Hint Lines

//! Display columns of n bytes of UTF-8, one per code point (hint lines hold no wide characters).
static int32_t hint_cols(const char* s, size_t n)
{
    int32_t cols = 0;
    for (size_t i = 0; i < n; i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80)
            cols++;
    return cols;
}

//! The key a hint's name stands for, or 0 when it names none (or several, like "j/k").
static int32_t hint_key(const char* s, size_t n, bool letters)
{
    static const struct {
        const char* name;
        int32_t key;
    } names[] = {
        { "enter", '\r' },
        { "esc", '\x1b' },
        { "tab", '\t' },
        { "space", ' ' },
        { "<-", DAWN_KEY_LEFT },
        { "->", DAWN_KEY_RIGHT },
        { "^S", 19 },
        { "^N", 14 },
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strlen(names[i].name) == n && memcmp(names[i].name, s, n) == 0)
            return names[i].key;
    if (n != 1)
        return 0;
    unsigned char c = (unsigned char)s[0];
    if (c <= ' ' || c > '~')
        return 0;
    if (!letters && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
        return 0;
    return c;
}

void hit_add_hints(int32_t col, int32_t row, const char* text, int32_t max_cols, bool letters)
{
    if (!text)
        return;
    size_t len = strlen(text);
    size_t i = 0;
    while (i < len) {
        while (i < len && text[i] == ' ')
            i++;
        if (i >= len)
            break;
        size_t start = i, end, spec_at, spec_len;
        if (text[i] == '[') {
            // "[key] label": the label runs to two spaces, to the next "[", or to the end.
            const char* close = memchr(text + i + 1, ']', len - i - 1);
            if (!close)
                break;
            spec_at = i + 1;
            spec_len = (size_t)(close - (text + spec_at));
            end = (size_t)(close - text) + 1;
            while (end < len && !(text[end] == ' ' && (end + 1 >= len || text[end + 1] == ' ' || text[end + 1] == '[')))
                end++;
        } else {
            // "key:label", hints apart by two spaces.
            end = i;
            while (end < len && !(text[end] == ' ' && end + 1 < len && text[end + 1] == ' '))
                end++;
            const char* colon = memchr(text + i, ':', end - i);
            if (!colon || colon == text + i) {
                i = end;
                continue;
            }
            spec_at = i;
            spec_len = (size_t)(colon - (text + i));
        }
        while (end > start && text[end - 1] == ' ')
            end--;

        int32_t key = hint_key(text + spec_at, spec_len, letters);
        if (key) {
            int32_t at = hint_cols(text, start);
            int32_t w = hint_cols(text + start, end - start);
            if (at + w > max_cols)
                w = max_cols - at;
            hit_add(col + at, row, w, 1, HIT_KEY, key, 0);
        }
        i = end > start ? end : start + 1;
    }
}

// #endregion
