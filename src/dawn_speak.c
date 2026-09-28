// dawn_speak.c - Read aloud, karaoke-style: sentence builder, run state and the highlight.
//
// speak_build() is a small, forgiving markdown-to-speech pass over a flat copy of the note: it
// keeps what the page shows as words and drops what it shows as syntax (or doesn't show at all),
// remembering for every spoken byte the note byte it came from. The run itself is libai's
// (ai_speak.c); this file only polls it once a frame and turns "sentence i, sent t ms ago, at
// s seconds per byte" into colours. Any key stops the run (dawn_voice.c), so the note cannot
// change under the sentence spans while they are in use.

#include "dawn_speak.h"
#include "dawn_gap.h"
#include "dawn_nav.h"
#include "dawn_notice.h"
#include "dawn_theme.h"

#ifdef DAWN_HAS_SPEAK
#include "ai_speak.h"
#endif

#define SPEAK_SOFT_MAX 240 //!< Past this many bytes a sentence ends at the next space or comma
#define SPEAK_HARD_MAX 400 //!< ...and at this many, wherever it is (on a UTF-8 boundary)
#define SPEAK_MAX_SENTENCES 8192
#define SPEAK_DEFAULT_SEC_PER_BYTE 0.068 //!< ~15 characters a second, until one is measured
#define SPEAK_DEFAULT_LEAD_IN_MS 450 //!< Request to first sound, until one is measured
#define SPEAK_GLOW_SPREAD 6.0f //!< Bytes over which the word glow falls off either side
#define SPEAK_SENTENCE_TINT 0.45f //!< How far the sentence's background leans to tertiary_container

// #region Builder

typedef struct {
    SpeakText* t;
    size_t text_cap;
    int32_t sent_cap;
    size_t cur_off; //!< Where the sentence being built starts in t->text
    bool pending_space; //!< A space is owed before the next visible byte
    bool terminal_seen; //!< Just emitted . ! ? (and maybe closing quotes): a space ends the sentence
    bool oom;
} Builder;

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static bool is_alnum(char c) { return is_alpha(c) || is_digit(c) || ((unsigned char)c & 0x80); }

static void put_byte(Builder* b, char c, size_t pos)
{
    if (b->oom)
        return;
    SpeakText* t = b->t;
    if (t->text_len == b->text_cap) {
        size_t cap = b->text_cap ? b->text_cap * 2 : 1024;
        char* text = realloc(t->text, cap);
        if (!text) {
            b->oom = true;
            return;
        }
        t->text = text;
        uint32_t* map = realloc(t->map, cap * sizeof(uint32_t));
        if (!map) {
            b->oom = true;
            return;
        }
        t->map = map;
        b->text_cap = cap;
    }
    t->text[t->text_len] = c;
    t->map[t->text_len] = (uint32_t)pos;
    t->text_len++;
}

//! Close the sentence being built. One with no letter or digit in it (a stray dash, a lone
//! bullet) is dropped rather than sent to be spoken as silence.
static void end_sentence(Builder* b)
{
    SpeakText* t = b->t;
    b->pending_space = false;
    b->terminal_seen = false;
    size_t len = t->text_len - b->cur_off;
    while (len > 0 && t->text[b->cur_off + len - 1] == ' ')
        len--;
    bool speakable = false;
    for (size_t i = 0; i < len && !speakable; i++)
        speakable = is_alnum(t->text[b->cur_off + i]);
    if (!speakable || b->oom || t->count >= SPEAK_MAX_SENTENCES) {
        t->text_len = b->cur_off;
        return;
    }
    if (t->count == b->sent_cap) {
        int32_t cap = b->sent_cap ? b->sent_cap * 2 : 32;
        SpeakSentence* s = realloc(t->sentences, (size_t)cap * sizeof(SpeakSentence));
        if (!s) {
            b->oom = true;
            t->text_len = b->cur_off;
            return;
        }
        t->sentences = s;
        b->sent_cap = cap;
    }
    SpeakSentence* s = &t->sentences[t->count++];
    s->off = b->cur_off;
    s->len = len;
    s->start = t->map[b->cur_off];
    s->end = (size_t)t->map[b->cur_off + len - 1] + 1;
    t->text_len = b->cur_off + len;
    b->cur_off = t->text_len;
}

