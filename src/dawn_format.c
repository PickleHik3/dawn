// dawn_format.c - Word-style formatting commands on the Markdown in the gap buffer
//
// format_apply is what a ribbon button or a shortcut calls; format_state tells whether its button
// shows pressed. Both work on the gap buffer and a FormatCursor only, never on the app global or
// the backend, so tests/test_format.c links this file with dawn_gap.c alone.
//
// Inline styles (bold **, italic *, strike ~~, code `). Each line is scanned the way CommonMark
// reads it: code spans first, then runs of * and ~~ matched by the flanking and rule-of-three
// tests. A selection is cut into one piece per line, leaving out the line's block prefix (quote,
// heading, list marker, task box), the whitespace at its ends and any marker characters at its
// ends. When every piece already sits inside the style, the style comes off the pieces only: the
// span is closed before a piece and reopened after it, and a side left empty loses its marker.
// Otherwise the style goes on the pieces that lack it, after swallowing spans of the same style
// inside them; a piece first grows over any span that crosses its edges, so markers always nest,
// and emphasis on text inside a code span goes outside the backticks. The selection then covers
// the same text without its markers. Bold on italic gives ***text***, which CommonMark reads as
// both. Without a selection the word at (or touching) the cursor is the piece and the cursor stays
// on the same letter; on whitespace an empty pair is typed with the cursor between, a second press
// on that empty pair removes it, and inside a span the span is closed and reopened around the
// cursor. Spans never cross a line here.
//
// Line styles (H1-H3, bullet, numbered, task, quote) act on every line the selection touches (a
// selection ending at the very start of a line leaves that line out), skipping blank lines; a lone
// blank line under the cursor gets the prefix. When every line has the style it is removed,
// otherwise it is set on the lines that lack it. Headings and list markers replace each other; a
// task is a box on a list item, so removing the task keeps the item's marker and removing the
// marker takes the box with it. Numbered items continue the list right above and count per
// indentation. Quotes go outside everything, and blank lines between quoted lines get ">".
// Indentation is kept.
//
// Inserts (link, image, table, rule, code block) place a template and select the part to type
// over. Blocks go on lines of their own with a blank line before and after, never splitting a
// line: into the cursor's line when blank, above it when the cursor is at its start, below it
// otherwise. Code block on lines inside a fenced block removes the fences instead.
//
// Only '\n' ends a line; a '\r' before it counts as trailing whitespace.

#include "dawn_format.h"
#include "dawn_gap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// #region Characters and lines

//! Space or tab
static bool is_blank(char c) { return c == ' ' || c == '\t'; }

//! Whitespace inside a line ('\r' included)
static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

//! Whitespace for the flanking tests; '\0' (outside the buffer) counts as a line edge
static bool is_ws(char c) { return is_space(c) || c == '\n' || c == '\f' || c == '\v' || c == '\0'; }

//! ASCII punctuation as CommonMark defines it
static bool is_punct(char c)
{
    unsigned char u = (unsigned char)c;
    return (u >= '!' && u <= '/') || (u >= ':' && u <= '@') || (u >= '[' && u <= '`') || (u >= '{' && u <= '~');
}

