// dawn_embed_index.c - chunker, index file format and ranking for the meaning index.
//
// The file format, all little-endian:
//
//   "DAWNEMBD"                          8 bytes, magic
//   u32 version                         EMBED_INDEX_VERSION
//   u32 dims, u32 count, u32 body_len
//   u64 body_hash, i64 mtime, u64 size
//   str model, str revision, str path, str title       (str = u16 length, then the bytes)
//   count x { u32 start, u32 len, u64 text_hash, str heading, f32 vector[dims] }
//   u64 checksum                        embed_hash() of every byte before it
//
// A reader trusts nothing: the checksum is checked first, then every length against the limits in
// the header and against the bytes left, before anything is allocated.

#include "dawn_embed_index.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EMBED_MAGIC "DAWNEMBD"
#define EMBED_MAGIC_LEN 8

// #region Text

uint64_t embed_hash(const void* data, size_t len)
{
    const uint8_t* p = data;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

size_t embed_body_offset(const char* text, size_t len)
{
    if (!text || len < 4)
        return 0;
    if (memcmp(text, "---", 3) != 0 || (text[3] != '\n' && text[3] != '\r'))
        return 0;
    for (size_t p = 4; p + 3 < len; p++) {
        if (text[p] != '\n' || memcmp(text + p + 1, "---", 3) != 0)
            continue;
        size_t after = p + 4;
        if (after >= len || text[after] == '\n' || text[after] == '\r') {
            size_t consumed = after;
            if (consumed < len && text[consumed] == '\n')
                consumed++;
            return consumed;
        }
    }
    return 0;
}

size_t embed_normalize_newlines(char* buf, size_t len)
{
    size_t r = 0, w = 0;
    while (r < len) {
        if (buf[r] == '\r') {
            buf[w++] = '\n';
            r++;
            if (r < len && buf[r] == '\n')
                r++;
        } else {
            buf[w++] = buf[r++];
        }
    }
    return w;
}

static bool is_utf8_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

//! Copy n bytes of s into out as a C string, cutting at a UTF-8 boundary when it does not fit.
static void copy_utf8(char* out, size_t out_size, const char* s, size_t n)
{
    if (!out || out_size == 0)
        return;
    if (n > out_size - 1) {
        n = out_size - 1;
        while (n > 0 && is_utf8_cont((unsigned char)s[n]))
            n--;
    }
    if (n > 0)
        memcpy(out, s, n);
    out[n] = '\0';
}

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

//! Trim [*s, *e) of blank space on both ends.
static void trim_range(const char* text, size_t* s, size_t* e)
{
    while (*s < *e && is_space(text[*s]))
        (*s)++;
    while (*e > *s && is_space(text[*e - 1]))
        (*e)--;
}

//! An ATX heading line: up to three spaces, 1-6 '#', then a space or the line's end. On success
//! [*ts, *te) is its text without the markers or a closing run of '#'.
static bool parse_heading(const char* text, size_t ls, size_t le, int32_t* level, size_t* ts, size_t* te)
{
    size_t p = ls;
    for (int32_t i = 0; i < 3 && p < le && text[p] == ' '; i++)
        p++;
    int32_t n = 0;
    while (p < le && text[p] == '#' && n < 7) {
        p++;
        n++;
    }
    if (n < 1 || n > 6)
        return false;
    if (p < le && text[p] != ' ' && text[p] != '\t')
        return false;
    size_t s = p, e = le;
    trim_range(text, &s, &e);
    // A closing sequence of '#' counts only after a space ("# Title ##").
    size_t c = e;
    while (c > s && text[c - 1] == '#')
        c--;
    if (c < e && (c == s || text[c - 1] == ' ' || text[c - 1] == '\t')) {
        e = c;
        trim_range(text, &s, &e);
    }
    if (level)
        *level = n;
    *ts = s;
    *te = e;
    return true;
}

//! A fence line: up to three spaces, then three or more '`' or '~'.
static bool parse_fence(const char* text, size_t ls, size_t le, char* ch, size_t* n)
{
    size_t p = ls;
    for (int32_t i = 0; i < 3 && p < le && text[p] == ' '; i++)
        p++;
    if (p >= le || (text[p] != '`' && text[p] != '~'))
        return false;
    char c = text[p];
    size_t k = 0;
    while (p < le && text[p] == c) {
        p++;
        k++;
    }
    if (k < 3)
        return false;
    *ch = c;
    *n = k;
    return true;
}

//! Whether a line closes a fence opened with n of ch: at least as many, then only blank space.
static bool closes_fence(const char* text, size_t ls, size_t le, char ch, size_t n)
{
    char c = 0;
    size_t k = 0;
    if (!parse_fence(text, ls, le, &c, &k) || c != ch || k < n)
        return false;
    size_t p = ls;
    while (p < le && (text[p] == ' ' || text[p] == ch))
        p++;
    while (p < le && is_space(text[p]))
        p++;
    return p == le;
}

static bool is_blank_line(const char* text, size_t ls, size_t le)
{
    for (size_t p = ls; p < le; p++)
        if (!is_space(text[p]))
            return false;
    return true;
}

void embed_note_title(const char* text, size_t len, const char* path, char* out, size_t out_size)
{
    if (!out || out_size == 0)
        return;
    out[0] = '\0';
    if (!text)
        len = 0;

    // 1) The frontmatter's title.
    size_t body = embed_body_offset(text, len);
    if (body > 0) {
        size_t pos = 4;
        while (pos < body) {
            const char* nl = memchr(text + pos, '\n', body - pos);
            size_t le = nl ? (size_t)(nl - text) : body;
            if (le - pos > 6 && memcmp(text + pos, "title:", 6) == 0) {
                size_t s = pos + 6, e = le;
                trim_range(text, &s, &e);
                if (e - s >= 2 && (text[s] == '"' || text[s] == '\'') && text[e - 1] == text[s]) {
                    s++;
                    e--;
                }
                if (e > s) {
                    copy_utf8(out, out_size, text + s, e - s);
                    return;
                }
            }
            pos = le + 1;
        }
    }

    // 2) The first level-one heading outside code.
    size_t pos = body;
    bool in_fence = false;
    char fch = 0;
    size_t fn = 0;
    while (pos < len) {
        const char* nl = memchr(text + pos, '\n', len - pos);
        size_t le = nl ? (size_t)(nl - text) : len;
        char c = 0;
        size_t k = 0;
        if (in_fence) {
            if (closes_fence(text, pos, le, fch, fn))
                in_fence = false;
        } else if (parse_fence(text, pos, le, &c, &k)) {
            in_fence = true;
            fch = c;
            fn = k;
        } else {
            int32_t level = 0;
            size_t ts = 0, te = 0;
            if (parse_heading(text, pos, le, &level, &ts, &te) && level == 1 && te > ts) {
                copy_utf8(out, out_size, text + ts, te - ts);
                return;
            }
        }
        pos = le + 1;
    }

    // 3) The file name.
    if (path) {
        const char* base = path;
        for (const char* p = path; *p; p++)
            if (*p == '/' || *p == '\\')
                base = p + 1;
        size_t n = strlen(base);
        if (n > 3 && strcmp(base + n - 3, ".md") == 0)
            n -= 3;
        copy_utf8(out, out_size, base, n);
    }
}

int32_t embed_estimate_tokens(const char* text, size_t len)
{
    if (!text || len == 0)
        return 0;
    size_t chars = 0, arabic = 0;
    const unsigned char* p = (const unsigned char*)text;
    const unsigned char* end = p + len;
    while (p < end) {
        uint32_t cp;
        int32_t n;
        if (*p < 0x80) {
            cp = *p;
            n = 1;
        } else if ((*p & 0xE0) == 0xC0) {
            cp = *p & 0x1F;
            n = 2;
        } else if ((*p & 0xF0) == 0xE0) {
            cp = *p & 0x0F;
            n = 3;
        } else if ((*p & 0xF8) == 0xF0) {
            cp = *p & 0x07;
            n = 4;
        } else {
            p++;
            continue;
        }
        for (int32_t i = 1; i < n && p + i < end; i++)
            cp = (cp << 6) | (p[i] & 0x3F);
        p += n;
        chars++;
        if ((cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0x0750 && cp <= 0x077F) || (cp >= 0xFB50 && cp <= 0xFDFF))
            arabic++;
    }
    if (chars == 0)
        return 0;
    double divisor = arabic * 2 > chars ? 2.5 : 3.6;
    int32_t tokens = (int32_t)((double)chars / divisor + 0.5);
    return tokens > 0 ? tokens : 1;
}

// #endregion

// #region Chunking

typedef enum { BLK_PARA, BLK_HEADING, BLK_FENCE } BlkKind;

typedef struct {
    size_t start, end; //!< [start, end), without the final newline
    BlkKind kind;
} Blk;

//! Pieces being assembled by embed_chunk().
typedef struct {
    const char* body;
    size_t len;
    float scale;
    EmbedChunk* out;
    int32_t max;
    int32_t count;
    char heading[EMBED_HEADING_MAX];
    // The piece still open, if any.
    bool open;
    bool heading_only; //!< It holds just the heading line so far
    size_t start, end;
    int32_t tokens;
} Chunker;

static int32_t chunk_tokens(const Chunker* c, size_t s, size_t e)
{
    int32_t t = embed_estimate_tokens(c->body + s, e - s);
    return (int32_t)((float)t * c->scale + 0.5f);
}

static void chunk_emit(Chunker* c, size_t s, size_t e)
{
    trim_range(c->body, &s, &e);
    if (e <= s || c->count >= c->max)
        return;
    EmbedChunk* ch = &c->out[c->count++];
    ch->start = (uint32_t)s;
    ch->len = (uint32_t)(e - s);
    ch->text_hash = embed_hash(c->body + s, e - s);
    memcpy(ch->heading, c->heading, sizeof(ch->heading));
}

static void chunk_flush(Chunker* c)
{
    if (c->open && !c->heading_only)
        chunk_emit(c, c->start, c->end);
    c->open = false;
}

//! Where the sentence starting at a ends: after '.', '!', '?', '…', '؟' or '。' (and any closing
//! quote or bracket) when blank space or the end follows, or after a newline. Never past e.
static size_t sentence_end(const char* t, size_t a, size_t e)
{
    size_t i = a;
    while (i < e) {
        unsigned char c = (unsigned char)t[i];
        if (c == '\n')
            return i + 1;
        size_t n = 0;
        if (c == '.' || c == '!' || c == '?')
            n = 1;
        else if (c == 0xD8 && i + 1 < e && (unsigned char)t[i + 1] == 0x9F)
            n = 2; // ؟
        else if (c == 0xE2 && i + 2 < e && (unsigned char)t[i + 1] == 0x80 && (unsigned char)t[i + 2] == 0xA6)
            n = 3; // …
        else if (c == 0xE3 && i + 2 < e && (unsigned char)t[i + 1] == 0x80 && (unsigned char)t[i + 2] == 0x82)
            n = 3; // 。
        if (n == 0) {
            i++;
            continue;
        }
        size_t j = i + n;
        while (j < e && (t[j] == ')' || t[j] == '"' || t[j] == '\'' || t[j] == ']'))
            j++;
        if (j >= e || is_space(t[j]) || n == 3) {
            while (j < e && (t[j] == ' ' || t[j] == '\t'))
                j++;
            return j;
        }
        i = j;
    }
    return e;
}

//! Where to cut a run that has no sentence end: about limit bytes on, backed off to a UTF-8
//! boundary and, when there is one in the second half, to just after a space. Always > p.
static size_t hard_cut(const char* t, size_t p, size_t e, size_t limit)
{
    size_t c = p + limit;
    if (c >= e)
        return e;
    while (c > p && is_utf8_cont((unsigned char)t[c]))
        c--;
    for (size_t q = c; q > p + limit / 2; q--)
        if (t[q - 1] == ' ' || t[q - 1] == '\t')
            return q;
    if (c > p)
        return c;
    c = p + 1;
    while (c < e && is_utf8_cont((unsigned char)t[c]))
        c++;
    return c;
}

//! Split a paragraph [s, e) that is too long for one piece. first is where the first piece
//! starts: s, or earlier when a heading line waits to be attached. Every piece but the last is
//! emitted; the last stays open so that small paragraphs after it can join.
static void chunk_split_paragraph(Chunker* c, size_t first, size_t s, size_t e)
{
    size_t acc_start = first, acc_end = first;
    int32_t acc_tokens = first < s ? chunk_tokens(c, first, s) : 0;
    bool acc_has = false;

    size_t a = s;
    while (a < e && c->count < c->max) {
        size_t b = sentence_end(c->body, a, e);
        int32_t st = chunk_tokens(c, a, b);
        if (st > EMBED_MAX_TOKENS) {
            size_t from = a;
            if (acc_has)
                chunk_emit(c, acc_start, acc_end);
            else
                from = acc_start;
            size_t per = (size_t)st > 0 ? (b - a) / (size_t)st : 4;
            size_t limit = per * EMBED_TARGET_TOKENS;
            if (limit < 64)
                limit = 64;
            size_t p = a;
            while (b - p > limit && c->count < c->max) {
                size_t cut = hard_cut(c->body, p, b, limit);
                chunk_emit(c, from, cut);
                p = from = cut;
            }
            acc_start = from;
            acc_end = b;
            acc_tokens = chunk_tokens(c, from, b);
            acc_has = true;
        } else if (acc_has && acc_tokens + st > EMBED_TARGET_TOKENS) {
            chunk_emit(c, acc_start, acc_end);
            acc_start = a;
            acc_end = b;
            acc_tokens = st;
        } else {
            acc_end = b;
            acc_tokens += st;
            acc_has = true;
        }
        a = b;
    }

    c->open = acc_has;
    c->heading_only = false;
    c->start = acc_start;
    c->end = acc_end;
    c->tokens = acc_tokens;
}

//! Append b to a growing block array; false (and nothing changed) when memory runs out.
static bool blk_push(Blk** blocks, int32_t* n, int32_t* cap, Blk b)
{
    if (*n == *cap) {
        if (*cap > INT32_MAX / 2)
            return false;
        int32_t ncap = *cap ? *cap * 2 : 64;
        Blk* grown = realloc(*blocks, sizeof(Blk) * (size_t)ncap);
        if (!grown)
            return false;
        *blocks = grown;
        *cap = ncap;
    }
    (*blocks)[(*n)++] = b;
    return true;
}

//! Cut the body into blocks: headings, fenced code, and paragraphs (runs of non-blank lines).
//! Returns a malloc'd array, or NULL (with *count 0) when there is nothing or memory runs out.
static Blk* split_blocks(const char* t, size_t len, int32_t* count)
{
    *count = 0;
    Blk* blocks = NULL;
    int32_t cap = 0, n = 0;
    bool have = false, in_fence = false;
    Blk cur = { 0 };
    char fch = 0;
    size_t fn = 0;

    size_t pos = 0;
    while (pos < len) {
        const char* nl = memchr(t + pos, '\n', len - pos);
        size_t le = nl ? (size_t)(nl - t) : len;
        size_t next = nl ? le + 1 : len;
        char c = 0;
        size_t k = 0, ts = 0, te = 0;

        if (in_fence) {
            cur.end = le;
            if (closes_fence(t, pos, le, fch, fn)) {
                in_fence = false;
                have = false;
                if (!blk_push(&blocks, &n, &cap, cur))
                    goto fail;
            }
        } else if (is_blank_line(t, pos, le)) {
            if (have && !blk_push(&blocks, &n, &cap, cur))
                goto fail;
            have = false;
        } else if (parse_fence(t, pos, le, &c, &k)) {
            if (have && !blk_push(&blocks, &n, &cap, cur))
                goto fail;
            cur = (Blk) { pos, le, BLK_FENCE };
            have = true;
            in_fence = true;
            fch = c;
            fn = k;
        } else if (parse_heading(t, pos, le, NULL, &ts, &te)) {
            if (have && !blk_push(&blocks, &n, &cap, cur))
                goto fail;
            have = false;
            if (!blk_push(&blocks, &n, &cap, (Blk) { pos, le, BLK_HEADING }))
                goto fail;
        } else if (have) {
            cur.end = le;
        } else {
            cur = (Blk) { pos, le, BLK_PARA };
            have = true;
        }
        pos = next;
    }
    // An unclosed fence runs to the end.
    if (have && !blk_push(&blocks, &n, &cap, cur))
        goto fail;

    *count = n;
    return blocks;

fail:
    free(blocks);
    return NULL;
}

int32_t embed_chunk(const char* body, size_t len, float token_scale, EmbedChunk* out, int32_t max)
{
    if (!body || !out || max <= 0 || len == 0 || len > EMBED_NOTE_MAX)
        return 0;

    int32_t nblocks = 0;
    Blk* blocks = split_blocks(body, len, &nblocks);
    if (!blocks)
        return 0;

    Chunker c = { .body = body, .len = len, .out = out, .max = max };
    c.scale = (token_scale > 0.05f && token_scale < 20.0f) ? token_scale : 1.0f;

    for (int32_t i = 0; i < nblocks && c.count < c.max; i++) {
        const Blk* b = &blocks[i];
        if (b->kind == BLK_HEADING) {
            chunk_flush(&c);
            size_t ts = 0, te = 0;
            if (parse_heading(body, b->start, b->end, NULL, &ts, &te))
                copy_utf8(c.heading, sizeof(c.heading), body + ts, te - ts);
            c.open = true;
            c.heading_only = true;
            c.start = b->start;
            c.end = b->end;
            c.tokens = chunk_tokens(&c, b->start, b->end);
            continue;
        }

        int32_t t = chunk_tokens(&c, b->start, b->end);
        size_t first = (c.open && c.heading_only) ? c.start : b->start;
        if (t > EMBED_MAX_TOKENS && b->kind == BLK_FENCE) {
            // Code is never split: it goes whole, as a piece of its own (with its heading).
            if (c.open && !c.heading_only) {
                chunk_flush(&c);
                first = b->start;
            }
            chunk_emit(&c, first, b->end);
            c.open = false;
        } else if (t > EMBED_MAX_TOKENS) {
            if (c.open && !c.heading_only) {
                chunk_flush(&c);
                first = b->start;
            }
            chunk_split_paragraph(&c, first, b->start, b->end);
        } else {
            if (c.open && !c.heading_only && c.tokens + t > EMBED_TARGET_TOKENS)
                chunk_flush(&c);
            if (c.open) {
                c.end = b->end;
                c.tokens += t;
                c.heading_only = false;
            } else {
                c.open = true;
                c.heading_only = false;
                c.start = b->start;
                c.end = b->end;
                c.tokens = t;
            }
        }
    }
    chunk_flush(&c);
    free(blocks);
    return c.count;
}

// #endregion

// #region Index file

void embed_index_free(EmbedIndex* idx)
{
    if (!idx)
        return;
    free(idx->chunks);
    free(idx->vectors);
    memset(idx, 0, sizeof(*idx));
}

typedef struct {
    uint8_t* p;
    size_t len;
    size_t cap;
} Writer;

static void put_bytes(Writer* w, const void* src, size_t n)
{
    // The buffer is sized exactly up front; this guard only protects against a sizing bug.
    if (w->len + n > w->cap)
        return;
    memcpy(w->p + w->len, src, n);
    w->len += n;
}

static void put_u16(Writer* w, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    put_bytes(w, b, 2);
}

static void put_u32(Writer* w, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    put_bytes(w, b, 4);
}

static void put_u64(Writer* w, uint64_t v)
{
    put_u32(w, (uint32_t)v);
    put_u32(w, (uint32_t)(v >> 32));
}

static void put_str(Writer* w, const char* s, size_t n)
{
    put_u16(w, (uint16_t)n);
    put_bytes(w, s, n);
}

//! strnlen() of a fixed field, which a well-formed index always NUL-terminates inside max.
static size_t field_len(const char* s, size_t max)
{
    const char* nul = memchr(s, '\0', max);
    return nul ? (size_t)(nul - s) : max;
}

bool embed_index_encode(const EmbedIndex* idx, uint8_t** out, size_t* out_len)
{
    *out = NULL;
    *out_len = 0;
    if (!idx || idx->dims < 1 || idx->dims > EMBED_MAX_DIMS || idx->count < 0 || idx->count > EMBED_MAX_PIECES)
        return false;
    if (idx->count > 0 && (!idx->chunks || !idx->vectors))
        return false;

    size_t model_n = field_len(idx->model, sizeof(idx->model));
    size_t rev_n = field_len(idx->revision, sizeof(idx->revision));
    size_t path_n = field_len(idx->path, sizeof(idx->path));
    size_t title_n = field_len(idx->title, sizeof(idx->title));
    if (model_n >= sizeof(idx->model) || rev_n >= sizeof(idx->revision) || path_n >= sizeof(idx->path)
        || title_n >= sizeof(idx->title))
        return false;

    size_t size = EMBED_MAGIC_LEN + 4 * 4 + 8 * 3 + 2 * 4 + model_n + rev_n + path_n + title_n + 8;
    for (int32_t i = 0; i < idx->count; i++) {
        const EmbedChunk* ch = &idx->chunks[i];
        size_t hn = field_len(ch->heading, sizeof(ch->heading));
        if (hn >= sizeof(ch->heading) || (uint64_t)ch->start + ch->len > idx->body_len)
            return false;
        size += 4 + 4 + 8 + 2 + hn + (size_t)idx->dims * 4;
    }
    if (size > EMBED_FILE_MAX)
        return false;

    Writer w = { .p = malloc(size), .cap = size };
    if (!w.p)
        return false;
    put_bytes(&w, EMBED_MAGIC, EMBED_MAGIC_LEN);
    put_u32(&w, EMBED_INDEX_VERSION);
    put_u32(&w, (uint32_t)idx->dims);
    put_u32(&w, (uint32_t)idx->count);
    put_u32(&w, idx->body_len);
    put_u64(&w, idx->body_hash);
    put_u64(&w, (uint64_t)idx->mtime);
    put_u64(&w, idx->size);
    put_str(&w, idx->model, model_n);
    put_str(&w, idx->revision, rev_n);
    put_str(&w, idx->path, path_n);
    put_str(&w, idx->title, title_n);
    for (int32_t i = 0; i < idx->count; i++) {
        const EmbedChunk* ch = &idx->chunks[i];
        put_u32(&w, ch->start);
        put_u32(&w, ch->len);
        put_u64(&w, ch->text_hash);
        put_str(&w, ch->heading, field_len(ch->heading, sizeof(ch->heading)));
        const float* v = idx->vectors + (size_t)i * (size_t)idx->dims;
        for (int32_t d = 0; d < idx->dims; d++) {
            uint32_t bits;
            memcpy(&bits, &v[d], 4);
            put_u32(&w, bits);
        }
    }
    put_u64(&w, embed_hash(w.p, w.len));
    if (w.len != size) {
        free(w.p);
        return false;
    }
    *out = w.p;
    *out_len = w.len;
    return true;
}

typedef struct {
    const uint8_t* p;
    size_t len;
    size_t pos;
    bool bad;
} Reader;

static bool take(Reader* r, size_t n)
{
    if (r->bad || n > r->len - r->pos) {
        r->bad = true;
        return false;
    }
    return true;
}

static uint32_t get_u32(Reader* r)
{
    if (!take(r, 4))
        return 0;
    const uint8_t* b = r->p + r->pos;
    r->pos += 4;
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static uint64_t get_u64(Reader* r)
{
    uint64_t lo = get_u32(r);
    uint64_t hi = get_u32(r);
    return lo | hi << 32;
}

//! A length-prefixed string into a fixed field of out_size bytes; bad unless it fits with its NUL
//! and holds no NUL of its own.
static void get_str(Reader* r, char* out, size_t out_size)
{
    if (!take(r, 2))
        return;
    size_t n = (size_t)r->p[r->pos] | (size_t)r->p[r->pos + 1] << 8;
    r->pos += 2;
    if (n >= out_size || !take(r, n) || memchr(r->p + r->pos, '\0', n)) {
        r->bad = true;
        return;
    }
    memcpy(out, r->p + r->pos, n);
    out[n] = '\0';
    r->pos += n;
}

bool embed_index_decode(const uint8_t* data, size_t len, EmbedIndex* out)
{
    memset(out, 0, sizeof(*out));
    const size_t min = EMBED_MAGIC_LEN + 4 * 4 + 8 * 3 + 2 * 4 + 8;
    if (!data || len < min || len > EMBED_FILE_MAX)
        return false;
    if (memcmp(data, EMBED_MAGIC, EMBED_MAGIC_LEN) != 0)
        return false;

    Reader tail = { .p = data, .len = len, .pos = len - 8 };
    if (get_u64(&tail) != embed_hash(data, len - 8))
        return false;

    // The checksum is excluded from what the body parser may consume.
    Reader r = { .p = data, .len = len - 8, .pos = EMBED_MAGIC_LEN };
    uint32_t version = get_u32(&r);
    uint32_t dims = get_u32(&r);
    uint32_t count = get_u32(&r);
    uint32_t body_len = get_u32(&r);
    if (r.bad || version != EMBED_INDEX_VERSION || dims < 1 || dims > EMBED_MAX_DIMS || count > EMBED_MAX_PIECES)
        return false;

    EmbedIndex idx = { 0 };
    idx.dims = (int32_t)dims;
    idx.count = (int32_t)count;
    idx.body_len = body_len;
    idx.body_hash = get_u64(&r);
    idx.mtime = (int64_t)get_u64(&r);
    idx.size = get_u64(&r);
    get_str(&r, idx.model, sizeof(idx.model));
    get_str(&r, idx.revision, sizeof(idx.revision));
    get_str(&r, idx.path, sizeof(idx.path));
    get_str(&r, idx.title, sizeof(idx.title));
    if (r.bad || !idx.model[0] || !idx.path[0])
        return false;

    // Each piece needs at least its fixed fields and its vector; refuse before allocating.
    uint64_t per_min = 4 + 4 + 8 + 2 + (uint64_t)dims * 4;
    if ((uint64_t)count * per_min > r.len - r.pos)
        return false;

    if (count > 0) {
        idx.chunks = calloc(count, sizeof(EmbedChunk));
        idx.vectors = malloc(sizeof(float) * (size_t)count * dims);
        if (!idx.chunks || !idx.vectors) {
            embed_index_free(&idx);
            return false;
        }
    }
    for (uint32_t i = 0; i < count && !r.bad; i++) {
        EmbedChunk* ch = &idx.chunks[i];
        ch->start = get_u32(&r);
        ch->len = get_u32(&r);
        ch->text_hash = get_u64(&r);
        get_str(&r, ch->heading, sizeof(ch->heading));
        if (r.bad || (uint64_t)ch->start + ch->len > body_len || ch->len == 0) {
            r.bad = true;
            break;
        }
        float* v = idx.vectors + (size_t)i * dims;
        for (uint32_t d = 0; d < dims && !r.bad; d++) {
            uint32_t bits = get_u32(&r);
            memcpy(&v[d], &bits, 4);
            if (!isfinite(v[d]))
                r.bad = true;
        }
    }
    if (r.bad || r.pos != r.len) {
        embed_index_free(&idx);
        return false;
    }
    *out = idx;
    return true;
}

bool embed_index_write(const char* file_path, const EmbedIndex* idx)
{
    if (!file_path || !file_path[0])
        return false;
    uint8_t* data;
    size_t len;
    if (!embed_index_encode(idx, &data, &len))
        return false;

    size_t n = strlen(file_path);
    char* tmp = malloc(n + 5);
    if (!tmp) {
        free(data);
        return false;
    }
    memcpy(tmp, file_path, n);
    memcpy(tmp + n, ".tmp", 5);

    bool ok = false;
    FILE* f = fopen(tmp, "wb");
    if (f) {
        ok = fwrite(data, 1, len, f) == len;
        ok = (fflush(f) == 0) && ok;
        ok = (fclose(f) == 0) && ok;
    }
    if (ok) {
#ifdef _WIN32
        remove(file_path); // rename() does not replace an existing file there
#endif
        ok = rename(tmp, file_path) == 0;
    }
    if (!ok)
        remove(tmp);
    free(tmp);
    free(data);
    return ok;
}

bool embed_index_read(const char* file_path, EmbedIndex* out)
{
    memset(out, 0, sizeof(*out));
    FILE* f = file_path ? fopen(file_path, "rb") : NULL;
    if (!f)
        return false;
    bool ok = false;
    uint8_t* data = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long size = ftell(f);
        if (size > 0 && (unsigned long)size <= EMBED_FILE_MAX && fseek(f, 0, SEEK_SET) == 0) {
            data = malloc((size_t)size);
            if (data && fread(data, 1, (size_t)size, f) == (size_t)size)
                ok = embed_index_decode(data, (size_t)size, out);
        }
    }
    free(data);
    fclose(f);
    return ok;
}

void embed_index_file_name(const char* note_path, char* out, size_t out_size)
{
    uint64_t h = note_path ? embed_hash(note_path, strlen(note_path)) : 0;
    snprintf(out, out_size, "%016llx.idx", (unsigned long long)h);
}

// #endregion

// #region Ranking

float embed_dot(const float* a, const float* b, int32_t dims)
{
    float s = 0.0f;
    for (int32_t i = 0; i < dims; i++)
        s += a[i] * b[i];
    return s;
}

void embed_normalize(float* v, int32_t dims)
{
    double sum = 0.0;
    for (int32_t i = 0; i < dims; i++)
        sum += (double)v[i] * (double)v[i];
    if (sum <= 0.0)
        return;
    float inv = (float)(1.0 / sqrt(sum));
    for (int32_t i = 0; i < dims; i++)
        v[i] *= inv;
}

void embed_topk_push(EmbedScored* top, int32_t* count, int32_t k, EmbedScored cand)
{
    if (k <= 0)
        return;
    int32_t n = *count;
    if (n == k && cand.score <= top[k - 1].score)
        return;
    int32_t pos = n < k ? n : k - 1;
    while (pos > 0 && top[pos - 1].score < cand.score) {
        top[pos] = top[pos - 1];
        pos--;
    }
    top[pos] = cand;
    if (n < k)
        *count = n + 1;
}

// #endregion