//! Whether the word just before a '.' at the end of the sentence is an abbreviation or an
//! initial, so the '.' doesn't end the sentence ("Dr. Aziz", "J. Smith", "e.g. this").
static bool ends_with_abbreviation(const Builder* b)
{
    const SpeakText* t = b->t;
    size_t end = t->text_len - 1; // the '.'
    size_t start = end;
    while (start > b->cur_off && !is_space(t->text[start - 1]))
        start--;
    size_t n = end - start;
    if (n == 1 && is_alpha(t->text[start]))
        return true;
    static const char* const abbr[] = { "mr", "mrs", "ms", "dr", "st", "vs", "etc", "e.g", "i.e", "jr", "sr", "prof", "no" };
    for (size_t i = 0; i < sizeof(abbr) / sizeof(abbr[0]); i++) {
        size_t an = strlen(abbr[i]);
        if (an != n)
            continue;
        bool same = true;
        for (size_t k = 0; k < n && same; k++) {
            char c = t->text[start + k];
            if (c >= 'A' && c <= 'Z')
                c = (char)(c - 'A' + 'a');
            same = c == abbr[i][k];
        }
        if (same)
            return true;
    }
    return false;
}

//! Whether the bytes just emitted end in a sentence mark from outside ASCII: … ؟ 。 ！ ？
static bool ends_with_wide_terminal(const Builder* b)
{
    const SpeakText* t = b->t;
    size_t n = t->text_len - b->cur_off;
    const unsigned char* e = (const unsigned char*)t->text + t->text_len;
    if (n >= 2 && e[-2] == 0xD8 && e[-1] == 0x9F)
        return true; // ؟
    if (n >= 3) {
        if (e[-3] == 0xE2 && e[-2] == 0x80 && e[-1] == 0xA6)
            return true; // …
        if (e[-3] == 0xE3 && e[-2] == 0x80 && e[-1] == 0x82)
            return true; // 。
        if (e[-3] == 0xEF && e[-2] == 0xBC && (e[-1] == 0x81 || e[-1] == 0x9F))
            return true; // ！ ？
    }
    return false;
}

//! One visible byte (or whitespace) of spoken text, from note byte pos.
static void emit(Builder* b, char c, size_t pos)
{
    size_t cur = b->t->text_len - b->cur_off;
    if (is_space(c)) {
        if (b->terminal_seen) {
            end_sentence(b);
            return;
        }
        if (cur > 0)
            b->pending_space = true;
        if (cur >= SPEAK_SOFT_MAX)
            end_sentence(b);
        return;
    }
    bool lead = ((unsigned char)c & 0xC0) != 0x80;
    if (lead && cur >= SPEAK_HARD_MAX)
        end_sentence(b);
    if (b->terminal_seen && lead && !strchr("\"')]*_", c) && (unsigned char)c != 0xE2 && (unsigned char)c != 0xC2)
        b->terminal_seen = false; // "3.5", "a.b": the mark was inside a word
    if (b->pending_space && b->t->text_len > b->cur_off)
        put_byte(b, ' ', pos);
    b->pending_space = false;
    put_byte(b, c, pos);
    if (b->oom)
        return;
    if (c == '.' || c == '!' || c == '?') {
        if (c != '.' || !ends_with_abbreviation(b))
            b->terminal_seen = true;
    } else if (!lead && ends_with_wide_terminal(b)) {
        b->terminal_seen = true;
    }
    if ((c == ',' || c == ';') && b->t->text_len - b->cur_off >= SPEAK_SOFT_MAX)
        end_sentence(b);
}

// #endregion

// #region Markdown, loosely

//! Skip spaces and tabs from i up to end.
static size_t skip_blanks(const char* s, size_t i, size_t end)
{
    while (i < end && (s[i] == ' ' || s[i] == '\t'))
        i++;
    return i;
}

//! A ``` or ~~~ fence line (after leading blanks).
static bool is_fence(const char* s, size_t i, size_t end)
{
    i = skip_blanks(s, i, end);
    if (end - i < 3)
        return false;
    return (s[i] == '`' && s[i + 1] == '`' && s[i + 2] == '`') || (s[i] == '~' && s[i + 1] == '~' && s[i + 2] == '~');
}

//! A thematic break: three or more of one of - * _ and nothing but blanks.
static bool is_hr(const char* s, size_t i, size_t end)
{
    i = skip_blanks(s, i, end);
    if (i >= end || (s[i] != '-' && s[i] != '*' && s[i] != '_'))
        return false;
    char m = s[i];
    int32_t count = 0;
    for (; i < end; i++) {
        if (s[i] == m)
            count++;
        else if (s[i] != ' ' && s[i] != '\t')
            return false;
    }
    return count >= 3;
}

