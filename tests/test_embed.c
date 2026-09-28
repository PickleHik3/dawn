// test_embed.c - host tests for the pure half of the meaning index (dawn_embed_index.c):
// the chunker, the index file round trip and its corruption handling, and cosine top-k.

#include "dawn_embed_index.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        tests_run++;                                                           \
        if (!(cond)) {                                                         \
            tests_failed++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                      \
    } while (0)

// #region Helpers

//! Every piece lies in the body, in order, without overlap, trimmed, with a matching hash.
static void check_pieces_sane(const char* body, size_t len, const EmbedChunk* c, int32_t n)
{
    uint32_t prev_end = 0;
    for (int32_t i = 0; i < n; i++) {
        CHECK(c[i].len > 0);
        CHECK((size_t)c[i].start + c[i].len <= len);
        CHECK(c[i].start >= prev_end);
        CHECK(c[i].text_hash == embed_hash(body + c[i].start, c[i].len));
        char first = body[c[i].start], last = body[c[i].start + c[i].len - 1];
        CHECK(first != ' ' && first != '\n' && last != ' ' && last != '\n');
        prev_end = c[i].start + c[i].len;
    }
}

static int32_t piece_tokens(const char* body, const EmbedChunk* c)
{
    return embed_estimate_tokens(body + c->start, c->len);
}

//! A paragraph of n distinct sentences, each ending in ". ".
static char* make_sentences(int32_t n)
{
    size_t cap = (size_t)n * 64 + 1;
    char* s = malloc(cap);
    size_t len = 0;
    for (int32_t i = 0; i < n; i++)
        len += (size_t)snprintf(s + len, cap - len, "Sentence number %d talks about the visa office. ", i);
    s[len - 1] = '\0'; // drop the last space
    return s;
}

static bool contains(const char* body, const EmbedChunk* c, const char* needle)
{
    size_t nn = strlen(needle);
    for (uint32_t i = 0; i + nn <= c->len; i++)
        if (memcmp(body + c->start + i, needle, nn) == 0)
            return true;
    return false;
}

// #endregion

// #region Text

static void test_body_offset_and_title(void)
{
    const char* fm = "---\ntitle: \"Trip report\"\nauthor: me\n---\n# Heading\nBody.\n";
    size_t off = embed_body_offset(fm, strlen(fm));
    CHECK(off > 0 && strncmp(fm + off, "# Heading", 9) == 0);

    char title[EMBED_TITLE_MAX];
    embed_note_title(fm, strlen(fm), "/n/2026.md", title, sizeof(title));
    CHECK(strcmp(title, "Trip report") == 0);

    const char* h1 = "Intro line\n\n```\n# not a heading\n```\n\n## Two\n# Real title #\n";
    embed_note_title(h1, strlen(h1), "/n/x.md", title, sizeof(title));
    CHECK(strcmp(title, "Real title") == 0);

    const char* plain = "no headings here\n";
    embed_note_title(plain, strlen(plain), "/home/u/notes/20260928-1012.md", title, sizeof(title));
    CHECK(strcmp(title, "20260928-1012") == 0);

    // No closing delimiter: not frontmatter.
    const char* open = "---\ntitle: x\nstill yaml?\n";
    CHECK(embed_body_offset(open, strlen(open)) == 0);
    // "---" not followed by a newline: not frontmatter either.
    CHECK(embed_body_offset("----\na\n---\n", 11) == 0);

    // A long UTF-8 title is cut at a character boundary.
    char small[6];
    const char* arabic = "---\ntitle: مرحبا\n---\n";
    embed_note_title(arabic, strlen(arabic), NULL, small, sizeof(small));
    CHECK(strlen(small) == 4); // two 2-byte letters fit in 5 bytes, not a half third
}

static void test_normalize_and_estimate(void)
{
    char buf[] = "a\r\nb\rc\n";
    size_t n = embed_normalize_newlines(buf, strlen(buf));
    CHECK(n == 6 && memcmp(buf, "a\nb\nc\n", 6) == 0);

    CHECK(embed_estimate_tokens("", 0) == 0);
    CHECK(embed_estimate_tokens("abcdefghijklmnopqrstuvwxyz0123456789", 36) == 10);
    const char* ar = "مرحبا بكم"; // 9 characters, mostly Arabic: /2.5
    CHECK(embed_estimate_tokens(ar, strlen(ar)) == 4);
}