//! Letters, digits, underscore and every byte of a multibyte character
static bool is_wordc(char c)
{
    unsigned char u = (unsigned char)c;
    return u >= 0x80 || (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || u == '_';
}

//! Characters that open or close an inline style
static bool is_marker(char c) { return c == '*' || c == '~' || c == '`'; }

static size_t line_start(const GapBuffer* g, size_t p)
{
    while (p > 0 && gap_at(g, p - 1) != '\n')
        p--;
    return p;
}

static size_t line_end(const GapBuffer* g, size_t p)
{
    size_t n = gap_len(g);
    while (p < n && gap_at(g, p) != '\n')
        p++;
    return p;
}

//! Cursor and selection clamped to the buffer, with the selection ordered
typedef struct {
    size_t a, b; //!< Selection start and end (both the cursor without a selection)
    size_t cur, anc; //!< Clamped cursor and anchor
    bool sel; //!< A non-empty selection
} Range;

static Range selection(const GapBuffer* g, const FormatCursor* c)
{
    size_t n = gap_len(g);
    Range r;
    r.cur = c->cursor < n ? c->cursor : n;
    r.anc = c->anchor < n ? c->anchor : n;
    r.sel = c->selecting && r.cur != r.anc;
    r.a = r.sel && r.anc < r.cur ? r.anc : r.cur;
    r.b = r.sel && r.anc > r.cur ? r.anc : r.cur;
    return r;
}

//! Start of the last line the selection touches; a selection ending at a line's very start
//! leaves that line out
static size_t last_line(const GapBuffer* g, Range r)
{
    size_t b = r.b;
    if (r.sel && b > r.a && gap_at(g, b - 1) == '\n')
        b--;
    return line_start(g, b);
}

//! Number of lines from the line starting at first to the one starting at last
static int32_t count_lines(const GapBuffer* g, size_t first, size_t last)
{
    int32_t n = 1;
    for (size_t ls = first; ls < last; ls = line_end(g, ls) + 1)
        n++;
    return n;
}

// #endregion

// #region Edits

#define TRACK_MAX 8

//! An edit session: replacements on the buffer that carry tracked positions along
typedef struct {
    GapBuffer* g;
    size_t* track[TRACK_MAX];
    int32_t ntrack;
    bool changed;
} Edit;

static void edit_init(Edit* e, GapBuffer* g)
{
    e->g = g;
    e->ntrack = 0;
    e->changed = false;
}

static void edit_track(Edit* e, size_t* p)
{
    if (e->ntrack < TRACK_MAX)
        e->track[e->ntrack++] = p;
}

//! Replace [pos, pos + del) with s[0, n). Tracked positions after the edit move with the text;
//! one at pos, or inside the removed bytes, lands after the new bytes when after is true and
//! before them otherwise.
static void edit_replace(Edit* e, size_t pos, size_t del, const char* s, size_t n, bool after)
{
    size_t len = gap_len(e->g);
    if (pos > len)
        pos = len;
    if (del > len - pos)
        del = len - pos;
    if (del == n) {
        bool same = true;
        for (size_t i = 0; i < n && same; i++)
            same = gap_at(e->g, pos + i) == s[i];
        if (same)
            return;
    }
    if (del > 0)
        gap_delete(e->g, pos, del);
    if (n > 0)
        gap_insert_str(e->g, pos, s, n);
    e->changed = true;
    for (int32_t i = 0; i < e->ntrack; i++) {
        size_t v = *e->track[i];
        if (v < pos)
            continue;
        if (del > 0 && v >= pos + del)
            *e->track[i] = v - del + n;
        else if (del == 0 && v > pos)
            *e->track[i] = v + n;
        else
            *e->track[i] = after ? pos + n : pos;
    }
}

// #endregion

// #region Line prefixes

typedef enum {
    LIST_NONE,
    LIST_BULLET,
    LIST_NUMBERED
} ListKind;

//! The block prefix of one line: [indent][> quotes][indent][heading or list marker][task box]
typedef struct {
    size_t ls, le; //!< Line start and end (at its '\n' or the buffer end)
    size_t quote_at; //!< After the leading indentation: where a "> " goes
    size_t mark_at; //!< After the quotes and the indentation inside them
    size_t block_end; //!< After the heading's #s or the list marker, and one space
    size_t box_end; //!< After the task box and one space (block_end without a box)
    int32_t quotes; //!< Number of '>' markers
    int32_t heading; //!< Heading level, 0 for none
    int32_t number; //!< Number of a numbered item
    ListKind list; //!< List marker kind
    char delim; //!< '.' or ')' after a number
    bool task; //!< A task box follows the list marker
    bool blank; //!< Nothing but whitespace on the line
} LineInfo;

static void line_info(const GapBuffer* g, size_t p, LineInfo* li)
{
    memset(li, 0, sizeof(*li));
    size_t ls = line_start(g, p), le = line_end(g, ls), i = ls;
    li->ls = ls;
    li->le = le;
    while (i < le && is_space(gap_at(g, i)))
        i++;
    li->blank = i == le;
    li->quote_at = i;
    while (i < le && gap_at(g, i) == '>') {
        li->quotes++;
        i++;
        size_t j = i;
        while (j < le && is_blank(gap_at(g, j)))
            j++;
        if (j < le && gap_at(g, j) == '>')
            i = j;
        else if (i < le && is_blank(gap_at(g, i)))
            i++;
    }
    while (i < le && is_blank(gap_at(g, i)))
        i++;
    li->mark_at = li->block_end = li->box_end = i;

    size_t j = i;
    while (j < le && gap_at(g, j) == '#' && j - i < 7)
        j++;
    if (j - i >= 1 && j - i <= 6 && (j == le || is_space(gap_at(g, j)))) {
        li->heading = (int32_t)(j - i);
        if (j < le && is_blank(gap_at(g, j)))
            j++;
        li->block_end = li->box_end = j;
        return;
    }

    char c = gap_at(g, i);
    if (i + 1 < le && (c == '-' || c == '*' || c == '+') && is_blank(gap_at(g, i + 1))) {
        li->list = LIST_BULLET;
        j = i + 2;
    } else {
        int32_t num = 0;
        j = i;
        while (j < le && j - i < 9 && gap_at(g, j) >= '0' && gap_at(g, j) <= '9') {
            num = num * 10 + (gap_at(g, j) - '0');
            j++;
        }
        char d = gap_at(g, j);
        if (j == i || j + 1 >= le || (d != '.' && d != ')') || !is_blank(gap_at(g, j + 1)))
            return;
        li->list = LIST_NUMBERED;
        li->number = num;
        li->delim = d;
        j += 2;
    }
    li->block_end = li->box_end = j;
    if (j + 2 < le && gap_at(g, j) == '[' && gap_at(g, j + 2) == ']') {
        char x = gap_at(g, j + 1);
        if ((x == ' ' || x == 'x' || x == 'X') && (j + 3 == le || is_space(gap_at(g, j + 3)))) {
            li->task = true;
            li->box_end = j + 3 < le && is_blank(gap_at(g, j + 3)) ? j + 4 : j + 3;
        }
    }
}

// #endregion

// #region Line styles

static int32_t heading_level(FormatOp op)
{
    switch (op) {
    case FORMAT_H1:
        return 1;
    case FORMAT_H2:
        return 2;
    case FORMAT_H3:
        return 3;
    default:
        return 0;
    }
}

static bool line_has(const LineInfo* li, FormatOp op)
{
    switch (op) {
    case FORMAT_H1:
    case FORMAT_H2:
    case FORMAT_H3:
        return li->heading == heading_level(op);
    case FORMAT_BULLET:
        return li->list == LIST_BULLET;
    case FORMAT_NUMBERED:
        return li->list == LIST_NUMBERED;
    case FORMAT_TASK:
        return li->task;
    case FORMAT_QUOTE:
        return li->quotes > 0;
    default:
        return false;
    }
}

#define NUM_LEVELS 16

//! Next number per indentation while numbering lines
typedef struct {
    size_t indent[NUM_LEVELS];
    int32_t next[NUM_LEVELS];
    char delim[NUM_LEVELS];
    int32_t n;
} Numbering;

//! The level for an item indented by indent bytes: deeper levels are closed, a new one starts at 1
static int32_t numbering_level(Numbering* nb, size_t indent)
{
    while (nb->n > 0 && nb->indent[nb->n - 1] > indent)
        nb->n--;
    if (nb->n > 0 && nb->indent[nb->n - 1] == indent)
        return nb->n - 1;
    int32_t k = nb->n < NUM_LEVELS ? nb->n++ : NUM_LEVELS - 1;
    nb->indent[k] = indent;
    nb->next[k] = 1;
    nb->delim[k] = '.';
    return k;
}

//! Set or remove op on one line
static void line_apply(Edit* e, const LineInfo* li, FormatOp op, bool remove, Numbering* nb)
{
    char buf[24];
    int32_t level = heading_level(op);
    if (level > 0) {
        if (remove) {
            edit_replace(e, li->mark_at, li->block_end - li->mark_at, "", 0, true);
        } else if (li->heading != level) {
            int32_t k = 0;
            while (k < level)
                buf[k++] = '#';
            buf[k++] = ' ';
            edit_replace(e, li->mark_at, li->box_end - li->mark_at, buf, (size_t)k, true);
        }
        return;
    }
    switch (op) {
    case FORMAT_BULLET:
        if (remove)
            edit_replace(e, li->mark_at, li->box_end - li->mark_at, "", 0, true);
        else if (li->list != LIST_BULLET)
            edit_replace(e, li->mark_at, li->block_end - li->mark_at, "- ", 2, true);
        break;
    case FORMAT_NUMBERED:
        if (remove) {
            edit_replace(e, li->mark_at, li->box_end - li->mark_at, "", 0, true);
        } else {
            int32_t k = numbering_level(nb, li->mark_at - li->ls);
            int w = snprintf(buf, sizeof(buf), "%d%c ", (int)nb->next[k]++, nb->delim[k]);
            if (w > 0 && (size_t)w < sizeof(buf))
                edit_replace(e, li->mark_at, li->block_end - li->mark_at, buf, (size_t)w, true);
        }
        break;
    case FORMAT_TASK:
        if (remove)
            edit_replace(e, li->block_end, li->box_end - li->block_end, "", 0, true);
        else if (li->task)
            break;
        else if (li->list != LIST_NONE)
            edit_replace(e, li->block_end, 0, "[ ] ", 4, true);
        else
            edit_replace(e, li->mark_at, li->block_end - li->mark_at, "- [ ] ", 6, true);
        break;
    case FORMAT_QUOTE:
        if (remove) {
            size_t n = li->quote_at + 1 < li->le && is_blank(gap_at(e->g, li->quote_at + 1)) ? 2 : 1;
            edit_replace(e, li->quote_at, n, "", 0, true);
        } else if (li->quotes == 0) {
            edit_replace(e, li->quote_at, 0, "> ", 2, true);
        }
        break;
    default:
        break;
    }
}

//! Which lines a line style acts on, and whether it comes off
typedef struct {
    size_t first; //!< Start of the first line
    int32_t lines; //!< Number of lines
    int32_t first_text, last_text; //!< Index of the first and last non-blank line
    bool single; //!< Only blank lines: the cursor's line alone gets the prefix
    bool remove; //!< Every non-blank line has the style
} LineTargets;

static LineTargets line_targets(const GapBuffer* g, FormatOp op, Range r)
{
    LineTargets t = { line_start(g, r.a), 0, -1, -1, false, true };
    size_t last = last_line(g, r);
    LineInfo li;
    for (size_t ls = t.first;;) {
        line_info(g, ls, &li);
        if (!li.blank) {
            if (t.first_text < 0)
                t.first_text = t.lines;
            t.last_text = t.lines;
            t.remove = t.remove && line_has(&li, op);
        }
        t.lines++;
        if (ls >= last || li.le >= gap_len(g))
            break;
        ls = li.le + 1;
    }
    if (t.first_text < 0) {
        t.first = line_start(g, r.cur);
        t.lines = 1;
        t.single = true;
        t.remove = false;
    }
    return t;
}

static bool apply_lines(GapBuffer* g, FormatOp op, FormatCursor* c)
{
    Range r = selection(g, c);
    LineTargets t = line_targets(g, op, r);
    Edit e;
    edit_init(&e, g);
    size_t cur = r.cur, anc = r.anc;
    edit_track(&e, &cur);
    edit_track(&e, &anc);

    Numbering nb = { .n = 0 };
    if (op == FORMAT_NUMBERED && t.first > 0) {
        LineInfo prev;
        line_info(g, t.first - 1, &prev);
        if (prev.list == LIST_NUMBERED) {
            nb.indent[0] = prev.mark_at - prev.ls;
            nb.next[0] = prev.number + 1;
            nb.delim[0] = prev.delim;
            nb.n = 1;
        }
    }

    size_t ls = t.first;
    LineInfo li;
    for (int32_t k = 0; k < t.lines; k++) {
        line_info(g, ls, &li);
        if (!li.blank || t.single)
            line_apply(&e, &li, op, t.remove, &nb);
        else if (op == FORMAT_QUOTE && !t.remove && k > t.first_text && k < t.last_text)
            edit_replace(&e, li.quote_at, 0, ">", 1, true);
        ls = line_end(g, ls) + 1;
    }
    c->cursor = cur;
    c->anchor = anc;
    return e.changed;
}

static bool state_lines(const GapBuffer* g, FormatOp op, Range r)
{
    LineTargets t = line_targets(g, op, r);
    return !t.single && t.remove;
}

// #endregion

// #region Inline spans

//! A matched inline style on one line: opener [os, oe), content [oe, cs), closer [cs, ce)
typedef struct {
    size_t os, oe, cs, ce;
    FormatOp kind;
} Span;

typedef struct {
    Span* v;
    int32_t n, cap;
} Spans;

//! A run of * or ~~ waiting to be matched
typedef struct {
    size_t pos, len, orig;
    char ch;
    bool open, close, dead;
} Delim;

static void spans_push(Spans* sp, Span s)
{
    if (sp->n == sp->cap) {
        int32_t cap = sp->cap ? sp->cap * 2 : 16;
        Span* v = realloc(sp->v, (size_t)cap * sizeof(Span));
        if (!v)
            return;
        sp->v = v;
        sp->cap = cap;
    }
    sp->v[sp->n++] = s;
}

static size_t run_len(const char* s, size_t n, size_t i, char c)
{
    size_t j = i;
    while (j < n && s[j] == c)
        j++;
    return j - i;
}

//! Match runs of the same character as CommonMark's emphasis pass does: each closer takes the
//! nearest usable opener, two characters when both runs have two, and the runs between are dropped
static void match_delims(Delim* d, int32_t nd, size_t base, Spans* sp)
{
    for (int32_t ci = 0; ci < nd; ci++) {
        Delim* cl = &d[ci];
        if (!cl->close)
            continue;
        while (cl->len > 0 && !cl->dead) {
            int32_t oi = ci - 1;
            for (; oi >= 0; oi--) {
                const Delim* o = &d[oi];
                if (o->dead || o->len == 0 || o->ch != cl->ch || !o->open)
                    continue;
                if (cl->ch == '~' && o->len != cl->len)
                    continue;
                if (cl->ch == '*' && (o->close || cl->open) && (o->orig + cl->orig) % 3 == 0
                    && !(o->orig % 3 == 0 && cl->orig % 3 == 0))
                    continue;
                break;
            }
            if (oi < 0)
                break;
            Delim* o = &d[oi];
            size_t use = cl->ch == '~' || (o->len >= 2 && cl->len >= 2) ? 2 : 1;
            FormatOp kind = FORMAT_ITALIC;
            if (cl->ch == '~')
                kind = FORMAT_STRIKE;
            else if (use == 2)
                kind = FORMAT_BOLD;
            spans_push(sp, (Span) { base + o->pos + o->len - use, base + o->pos + o->len, base + cl->pos, base + cl->pos + use, kind });
            o->len -= use;
            cl->pos += use;
            cl->len -= use;
            for (int32_t k = oi + 1; k < ci; k++)
                d[k].dead = true;
        }
    }
}

//! Every code span, bold, italic and strike span on the line [ls, le)
static void spans_scan(const GapBuffer* g, size_t ls, size_t le, Spans* sp)
{
    sp->n = 0;
    size_t n = le - ls;
    char* s = gap_substr(g, ls, le);
    Delim* d = malloc((n + 1) * sizeof(Delim));
    if (!s || !d) {
        free(s);
        free(d);
        return;
    }
    int32_t nd = 0;
    size_t i = 0;
    while (i < n) {
        char c = s[i];
        if (c == '\\' && i + 1 < n) {
            i += 2;
        } else if (c == '`') {
            size_t run = run_len(s, n, i, '`'), j = i + run;
            bool found = false;
            while (j < n && !found) {
                size_t r2 = s[j] == '`' ? run_len(s, n, j, '`') : 0;
                if (r2 == run) {
                    spans_push(sp, (Span) { ls + i, ls + i + run, ls + j, ls + j + run, FORMAT_CODE });
                    found = true;
                }
                j += r2 ? r2 : 1;
            }
            i = found ? j : i + run;
        } else if (c == '*' || c == '~') {
            size_t run = run_len(s, n, i, c);
            char prev = i > 0 ? s[i - 1] : ' ', next = i + run < n ? s[i + run] : ' ';
            bool lf = !is_ws(next) && (!is_punct(next) || is_ws(prev) || is_punct(prev));
            bool rf = !is_ws(prev) && (!is_punct(prev) || is_ws(next) || is_punct(next));
            if (c == '*' || run == 2)
                d[nd++] = (Delim) { i, run, run, c, lf, rf, false };
            i += run;
        } else {
            i++;
        }
    }
    match_delims(d, nd, ls, sp);
    free(s);
    free(d);
}

static const char* inline_marker(FormatOp op)
{
    switch (op) {
    case FORMAT_BOLD:
        return "**";
    case FORMAT_ITALIC:
        return "*";
    case FORMAT_STRIKE:
        return "~~";
    default:
        return "`";
    }
}

//! The innermost op span whose content holds [a, b)
static const Span* span_holding(const Spans* sp, FormatOp op, size_t a, size_t b)
{
    const Span* best = NULL;
    for (int32_t i = 0; i < sp->n; i++) {
        const Span* s = &sp->v[i];
        if (s->kind == op && s->oe <= a && b <= s->cs && (!best || s->cs - s->oe < best->cs - best->oe))
            best = s;
    }
    return best;
}

//! Narrow [*a, *b) on line li to the text a style goes around: past the block prefix, without
//! whitespace or marker characters at its ends. False when nothing is left.
static bool piece_trim(const GapBuffer* g, const LineInfo* li, size_t* a, size_t* b)
{
    size_t s = *a > li->box_end ? *a : li->box_end;
    size_t e = *b < li->le ? *b : li->le;
    while (s < e && is_ws(gap_at(g, s)))
        s++;
    while (e > s && is_ws(gap_at(g, e - 1)))
        e--;
    if (s >= e)
        return false;
    size_t s2 = s, e2 = e;
    while (s2 < e2 && (is_ws(gap_at(g, s2)) || is_marker(gap_at(g, s2))))
        s2++;
    while (e2 > s2 && (is_ws(gap_at(g, e2 - 1)) || is_marker(gap_at(g, e2 - 1))))
        e2--;
    *a = s2 < e2 ? s2 : s;
    *b = s2 < e2 ? e2 : e;
    return true;
}

//! Grow [*a, *b) over spans that cross its edges, and, for emphasis, over a code span around it
static void piece_extend(const Spans* sp, FormatOp op, size_t* a, size_t* b)
{
    for (bool grew = true; grew;) {
        grew = false;
        for (int32_t i = 0; i < sp->n; i++) {
            const Span* s = &sp->v[i];
            if (s->ce <= *a || s->os >= *b || (s->os >= *a && s->ce <= *b))
                continue;
            bool holds = s->oe <= *a && *b <= s->cs;
            if (holds && !(s->kind == FORMAT_CODE && op != FORMAT_CODE))
                continue;
            if (s->os < *a)
                *a = s->os;
            if (s->ce > *b)
                *b = s->ce;
            grew = true;
        }
    }
}

//! The piece of [*a, *b) on line li that op acts on, and whether op already covers it. sp holds
//! the line's spans afterwards. False when the line has no text in the range.
static bool piece_find(const GapBuffer* g, FormatOp op, const LineInfo* li, Spans* sp, size_t* a, size_t* b, bool* on)
{
    if (!piece_trim(g, li, a, b))
        return false;
    spans_scan(g, li->ls, li->le, sp);
    piece_extend(sp, op, a, b);
    *on = span_holding(sp, op, *a, *b) != NULL;
    return true;
}

// #endregion

// #region Inline edits

//! Take op off [a, b) inside the innermost op span holding it: close the span before the piece
//! and reopen it after, dropping a marker whose side is left empty. Spans nested in it that hold
//! the piece (the bold of ***x*** when italic comes off) stay whole around the piece.
static void unwrap_held(Edit* e, const Spans* sp, FormatOp op, size_t a, size_t b)
{
    const Span* hp = span_holding(sp, op, a, b);
    if (!hp)
        return;
    Span h = *hp;
    for (bool grew = true; grew;) {
        grew = false;
        for (int32_t i = 0; i < sp->n; i++) {
            const Span* s = &sp->v[i];
            if (s->os >= h.oe && s->ce <= h.cs && s->os <= a && b <= s->ce && (s->os < a || s->ce > b)) {
                a = s->os;
                b = s->ce;
                grew = true;
            }
        }
    }
    char mk[16];
    size_t m = h.oe - h.os;
    if (m >= sizeof(mk))
        m = sizeof(mk) - 1;
    gap_copy_to(e->g, h.os, m, mk);
    size_t a2 = a, b2 = b;
    while (a2 > h.oe && is_ws(gap_at(e->g, a2 - 1)))
        a2--;
    while (b2 < h.cs && is_ws(gap_at(e->g, b2)))
        b2++;
    if (b2 < h.cs)
        edit_replace(e, b2, 0, mk, m, false);
    else
        edit_replace(e, h.cs, h.ce - h.cs, "", 0, false);
    if (a2 > h.oe)
        edit_replace(e, a2, 0, mk, m, true);
    else
        edit_replace(e, h.os, h.oe - h.os, "", 0, true);
}

//! Put op's markers around [a, b); code uses a fence longer than any backtick run inside
static void piece_wrap(Edit* e, FormatOp op, size_t a, size_t b)
{
    if (op != FORMAT_CODE) {
        const char* mk = inline_marker(op);
        edit_replace(e, b, 0, mk, strlen(mk), false);
        edit_replace(e, a, 0, mk, strlen(mk), true);
        return;
    }
    size_t longest = 0, run = 0;
    for (size_t i = a; i < b; i++) {
        run = gap_at(e->g, i) == '`' ? run + 1 : 0;
        if (run > longest)
            longest = run;
    }
    size_t m = longest + 1 < 16 ? longest + 1 : 16;
    bool pad = gap_at(e->g, a) == '`' || gap_at(e->g, b - 1) == '`';
    char buf[20];
    size_t k = 0;
    if (pad)
        buf[k++] = ' ';
    memset(buf + k, '`', m);
    edit_replace(e, b, 0, buf, k + m, false);
    memset(buf, '`', m);
    k = m;
    if (pad)
        buf[k++] = ' ';
    edit_replace(e, a, 0, buf, k, true);
}

//! Put op on [*a, *b) of the line starting at ls (both tracked by e), swallowing op spans inside
static void piece_add(Edit* e, FormatOp op, size_t ls, size_t* a, size_t* b, Spans* sp)
{
    for (;;) {
        spans_scan(e->g, ls, line_end(e->g, ls), sp);
        const Span* in = NULL;
        for (int32_t i = 0; i < sp->n && !in; i++)
            if (sp->v[i].kind == op && sp->v[i].os >= *a && sp->v[i].ce <= *b)
                in = &sp->v[i];
        if (!in)
            break;
        Span s = *in;
        edit_replace(e, s.cs, s.ce - s.cs, "", 0, false);
        edit_replace(e, s.os, s.oe - s.os, "", 0, true);
    }
    piece_wrap(e, op, *a, *b);
}

//! Whether every piece of the selection already has op (any: there is a piece at all)
static void selection_query(const GapBuffer* g, FormatOp op, Range r, bool* any, bool* all)
{
    size_t first = line_start(g, r.a), last = last_line(g, r);
    Spans sp = { 0 };
    LineInfo li;
    *any = false;
    *all = true;
    for (size_t ls = first;;) {
        line_info(g, ls, &li);
        size_t a = ls == first ? r.a : ls, b = r.b;
        bool on = false;
        if (piece_find(g, op, &li, &sp, &a, &b, &on)) {
            *any = true;
            *all = *all && on;
        }
        if (ls >= last || li.le >= gap_len(g))
            break;
        ls = li.le + 1;
    }
    free(sp.v);
}

static bool selection_apply(GapBuffer* g, FormatOp op, FormatCursor* c, Range r)
{
    bool any, all;
    selection_query(g, op, r, &any, &all);
    if (!any)
        return false;
    size_t first = line_start(g, r.a);
    int32_t lines = count_lines(g, first, last_line(g, r));
    Edit e;
    edit_init(&e, g);
    size_t rb = r.b, a = 0, b = 0, sa = 0, sb = 0;
    bool have = false;
    edit_track(&e, &rb);
    edit_track(&e, &a);
    edit_track(&e, &b);
    Spans sp = { 0 };
    LineInfo li;
    size_t ls = first;
    for (int32_t k = 0; k < lines; k++) {
        line_info(g, ls, &li);
        a = k == 0 ? r.a : ls;
        b = rb;
        bool on = false;
        if (piece_find(g, op, &li, &sp, &a, &b, &on)) {
            if (all)
                unwrap_held(&e, &sp, op, a, b);
            else if (!on)
                piece_add(&e, op, ls, &a, &b, &sp);
            if (!have)
                sa = a;
            have = true;
            sb = b;
        }
        ls = line_end(g, ls) + 1;
    }
    free(sp.v);
    c->selecting = true;
    c->anchor = r.cur >= r.anc ? sa : sb;
    c->cursor = r.cur >= r.anc ? sb : sa;
    return e.changed;
}

//! What a style does at a cursor with no selection
typedef enum {
    CARET_WORD, //!< [a, b) is the word at the cursor
    CARET_PAIR, //!< [a, b) is an empty marker pair around the cursor
    CARET_INSIDE, //!< whitespace inside an op span: split it at the cursor
    CARET_EMPTY, //!< whitespace: type an empty pair
    CARET_NONE //!< whitespace inside a code span: nothing to do
} CaretCase;

//! A word character, or ' - . between two of them (don't, well-known, 3.14)
static bool word_at(const GapBuffer* g, const LineInfo* li, size_t i)
{
    char c = gap_at(g, i);
    if (is_wordc(c))
        return true;
    return (c == '\'' || c == '-' || c == '.') && i > li->ls && i + 1 < li->le && is_wordc(gap_at(g, i - 1))
        && is_wordc(gap_at(g, i + 1));
}

static CaretCase caret_find(const GapBuffer* g, FormatOp op, size_t p, const LineInfo* li, Spans* sp, size_t* a, size_t* b, bool* on)
{
    *on = false;
    *a = *b = p;
    spans_scan(g, li->ls, li->le, sp);
    size_t wa = p, wb = p;
    while (wa > li->box_end && word_at(g, li, wa - 1))
        wa--;
    while (wb < li->le && word_at(g, li, wb))
        wb++;
    if (wa < wb) {
        *a = wa;
        *b = wb;
        piece_extend(sp, op, a, b);
        *on = span_holding(sp, op, *a, *b) != NULL;
        return CARET_WORD;
    }
    const char* mk = inline_marker(op);
    size_t m = strlen(mk), left = 0, right = 0;
    while (left < p - li->ls && gap_at(g, p - 1 - left) == mk[0])
        left++;
    while (p + right < li->le && gap_at(g, p + right) == mk[0])
        right++;
    if (left == m && right == m) {
        *a = p - m;
        *b = p + m;
        *on = true;
        return CARET_PAIR;
    }
    for (int32_t i = 0; i < sp->n; i++)
        if (op != FORMAT_CODE && sp->v[i].kind == FORMAT_CODE && sp->v[i].oe <= p && p <= sp->v[i].cs)
            return CARET_NONE;
    if (span_holding(sp, op, p, p)) {
        *on = true;
        return CARET_INSIDE;
    }
    return CARET_EMPTY;
}

static bool caret_apply(GapBuffer* g, FormatOp op, FormatCursor* c, Range r)
{
    LineInfo li;
    line_info(g, r.cur, &li);
    size_t p = r.cur > li.box_end ? r.cur : li.box_end, a, b;
    Spans sp = { 0 };
    bool on;
    CaretCase cc = caret_find(g, op, p, &li, &sp, &a, &b, &on);
    Edit e;
    edit_init(&e, g);
    edit_track(&e, &p);
    edit_track(&e, &a);
    edit_track(&e, &b);
    switch (cc) {
    case CARET_WORD:
        if (on)
            unwrap_held(&e, &sp, op, a, b);
        else
            piece_add(&e, op, li.ls, &a, &b, &sp);
        break;
    case CARET_PAIR:
        edit_replace(&e, a, b - a, "", 0, false);
        break;
    case CARET_INSIDE:
        unwrap_held(&e, &sp, op, p, p);
        break;
    case CARET_EMPTY: {
        const char* mk = inline_marker(op);
        size_t m = strlen(mk);
        char buf[8];
        memcpy(buf, mk, m);
        memcpy(buf + m, mk, m);
        edit_replace(&e, p, 0, buf, 2 * m, false);
        p += m;
        break;
    }
    case CARET_NONE:
        break;
    }
    free(sp.v);
    c->cursor = c->anchor = p;
    c->selecting = false;
    return e.changed;
}

static bool state_inline(const GapBuffer* g, FormatOp op, Range r)
{
    if (r.sel) {
        bool any, all;
        selection_query(g, op, r, &any, &all);
        return any && all;
    }
    LineInfo li;
    line_info(g, r.cur, &li);
    size_t p = r.cur > li.box_end ? r.cur : li.box_end, a, b;
    Spans sp = { 0 };
    bool on;
    caret_find(g, op, p, &li, &sp, &a, &b, &on);
    free(sp.v);
    return on;
}

// #endregion

// #region Inserts

//! Whether [ls, le) starts a fence: up to three spaces, then three or more ` or ~. *rest is
//! where the run ends.
static bool fence_at(const GapBuffer* g, size_t ls, size_t le, char* ch, size_t* n, size_t* rest)
{
    size_t i = ls;
    while (i < le && i - ls < 3 && gap_at(g, i) == ' ')
        i++;
    char c = gap_at(g, i);
    if (i >= le || (c != '`' && c != '~'))
        return false;
    size_t j = i;
    while (j < le && gap_at(g, j) == c)
        j++;
    if (j - i < 3)
        return false;
    *ch = c;
    *n = j - i;
    *rest = j;
    return true;
}

//! The fenced block around position p: opening line [*ols, *ole) and closing line [*cls, *cle),
//! *cls == SIZE_MAX when it runs to the end of the note
static bool fence_find(const GapBuffer* g, size_t p, size_t* ols, size_t* ole, size_t* cls, size_t* cle)
{
    size_t len = gap_len(g), target = line_start(g, p), on = 0;
    bool open = false;
    char och = 0;
    for (size_t ls = 0;;) {
        size_t le = line_end(g, ls), n, rest;
        char ch;
        if (!open) {
            if (ls > target)
                return false;
            if (fence_at(g, ls, le, &ch, &n, &rest)) {
                open = true;
                och = ch;
                on = n;
                *ols = ls;
                *ole = le;
            }
        } else if (fence_at(g, ls, le, &ch, &n, &rest) && ch == och && n >= on) {
            bool bare = true;
            for (size_t i = rest; i < le && bare; i++)
                bare = is_space(gap_at(g, i));
            if (bare) {
                if (target <= ls) {
                    *cls = ls;
                    *cle = le;
                    return true;
                }
                open = false;
            }
        }
        if (le >= len)
            break;
        ls = le + 1;
    }
    if (!open)
        return false;
    *cls = *cle = SIZE_MAX;
    return true;
}

//! Put block on lines of its own at the cursor's line, with a blank line before and after. The
//! cursor lands caret bytes into the block, or after everything inserted for SIZE_MAX.
static bool insert_block(GapBuffer* g, FormatCursor* c, const char* block, size_t caret)
{
    Range r = selection(g, c);
    size_t p = r.b, ls = line_start(g, p), le = line_end(g, p), q = ls;
    while (q < p && is_space(gap_at(g, q)))
        q++;
    bool blank = true;
    for (size_t i = ls; i < le && blank; i++)
        blank = is_space(gap_at(g, i));
    Edit e;
    edit_init(&e, g);
    if (blank) {
        edit_replace(&e, ls, le - ls, "", 0, false);
        p = ls;
    } else {
        p = q == p ? ls : le;
    }
    size_t len = gap_len(g);
    const char* pre = "";
    if (p > 0 && gap_at(g, p - 1) != '\n')
        pre = "\n\n";
    else if (p >= 2 && gap_at(g, p - 2) != '\n')
        pre = "\n";
    const char* suf = "\n";
    if (p < len && gap_at(g, p) != '\n')
        suf = "\n\n";
    else if (p < len && (p + 1 >= len || gap_at(g, p + 1) == '\n'))
        suf = "";
    size_t np = strlen(pre), nb = strlen(block), ns = strlen(suf);
    char* buf = malloc(np + nb + ns + 1);
    if (!buf)
        return e.changed;
    memcpy(buf, pre, np);
    memcpy(buf + np, block, nb);
    memcpy(buf + np + nb, suf, ns);
    edit_replace(&e, p, 0, buf, np + nb + ns, false);
    free(buf);
    c->cursor = c->anchor = caret == SIZE_MAX ? p + np + nb + ns : p + np + caret;
    c->selecting = false;
    return true;
}

static bool starts_with(const GapBuffer* g, size_t a, size_t b, const char* s)
{
    size_t n = strlen(s);
    if (b - a < n)
        return false;
    for (size_t i = 0; i < n; i++)
        if (gap_at(g, a + i) != s[i])
            return false;
    return true;
}

//! [selection](url) with url selected, [text](url) with text selected, or [text](selected url);
//! the image form is ![alt](path)
static bool insert_link(GapBuffer* g, FormatCursor* c, bool image)
{
    Range r = selection(g, c);
    size_t a = r.a, b = r.b;
    while (a < b && is_ws(gap_at(g, a)))
        a++;
    while (b > a && is_ws(gap_at(g, b - 1)))
        b--;
    Edit e;
    edit_init(&e, g);
    const char* open = image ? "![" : "[";
    const char* hole = image ? "path" : "url";
    if (!r.sel || a == b) {
        const char* t = image ? "![alt](path)" : "[text](url)";
        edit_replace(&e, r.cur, 0, t, strlen(t), false);
        c->anchor = r.cur + strlen(open);
        c->cursor = c->anchor + (image ? 3 : 4);
    } else if (!image && (starts_with(g, a, b, "http://") || starts_with(g, a, b, "https://"))) {
        edit_replace(&e, b, 0, ")", 1, false);
        edit_replace(&e, a, 0, "[text](", 7, true);
        c->anchor = a + 1;
        c->cursor = a + 5;
    } else {
        char tail[8];
        int w = snprintf(tail, sizeof(tail), "](%s)", hole);
        edit_track(&e, &b);
        edit_replace(&e, b, 0, tail, (size_t)w, false);
        edit_replace(&e, a, 0, open, strlen(open), true);
        c->anchor = b + 2;
        c->cursor = b + 2 + strlen(hole);
    }
    c->selecting = true;
    return true;
}

static bool apply_code_block(GapBuffer* g, FormatCursor* c)
{
    Range r = selection(g, c);
    Edit e;
    edit_init(&e, g);
    size_t cur = r.cur, anc = r.anc, ols, ole, cls, cle;
    edit_track(&e, &cur);
    edit_track(&e, &anc);
    if (fence_find(g, r.a, &ols, &ole, &cls, &cle)) {
        if (cls != SIZE_MAX)
            edit_replace(&e, cls - 1, cle - cls + 1, "", 0, false);
        if (ole < gap_len(g))
            edit_replace(&e, ols, ole - ols + 1, "", 0, true);
        else if (ols > 0)
            edit_replace(&e, ols - 1, ole - ols + 1, "", 0, false);
        else
            edit_replace(&e, ols, ole - ols, "", 0, true);
    } else if (r.sel) {
        size_t first = line_start(g, r.a), end = line_end(g, last_line(g, r));
        size_t longest = 0, run = 0;
        for (size_t i = first; i < end; i++) {
            run = gap_at(g, i) == '`' ? run + 1 : 0;
            if (run > longest)
                longest = run;
        }
        size_t n = longest >= 3 ? longest + 1 : 3;
        if (n > 32)
            n = 32;
        char buf[34];
        buf[0] = '\n';
        memset(buf + 1, '`', n);
        edit_replace(&e, end, 0, buf, n + 1, false);
        memset(buf, '`', n);
        buf[n] = '\n';
        edit_replace(&e, first, 0, buf, n + 1, true);
    } else {
        return insert_block(g, c, "```\n\n```", 4);
    }
    c->cursor = cur;
    c->anchor = anc;
    return e.changed;
}

// #endregion

// #region API

bool format_apply(GapBuffer* g, FormatOp op, FormatCursor* c)
{
    if (!g || !c)
        return false;
    bool changed = false;
    switch (op) {
    case FORMAT_BOLD:
    case FORMAT_ITALIC:
    case FORMAT_STRIKE:
    case FORMAT_CODE: {
        Range r = selection(g, c);
        changed = r.sel ? selection_apply(g, op, c, r) : caret_apply(g, op, c, r);
        break;
    }
    case FORMAT_H1:
    case FORMAT_H2:
    case FORMAT_H3:
    case FORMAT_BULLET:
    case FORMAT_NUMBERED:
    case FORMAT_TASK:
    case FORMAT_QUOTE:
        changed = apply_lines(g, op, c);
        break;
    case FORMAT_LINK:
    case FORMAT_IMAGE:
        changed = insert_link(g, c, op == FORMAT_IMAGE);
        break;
    case FORMAT_TABLE:
        changed = insert_block(g, c, "|  |  |\n| --- | --- |\n|  |  |\n|  |  |", 2);
        break;
    case FORMAT_RULE:
        changed = insert_block(g, c, "---", SIZE_MAX);
        break;
    case FORMAT_CODE_BLOCK:
        changed = apply_code_block(g, c);
        break;
    default:
        break;
    }
    size_t n = gap_len(g);
    if (c->cursor > n)
        c->cursor = n;
    if (c->anchor > n)
        c->anchor = n;
    return changed;
}

bool format_state(const GapBuffer* g, const FormatCursor* c, FormatOp op)
{
    if (!g || !c)
        return false;
    Range r = selection(g, c);
    switch (op) {
    case FORMAT_BOLD:
    case FORMAT_ITALIC:
    case FORMAT_STRIKE:
    case FORMAT_CODE:
        return state_inline(g, op, r);
    case FORMAT_H1:
    case FORMAT_H2:
    case FORMAT_H3:
    case FORMAT_BULLET:
    case FORMAT_NUMBERED:
    case FORMAT_TASK:
    case FORMAT_QUOTE:
        return state_lines(g, op, r);
    case FORMAT_CODE_BLOCK: {
        size_t ols, ole, cls, cle;
        return fence_find(g, r.a, &ols, &ole, &cls, &cle);
    }
    default:
        return false;
    }
}

// #endregion