//! A table's |---|:--:| divider row.
static bool is_table_divider(const char* s, size_t i, size_t end)
{
    bool pipe = false, dash = false;
    for (; i < end; i++) {
        char c = s[i];
        if (c == '|')
            pipe = true;
        else if (c == '-')
            dash = true;
        else if (c != ':' && c != ' ' && c != '\t')
            return false;
    }
    return pipe && dash;
}

//! Find c in s[i..end), or end.
static size_t find_byte(const char* s, size_t i, size_t end, char c)
{
    while (i < end && s[i] != c)
        i++;
    return i;
}

//! From an opening bracket at i, the index of its matching close within [i, end), or end.
static size_t match_bracket(const char* s, size_t i, size_t end, char open, char close)
{
    int32_t depth = 0;
    for (; i < end; i++) {
        if (s[i] == '\\' && i + 1 < end) {
            i++;
            continue;
        }
        if (s[i] == open)
            depth++;
        else if (s[i] == close && --depth == 0)
            return i;
    }
    return end;
}

//! Speak the inline content of one line, s[i..end), dropping syntax.
static void speak_inline(Builder* b, const char* s, size_t i, size_t end, size_t base)
{
    int32_t link_depth = 0;
    while (i < end && !b->oom) {
        char c = s[i];
        switch (c) {
        case '\\':
            if (i + 1 < end && !is_alnum(s[i + 1]) && !is_space(s[i + 1])) {
                emit(b, s[i + 1], base + i + 1);
                i += 2;
                continue;
            }
            break;
        case '`': { // inline code: left out entirely
            size_t run = 0;
            while (i + run < end && s[i + run] == '`')
                run++;
            size_t k = i + run;
            while (k < end) {
                if (s[k] == '`') {
                    size_t r2 = 0;
                    while (k + r2 < end && s[k + r2] == '`')
                        r2++;
                    if (r2 == run) {
                        emit(b, ' ', base + i);
                        i = k + r2;
                        goto next;
                    }
                    k += r2;
                } else {
                    k++;
                }
            }
            i += run;
            continue;
        }
        case '!':
            if (i + 1 < end && s[i + 1] == '[') { // image: nothing to say
                size_t close = match_bracket(s, i + 1, end, '[', ']');
                if (close < end) {
                    i = close + 1;
                    if (i < end && s[i] == '(') {
                        size_t paren = match_bracket(s, i, end, '(', ')');
                        i = paren < end ? paren + 1 : end;
                    }
                    continue;
                }
            }
            break;
        case '[':
            if (i + 1 < end && s[i + 1] == '^') { // footnote reference
                size_t close = find_byte(s, i, end, ']');
                if (close < end) {
                    i = close + 1;
                    continue;
                }
            }
            if (match_bracket(s, i, end, '[', ']') < end) { // link text is spoken, brackets not
                link_depth++;
                i++;
                continue;
            }
            break;
        case ']':
            if (link_depth > 0) {
                link_depth--;
                i++;
                if (i < end && s[i] == '(') {
                    size_t paren = match_bracket(s, i, end, '(', ')');
                    i = paren < end ? paren + 1 : end;
                } else if (i < end && s[i] == '[') {
                    size_t close = find_byte(s, i, end, ']');
                    i = close < end ? close + 1 : end;
                }
                continue;
            }
            break;
        case '<': // HTML tags, comments and <autolinks>
            if (i + 1 < end && (is_alpha(s[i + 1]) || s[i + 1] == '/' || s[i + 1] == '!')) {
                size_t close = find_byte(s, i, end, '>');
                if (close < end) {
                    emit(b, ' ', base + i);
                    i = close + 1;
                    continue;
                }
            }
            break;
        case '*':
            i++;
            continue;
        case '_':
            // snake_case keeps a space; emphasis underscores just go
            if (i > 0 && i + 1 < end && is_alnum(s[i - 1]) && is_alnum(s[i + 1]))
                emit(b, ' ', base + i);
            i++;
            continue;
        case '~':
        case '=':
            if (i + 1 < end && s[i + 1] == c) { // ~~strike~~, ==mark==
                i += 2;
                continue;
            }
            break;
        case '$':
            // $math$: an opening $ touching a non-digit, a closing $ touching text
            if (i + 1 < end && !is_space(s[i + 1]) && !is_digit(s[i + 1])) {
                size_t k = find_byte(s, i + 1, end, '$');
                if (k < end && !is_space(s[k - 1])) {
                    i = k + 1;
                    continue;
                }
            }
            break;
        case ':': { // :emoji: shortcodes
            size_t k = i + 1;
            while (k < end && k - i <= 40 && (is_alpha(s[k]) || is_digit(s[k]) || s[k] == '_' || s[k] == '+' || s[k] == '-'))
                k++;
            if (k < end && s[k] == ':' && k > i + 1 && is_alpha(s[i + 1])) {
                i = k + 1;
                continue;
            }
            break;
        }
        case '{': // {#heading-id}
            if (i + 1 < end && s[i + 1] == '#') {
                size_t close = find_byte(s, i, end, '}');
                if (close < end) {
                    i = close + 1;
                    continue;
                }
            }
            break;
        case '&': { // &amp; is "&"; other entities are dropped
            size_t k = i + 1;
            while (k < end && k - i <= 10 && (is_alpha(s[k]) || is_digit(s[k]) || s[k] == '#'))
                k++;
            if (k < end && s[k] == ';' && k > i + 1) {
                if (k - i == 4 && memcmp(s + i, "&amp", 4) == 0)
                    emit(b, '&', base + i);
                else
                    emit(b, ' ', base + i);
                i = k + 1;
                continue;
            }
            break;
        }
        case '|':
            emit(b, ' ', base + i);
            i++;
            continue;
        case 'h': // a bare URL isn't read out
            if ((i == 0 || is_space(s[i - 1]) || s[i - 1] == '(')
                && ((end - i > 7 && memcmp(s + i, "http://", 7) == 0) || (end - i > 8 && memcmp(s + i, "https://", 8) == 0))) {
                while (i < end && !is_space(s[i]))
                    i++;
                continue;
            }
            break;
        default:
            break;
        }
        emit(b, c, base + i);
        i++;
    next:;
    }
}