// #endregion

// #region Chunking

static void test_chunk_headings_and_merge(void)
{
    const char* body = "Preamble paragraph one.\n\nPreamble paragraph two.\n\n"
                       "# Travel\n\nFlights are booked.\n\nHotel is near the station.\n\n"
                       "## Visa\n\nPassport stamp needed.\n\n"
                       "## Empty section\n"
                       "## Budget ##\n\nAbout 900 KWD.\n";
    size_t len = strlen(body);
    EmbedChunk c[16];
    int32_t n = embed_chunk(body, len, 1.0f, c, 16);
    check_pieces_sane(body, len, c, n);
    CHECK(n == 4);
    // Small paragraphs merge; each heading starts a piece and is part of it.
    CHECK(strcmp(c[0].heading, "") == 0 && contains(body, &c[0], "one.") && contains(body, &c[0], "two."));
    CHECK(strcmp(c[1].heading, "Travel") == 0 && strncmp(body + c[1].start, "# Travel", 8) == 0);
    CHECK(contains(body, &c[1], "Hotel"));
    CHECK(strcmp(c[2].heading, "Visa") == 0 && contains(body, &c[2], "Passport"));
    // The heading-only section is dropped; a closing ## run is not part of the heading.
    CHECK(strcmp(c[3].heading, "Budget") == 0 && contains(body, &c[3], "900"));

    // max bounds the output.
    CHECK(embed_chunk(body, len, 1.0f, c, 2) == 2);
    CHECK(embed_chunk(body, 0, 1.0f, c, 16) == 0);
    CHECK(embed_chunk("   \n\n  \n", 8, 1.0f, c, 16) == 0);
}

static void test_chunk_merge_limit(void)
{
    // Forty paragraphs of 38 estimated tokens (136 characters) each: ten fit under the target,
    // the eleventh would not, so they merge into four pieces.
    char body[8192];
    size_t len = 0;
    for (int32_t i = 0; i < 40; i++)
        len += (size_t)snprintf(body + len, sizeof(body) - len,
            "Paragraph %02d says something about packing lists and the airport run in the morning, "
            "with enough words to weigh about sixty tokens or so.\n\n",
            i);
    EmbedChunk c[32];
    int32_t n = embed_chunk(body, len, 1.0f, c, 32);
    check_pieces_sane(body, len, c, n);
    CHECK(n == 4);
    for (int32_t i = 0; i < n; i++)
        CHECK(piece_tokens(body, &c[i]) <= EMBED_TARGET_TOKENS);
    // A token scale of 2 (the model counts twice the estimate) halves what fits: eight pieces.
    int32_t n2 = embed_chunk(body, len, 2.0f, c, 32);
    CHECK(n2 == 8);
}

static void test_chunk_long_paragraph(void)
{
    char* para = make_sentences(120); // ~120 * 13 tokens: far above EMBED_MAX_TOKENS
    char body[16384];
    int len = snprintf(body, sizeof(body), "# Long\n%s\n", para);
    free(para);
    EmbedChunk c[32];
    int32_t n = embed_chunk(body, (size_t)len, 1.0f, c, 32);
    check_pieces_sane(body, (size_t)len, c, n);
    CHECK(n >= 3);
    for (int32_t i = 0; i < n; i++) {
        CHECK(piece_tokens(body, &c[i]) <= EMBED_MAX_TOKENS);
        CHECK(body[c[i].start + c[i].len - 1] == '.'); // cut at sentence ends
        CHECK(strcmp(c[i].heading, "Long") == 0);
    }
    CHECK(strncmp(body + c[0].start, "# Long", 6) == 0); // the heading rides with the first piece
}