bool speak_build(const char* src, size_t len, size_t base, bool at_line_start, bool in_fence, SpeakText* out)
{
    memset(out, 0, sizeof(*out));
    if (!src || len == 0)
        return true;
    Builder b = { .t = out };
    bool in_math = false;
    bool first = true;

    for (size_t ls = 0; ls < len && !b.oom;) {
        size_t le = find_byte(src, ls, len, '\n');
        bool real_start = !first || at_line_start;
        first = false;

        if (in_fence) {
            if (real_start && is_fence(src, ls, le))
                in_fence = false;
            goto line_done;
        }
        size_t i = skip_blanks(src, ls, le);
        if (in_math) {
            if (le - i >= 2 && src[i] == '$' && src[i + 1] == '$')
                in_math = false;
            goto line_done;
        }

        bool block_line = false; // a heading, list item or table row: its end ends the sentence
        if (real_start) {
            if (is_fence(src, ls, le)) {
                end_sentence(&b);
                in_fence = true;
                goto line_done;
            }
            if (le - i >= 2 && src[i] == '$' && src[i + 1] == '$') {
                end_sentence(&b);
                // a one-line $$...$$ closes itself
                bool closes = false;
                for (size_t k = i + 2; k + 1 < le && !closes; k++)
                    closes = src[k] == '$' && src[k + 1] == '$';
                in_math = !closes;
                goto line_done;
            }
            if (i == le) { // blank line: paragraph break
                end_sentence(&b);
                goto line_done;
            }
            if (is_hr(src, ls, le) || is_table_divider(src, i, le)) {
                end_sentence(&b);
                goto line_done;
            }
            if (le - i >= 4 && memcmp(src + i, "<!--", 4) == 0)
                goto line_done;

            // Line prefixes: quotes, then a heading or list marker, then a task box.
            while (i < le && src[i] == '>')
                i = skip_blanks(src, i + 1, le);
            if (i < le && src[i] == '#') {
                size_t k = i;
                while (k < le && src[k] == '#' && k - i < 7)
                    k++;
                if (k - i <= 6 && (k == le || src[k] == ' ' || src[k] == '\t')) {
                    block_line = true;
                    i = skip_blanks(src, k, le);
                }
            } else if (i + 1 < le && (src[i] == '-' || src[i] == '*' || src[i] == '+') && (src[i + 1] == ' ' || src[i + 1] == '\t')) {
                block_line = true;
                i = skip_blanks(src, i + 1, le);
            } else if (i < le && is_digit(src[i])) {
                size_t k = i;
                while (k < le && is_digit(src[k]) && k - i < 10)
                    k++;
                if (k + 1 < le && (src[k] == '.' || src[k] == ')') && (src[k + 1] == ' ' || src[k + 1] == '\t')) {
                    block_line = true;
                    i = skip_blanks(src, k + 1, le);
                }
            } else if (i < le && src[i] == '|') {
                block_line = true;
            } else if (i + 1 < le && src[i] == '[' && src[i + 1] == '^') { // footnote definition
                size_t close = find_byte(src, i, le, ']');
                if (close + 1 < le && src[close + 1] == ':') {
                    block_line = true;
                    i = skip_blanks(src, close + 2, le);
                }
            }
            if (block_line && le - i >= 3 && src[i] == '[' && src[i + 2] == ']'
                && (src[i + 1] == ' ' || src[i + 1] == 'x' || src[i + 1] == 'X'))
                i = skip_blanks(src, i + 3, le);
            if (block_line)
                end_sentence(&b); // a new block starts a new sentence
        }

        speak_inline(&b, src, i, le, base);
        if (block_line)
            end_sentence(&b);
        else if (le < len)
            emit(&b, ' ', base + le); // a soft line break inside a paragraph

    line_done:
        ls = le + 1;
    }
    end_sentence(&b);

    if (b.oom) {
        speak_text_free(out);
        return false;
    }
    return true;
}

void speak_text_free(SpeakText* t)
{
    if (!t)
        return;
    free(t->text);
    free(t->map);
    free(t->sentences);
    memset(t, 0, sizeof(*t));
}

// #endregion

// #region Run state

static struct {
    bool active;
    SpeakText text; //!< The run's sentences (a copy went to the worker)
    int32_t index; //!< The sentence out now
    float est; //!< Estimated spoken byte within it, this frame
    bool shutdown_hooked;
} g_speak;

bool speak_active(void) { return g_speak.active; }

static void speak_clear(void)
{
    speak_text_free(&g_speak.text);
    g_speak.active = false;
    g_speak.index = 0;
    g_speak.est = -1.0f;
}

#ifdef DAWN_HAS_SPEAK

static void speak_on_shutdown(void)
{
    if (g_speak.active)
        ai_speak_shutdown();
    speak_clear();
}

//! Whether note byte pos is inside a ``` block, counting fences from the top.
static bool pos_in_fence(size_t pos)
{
    bool in = false;
    size_t ls = 0;
    while (ls < pos) {
        size_t i = ls;
        while (i < pos && (gap_at(&app.text, i) == ' ' || gap_at(&app.text, i) == '\t'))
            i++;
        if (i + 3 <= pos) {
            char c = gap_at(&app.text, i);
            if ((c == '`' || c == '~') && gap_at(&app.text, i + 1) == c && gap_at(&app.text, i + 2) == c)
                in = !in;
        }
        while (ls < pos && gap_at(&app.text, ls) != '\n')
            ls++;
        ls++;
    }
    return in;
}

bool speak_start(void)
{
    if (g_speak.active)
        speak_stop();

    size_t len = gap_len(&app.text);
    size_t from, to;
    if (has_selection()) {
        get_selection(&from, &to);
    } else {
        from = app.cursor;
        to = len;
        // From the start of the word the cursor is in
        while (from > 0 && !is_space(gap_at(&app.text, from - 1)))
            from--;
    }
    if (to > len)
        to = len;
    if (from >= to) {
        notice_post(NOTICE_INFO, "nothing to read aloud");
        return false;
    }

    char* flat = malloc(to - from);
    if (!flat)
        return false;
    gap_copy_to(&app.text, from, to - from, flat);
    bool at_line_start = from == 0 || gap_at(&app.text, from - 1) == '\n';
    bool ok = speak_build(flat, to - from, from, at_line_start, pos_in_fence(from), &g_speak.text);
    free(flat);
    if (!ok || g_speak.text.count == 0) {
        speak_clear();
        notice_post(NOTICE_INFO, "nothing to read aloud");
        return false;
    }

    int32_t n = g_speak.text.count;
    const char** texts = malloc((size_t)n * sizeof(char*));
    size_t* lens = malloc((size_t)n * sizeof(size_t));
    bool started = false;
    if (texts && lens) {
        for (int32_t i = 0; i < n; i++) {
            texts[i] = g_speak.text.text + g_speak.text.sentences[i].off;
            lens[i] = g_speak.text.sentences[i].len;
        }
        started = ai_speak_start(texts, lens, n);
    }
    free(texts);
    free(lens);
    if (!started) {
        speak_clear();
        notice_post(NOTICE_INFO, "read aloud · couldn't start");
        return false;
    }

    app.selecting = false; // the selection would cover the highlight
    g_speak.active = true;
    g_speak.index = 0;
    g_speak.est = -1.0f;
    if (!g_speak.shutdown_hooked && DAWN_BACKEND(app)->on_shutdown) {
        DAWN_BACKEND(app)->on_shutdown(speak_on_shutdown);
        g_speak.shutdown_hooked = true;
    }
    return true;
}