static void test_chunk_no_punctuation(void)
{
    // One enormous "sentence" of Arabic words: hard cuts, at spaces, never inside a character.
    char body[20000];
    size_t len = 0;
    while (len + 16 < sizeof(body))
        len += (size_t)snprintf(body + len, sizeof(body) - len, "كلمة ");
    EmbedChunk c[64];
    int32_t n = embed_chunk(body, len, 1.0f, c, 64);
    check_pieces_sane(body, len, c, n);
    CHECK(n >= 2);
    for (int32_t i = 0; i < n; i++) {
        CHECK(((unsigned char)body[c[i].start] & 0xC0) != 0x80);
        uint32_t end = c[i].start + c[i].len;
        CHECK(end == len || ((unsigned char)body[end] & 0xC0) != 0x80);
        CHECK(piece_tokens(body, &c[i]) <= EMBED_MAX_TOKENS);
    }
}

static void test_chunk_fence_not_split(void)
{
    char body[16384];
    size_t len = (size_t)snprintf(body, sizeof(body), "Intro.\n\n```c\n");
    for (int32_t i = 0; i < 200; i++)
        len += (size_t)snprintf(body + len, sizeof(body) - len, "int value_%d = %d; // x\n\n", i, i);
    len += (size_t)snprintf(body + len, sizeof(body) - len, "```\n\nAfter the code.\n");
    EmbedChunk c[16];
    int32_t n = embed_chunk(body, len, 1.0f, c, 16);
    check_pieces_sane(body, len, c, n);
    int32_t fences = 0;
    for (int32_t i = 0; i < n; i++) {
        if (contains(body, &c[i], "```c")) {
            fences++;
            // The whole block, blank lines and all, is one piece: both fences are inside it.
            CHECK(contains(body, &c[i], "value_0") && contains(body, &c[i], "value_199"));
            CHECK(strncmp(body + c[i].start + c[i].len - 3, "```", 3) == 0);
        }
    }
    CHECK(fences == 1);
    CHECK(contains(body, &c[n - 1], "After the code."));

    // An unclosed fence runs to the end without swallowing anything before it.
    const char* open = "Before.\n\n```\ncode\n# not heading\n";
    n = embed_chunk(open, strlen(open), 1.0f, c, 16);
    CHECK(n == 1 && contains(open, &c[0], "not heading") && strcmp(c[0].heading, "") == 0);
}

static void test_chunk_stable_hashes(void)
{
    // Editing one paragraph changes only that piece's hash.
    char a[4096], b[4096];
    const char* tmpl = "# One\n\nFirst %s paragraph.\n\n# Two\n\nSecond paragraph.\n\n# Three\n\nThird.\n";
    snprintf(a, sizeof(a), tmpl, "original");
    snprintf(b, sizeof(b), tmpl, "edited");
    EmbedChunk ca[8], cb[8];
    int32_t na = embed_chunk(a, strlen(a), 1.0f, ca, 8);
    int32_t nb = embed_chunk(b, strlen(b), 1.0f, cb, 8);
    CHECK(na == 3 && nb == 3);
    CHECK(ca[0].text_hash != cb[0].text_hash);
    CHECK(ca[1].text_hash == cb[1].text_hash && ca[2].text_hash == cb[2].text_hash);
}

// #endregion

// #region Index file

static void make_index(EmbedIndex* idx, int32_t count, int32_t dims)
{
    memset(idx, 0, sizeof(*idx));
    snprintf(idx->model, sizeof(idx->model), "embeddinggemma-300m");
    snprintf(idx->revision, sizeof(idx->revision), "029e17e73f22fb13");
    snprintf(idx->path, sizeof(idx->path), "/home/u/.local/share/dawn/2026.md");
    snprintf(idx->title, sizeof(idx->title), "Trip report");
    idx->dims = dims;
    idx->count = count;
    idx->body_len = 10000;
    idx->body_hash = 0x1234567890abcdefull;
    idx->mtime = 1790000000;
    idx->size = 10234;
    idx->chunks = calloc((size_t)count, sizeof(EmbedChunk));
    idx->vectors = malloc(sizeof(float) * (size_t)count * (size_t)dims);
    for (int32_t i = 0; i < count; i++) {
        idx->chunks[i].start = (uint32_t)(i * 100);
        idx->chunks[i].len = 90;
        idx->chunks[i].text_hash = (uint64_t)i * 7919u + 1;
        snprintf(idx->chunks[i].heading, sizeof(idx->chunks[i].heading), "Heading %d", i);
        for (int32_t d = 0; d < dims; d++)
            idx->vectors[(size_t)i * (size_t)dims + (size_t)d] = (float)(i + 1) * 0.001f * (float)(d + 1);
        embed_normalize(idx->vectors + (size_t)i * (size_t)dims, dims);
    }
}