void speak_stop(void)
{
    if (!g_speak.active)
        return;
    ai_speak_stop();
    ai_speak_ack();
    speak_clear();
}

bool speak_tick(int64_t now_ms)
{
    (void)now_ms; // libai times the sentence on its own clock
    if (!g_speak.active)
        return false;
    ai_speak_status_t st;
    ai_speak_status(&st);

    if (st.phase != AI_SPEAK_RUNNING) {
        if (st.phase == AI_SPEAK_FAILED) {
            char msg[160];
            snprintf(msg, sizeof(msg), "read aloud · %s", st.error);
            notice_post(NOTICE_INFO, msg);
        }
        ai_speak_ack();
        speak_clear();
        return true; // one more frame to take the highlight off
    }

    if (st.index < 0 || st.index >= g_speak.text.count) {
        g_speak.est = -1.0f;
        return true;
    }
    g_speak.index = st.index;
    double spb = st.sec_per_byte > 0.0 ? st.sec_per_byte : SPEAK_DEFAULT_SEC_PER_BYTE;
    int32_t lead = st.lead_in_ms >= 0 ? st.lead_in_ms : SPEAK_DEFAULT_LEAD_IN_MS;
    double spoken_s = (double)(st.elapsed_ms - lead) / 1000.0;
    float est = (float)(spoken_s / spb);
    float limit = (float)g_speak.text.sentences[g_speak.index].len + SPEAK_GLOW_SPREAD;
    if (est < -SPEAK_GLOW_SPREAD)
        est = -SPEAK_GLOW_SPREAD;
    if (est > limit)
        est = limit;
    g_speak.est = est;
    return true;
}

#else // !DAWN_HAS_SPEAK: no launcher bridge in this build

bool speak_start(void)
{
    notice_post(NOTICE_INFO, "read aloud needs termux launcher");
    return false;
}

void speak_stop(void) { speak_clear(); }

bool speak_tick(int64_t now_ms)
{
    (void)now_ms;
    return false;
}

#endif // DAWN_HAS_SPEAK

bool speak_style_at(size_t pos, VoiceStyle* out)
{
    if (!g_speak.active || g_speak.index >= g_speak.text.count)
        return false;
    const SpeakSentence* s = &g_speak.text.sentences[g_speak.index];
    if (pos < s->start || pos >= s->end)
        return false;

    out->has_bg = true;
    out->bg = color_lerp(get_bg(), get_highlight_bg(), SPEAK_SENTENCE_TINT);
    out->has_fg = false;

    // The spoken byte that came from pos (syntax bytes in between have none: no glow on those)
    const uint32_t* map = g_speak.text.map + s->off;
    size_t lo = 0, hi = s->len;
    while (hi - lo > 1) {
        size_t mid = lo + (hi - lo) / 2;
        if (map[mid] <= pos)
            lo = mid;
        else
            hi = mid;
    }
    if (map[lo] != pos)
        return true;

    // The word around it, and how far the estimate is from that word
    const char* text = g_speak.text.text + s->off;
    if (text[lo] == ' ')
        return true;
    size_t ws = lo, we = lo + 1;
    while (ws > 0 && text[ws - 1] != ' ')
        ws--;
    while (we < s->len && text[we] != ' ')
        we++;
    float est = g_speak.est;
    float d = est < (float)ws ? (float)ws - est : est >= (float)we ? est - (float)we + 1.0f : 0.0f;
    float glow = 1.0f - d / SPEAK_GLOW_SPREAD;
    if (glow <= 0.0f)
        return true;
    out->has_fg = true;
    out->fg = color_lerp(get_fg(), get_accent(), glow);
    return true;
}

// #endregion