static bool index_equal(const EmbedIndex* a, const EmbedIndex* b)
{
    if (strcmp(a->model, b->model) || strcmp(a->revision, b->revision) || strcmp(a->path, b->path)
        || strcmp(a->title, b->title) || a->dims != b->dims || a->count != b->count || a->body_len != b->body_len
        || a->body_hash != b->body_hash || a->mtime != b->mtime || a->size != b->size)
        return false;
    for (int32_t i = 0; i < a->count; i++) {
        if (a->chunks[i].start != b->chunks[i].start || a->chunks[i].len != b->chunks[i].len
            || a->chunks[i].text_hash != b->chunks[i].text_hash || strcmp(a->chunks[i].heading, b->chunks[i].heading))
            return false;
    }
    return memcmp(a->vectors, b->vectors, sizeof(float) * (size_t)a->count * (size_t)a->dims) == 0;
}

static void put_u32_at(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

//! Recompute the trailing checksum, so a test reaches the checks behind it.
static void reseal(uint8_t* data, size_t len)
{
    uint64_t h = embed_hash(data, len - 8);
    put_u32_at(data + len - 8, (uint32_t)h);
    put_u32_at(data + len - 4, (uint32_t)(h >> 32));
}

static void test_index_round_trip(void)
{
    EmbedIndex idx, back;
    make_index(&idx, 5, 256);
    uint8_t* data;
    size_t len;
    CHECK(embed_index_encode(&idx, &data, &len));
    CHECK(embed_index_decode(data, len, &back));
    CHECK(index_equal(&idx, &back));
    embed_index_free(&back);

    // Through a file, atomically: no .tmp is left behind.
    const char* file = "test_embed_roundtrip.idx";
    CHECK(embed_index_write(file, &idx));
    FILE* tmp = fopen("test_embed_roundtrip.idx.tmp", "rb");
    CHECK(tmp == NULL);
    if (tmp)
        fclose(tmp);
    CHECK(embed_index_read(file, &back));
    CHECK(index_equal(&idx, &back));
    embed_index_free(&back);
    remove(file);
    CHECK(!embed_index_read(file, &back)); // missing file

    // An empty note still round-trips.
    EmbedIndex empty;
    make_index(&empty, 0, 256);
    uint8_t* d2;
    size_t l2;
    CHECK(embed_index_encode(&empty, &d2, &l2));
    CHECK(embed_index_decode(d2, l2, &back) && back.count == 0);
    embed_index_free(&back);
    free(d2);
    embed_index_free(&empty);

    // The file name is stable per path.
    char n1[32], n2[32];
    embed_index_file_name("/a/b.md", n1, sizeof(n1));
    embed_index_file_name("/a/b.md", n2, sizeof(n2));
    CHECK(strcmp(n1, n2) == 0 && strlen(n1) == 20 && strcmp(n1 + 16, ".idx") == 0);
    embed_index_file_name("/a/c.md", n2, sizeof(n2));
    CHECK(strcmp(n1, n2) != 0);

    free(data);
    embed_index_free(&idx);
}

static void test_index_corruption(void)
{
    EmbedIndex idx, back;
    make_index(&idx, 3, 16);
    uint8_t* data;
    size_t len;
    CHECK(embed_index_encode(&idx, &data, &len));
    uint8_t* copy = malloc(len);

    // Every truncation fails, and leaves the output zeroed.
    int32_t accepted = 0;
    for (size_t cut = 0; cut < len; cut++) {
        if (embed_index_decode(data, cut, &back)) {
            accepted++;
            embed_index_free(&back);
        }
        CHECK(back.chunks == NULL && back.vectors == NULL);
    }
    CHECK(accepted == 0);

    // Any single flipped byte fails the checksum.
    int32_t flips_accepted = 0;
    for (size_t i = 0; i < len; i++) {
        memcpy(copy, data, len);
        copy[i] ^= 0x5A;
        if (embed_index_decode(copy, len, &back)) {
            flips_accepted++;
            embed_index_free(&back);
        }
    }
    CHECK(flips_accepted == 0);

    // Well-sealed but wrong: another version, zero or huge dims, a huge count.
    struct {
        size_t at;
        uint32_t value;
    } bad[] = { { 8, EMBED_INDEX_VERSION + 1 }, { 12, 0 }, { 12, EMBED_MAX_DIMS + 1 }, { 16, 0x7fffffff },
        { 16, 4 }, { 20, 5 } };
    for (size_t k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
        memcpy(copy, data, len);
        put_u32_at(copy + bad[k].at, bad[k].value);
        reseal(copy, len);
        CHECK(!embed_index_decode(copy, len, &back));
    }

    // A non-finite value in a vector is refused too (the last vector's last float).
    memcpy(copy, data, len);
    put_u32_at(copy + len - 8 - 4, 0x7fc00000u); // NaN
    reseal(copy, len);
    CHECK(!embed_index_decode(copy, len, &back));

    // Trailing garbage after the pieces (with a valid seal) is refused.
    uint8_t* longer = malloc(len + 4);
    memcpy(longer, data, len - 8);
    memset(longer + len - 8, 0, 4);
    reseal(longer, len + 4);
    CHECK(!embed_index_decode(longer, len + 4, &back));
    free(longer);

    // A corrupt file on disk reads as missing.
    const char* file = "test_embed_corrupt.idx";
    FILE* f = fopen(file, "wb");
    CHECK(f != NULL);
    if (f) {
        fwrite(data, 1, len / 2, f);
        fclose(f);
    }
    CHECK(!embed_index_read(file, &back));
    remove(file);

    // Out-of-bounds indexes are not written at all.
    idx.chunks[1].start = idx.body_len; // start + len past the body
    uint8_t* d2 = NULL;
    size_t l2 = 0;
    CHECK(!embed_index_encode(&idx, &d2, &l2) && d2 == NULL);
    idx.chunks[1].start = 100;
    idx.dims = 0;
    CHECK(!embed_index_encode(&idx, &d2, &l2));
    idx.dims = 16;

    free(copy);
    free(data);
    embed_index_free(&idx);
}

// #endregion

// #region Ranking

static void test_topk_and_cosine(void)
{
    float a[4] = { 3, 0, 4, 0 };
    embed_normalize(a, 4);
    CHECK(fabsf(embed_dot(a, a, 4) - 1.0f) < 1e-5f);
    float z[4] = { 0, 0, 0, 0 };
    embed_normalize(z, 4); // left alone, no NaN
    CHECK(z[0] == 0.0f && !isnan(z[1]));
    float b[4] = { 0, 1, 0, 0 };
    CHECK(fabsf(embed_dot(a, b, 4)) < 1e-6f);

    EmbedScored top[3];
    int32_t n = 0;
    float scores[] = { 0.1f, 0.9f, 0.5f, 0.7f, 0.2f, 0.95f, 0.5f };
    for (int32_t i = 0; i < 7; i++)
        embed_topk_push(top, &n, 3, (EmbedScored) { i, i, scores[i] });
    CHECK(n == 3);
    CHECK(top[0].note == 5 && top[1].note == 1 && top[2].note == 3);
    CHECK(top[0].score >= top[1].score && top[1].score >= top[2].score);

    // Fewer candidates than k: all kept, sorted.
    n = 0;
    embed_topk_push(top, &n, 3, (EmbedScored) { 0, 0, 0.2f });
    embed_topk_push(top, &n, 3, (EmbedScored) { 1, 0, 0.4f });
    CHECK(n == 2 && top[0].note == 1 && top[1].note == 0);
    embed_topk_push(top, &n, 0, (EmbedScored) { 2, 0, 1.0f }); // k = 0 is a no-op
    CHECK(n == 2);
}

// #endregion

int main(void)
{
    test_body_offset_and_title();
    test_normalize_and_estimate();
    test_chunk_headings_and_merge();
    test_chunk_merge_limit();
    test_chunk_long_paragraph();
    test_chunk_no_punctuation();
    test_chunk_fence_not_split();
    test_chunk_stable_hashes();
    test_index_round_trip();
    test_index_corruption();
    test_topk_and_cosine();

    printf("test-embed: %d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
