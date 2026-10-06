// dawn_title.c

#include "dawn_title.h"

#if HAS_LIBAI

#include "dawn_chat.h"
#include "dawn_file.h"
#include "dawn_fm.h"
#include "dawn_gap.h"
#include "dawn_notice.h"
#include "dawn_session.h"
#include "dawn_utils.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

#define TITLE_FIRST_CHARS 160 //!< The first title comes once the note has this much text
#define TITLE_IDLE_MS 8000 //!< … and the writer paused this long
#define TITLE_EVERY_MS (2 * 60 * 1000) //!< At most one title this often per note
#define TITLE_MAX_PER_NOTE 5
#define TITLE_RETRY_MS 30000 //!< After a title job that failed or gave way
#define TITLE_REPLY_TOKENS 24
#define SLUG_MAX 60

// #region State

//! Live titles for the open note (in memory: a note reopened later starts counting again).
static struct {
    char* path; //!< The note this is about
    int32_t count; //!< Titles given this run
    int64_t last_ms; //!< When the last title was asked for (DAWN_CLOCK_MS), 0 = never
    size_t len_at_last; //!< The note's length then
    char heading_at_last[96]; //!< Its first heading then
    bool busy; //!< A title job is out
    int64_t retry_at;
} g_t;

//! The last live title, for Ctrl+Z right after it.
static struct {
    bool valid;
    char* path; //!< The note's path after the title
    char* old_path; //!< Its path before, when the file was renamed (NULL when not)
    char* old_title; //!< The title before (NULL: none)
    char* old_source; //!< title-source before (NULL: none)
    int32_t undo_pos, undo_count; //!< The undo stack then: any edit since means "not right after"
    size_t text_len;
} g_undo;

static void undo_forget(void)
{
    free(g_undo.path);
    free(g_undo.old_path);
    free(g_undo.old_title);
    free(g_undo.old_source);
    memset(&g_undo, 0, sizeof(g_undo));
}

static const char* note_title(void) { return app.frontmatter ? fm_get_string(app.frontmatter, "title") : NULL; }

static bool note_untitled(void)
{
    const char* title = note_title();
    return !title || !title[0] || strcmp(title, "Untitled") == 0;
}

static bool title_is_ai(void)
{
    const char* source = app.frontmatter ? fm_get_string(app.frontmatter, "title-source") : NULL;
    return source && strcmp(source, "ai") == 0;
}

// #endregion

// #region Reading the note

//! The note's first ATX heading's text (outside code fences), "" when it has none.
static void first_heading(char* out, size_t cap)
{
    out[0] = '\0';
    size_t len = gap_len(&app.text);
    bool fence = false;
    for (size_t pos = 0; pos < len;) {
        size_t p = pos;
        if (p + 3 <= len && (gap_at(&app.text, p) == '`' || gap_at(&app.text, p) == '~')
            && gap_at(&app.text, p + 1) == gap_at(&app.text, p) && gap_at(&app.text, p + 2) == gap_at(&app.text, p)) {
            fence = !fence;
        } else if (!fence && gap_at(&app.text, p) == '#') {
            size_t hashes = 0;
            while (p < len && gap_at(&app.text, p) == '#' && hashes < 7) {
                p++;
                hashes++;
            }
            if (hashes <= 6 && p < len && gap_at(&app.text, p) == ' ') {
                while (p < len && gap_at(&app.text, p) == ' ')
                    p++;
                size_t n = 0;
                while (p < len && gap_at(&app.text, p) != '\n' && n + 1 < cap)
                    out[n++] = gap_at(&app.text, p++);
                while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '#'))
                    n--;
                while (n > 0 && ((unsigned char)out[n - 1] & 0xC0) == 0x80)
                    n--;
                if (n > 0 && ((unsigned char)out[n - 1] & 0xC0) == 0xC0)
                    n--;
                out[n] = '\0';
                return;
            }
        }
        while (pos < len && gap_at(&app.text, pos) != '\n')
            pos++;
        pos++;
    }
}

//! Whether the cursor sits at the end of a sentence or a paragraph: nothing but spaces after it on
//! its line, and before it (past spaces) sentence-ending punctuation or a line break.
static bool at_sentence_end(void)
{
    size_t len = gap_len(&app.text);
    size_t p = app.cursor > len ? len : app.cursor;
    for (size_t q = p; q < len && gap_at(&app.text, q) != '\n'; q++)
        if (gap_at(&app.text, q) != ' ' && gap_at(&app.text, q) != '\t')
            return false;
    bool line_break = false;
    while (p > 0) {
        char c = gap_at(&app.text, p - 1);
        if (c == '\n')
            line_break = true;
        else if (c != ' ' && c != '\t')
            break;
        p--;
    }
    if (p == 0)
        return false;
    char c = gap_at(&app.text, p - 1);
    if (c == '.' || c == '!' || c == '?' || c == ':')
        return true;
    unsigned char b1 = p >= 2 ? (unsigned char)gap_at(&app.text, p - 2) : 0;
    unsigned char b2 = p >= 3 ? (unsigned char)gap_at(&app.text, p - 3) : 0;
    if (b2 == 0xE2 && b1 == 0x80 && (unsigned char)c == 0xA6) // …
        return true;
    if (b1 == 0xD8 && (unsigned char)c == 0x9F) // ؟
        return true;
    return line_break;
}

// #endregion

// #region Comparing and naming

//! title lower-cased to its words: ASCII letters and digits (and anything non-ASCII) kept, the
//! rest one space.
static void title_words(const char* title, char* out, size_t cap)
{
    size_t n = 0;
    bool space = true;
    for (const unsigned char* p = (const unsigned char*)title; *p && n + 1 < cap; p++) {
        if (isalnum(*p) || *p >= 0x80) {
            out[n++] = (char)tolower(*p);
            space = false;
        } else if (!space) {
            out[n++] = ' ';
            space = true;
        }
    }
    while (n > 0 && out[n - 1] == ' ')
        n--;
    out[n] = '\0';
}

//! Whether two titles are nearly the same: equal word for word, or a few letters apart.
static bool nearly_same(const char* a, const char* b)
{
    char wa[96], wb[96];
    title_words(a ? a : "", wa, sizeof(wa));
    title_words(b ? b : "", wb, sizeof(wb));
    if (strcmp(wa, wb) == 0)
        return true;
    size_t la = strlen(wa), lb = strlen(wb);
    // Levenshtein distance over two rows.
    int32_t prev[96], cur[96];
    for (size_t j = 0; j <= lb; j++)
        prev[j] = (int32_t)j;
    for (size_t i = 1; i <= la; i++) {
        cur[0] = (int32_t)i;
        for (size_t j = 1; j <= lb; j++) {
            int32_t cost = wa[i - 1] == wb[j - 1] ? 0 : 1;
            int32_t best = prev[j] + 1;
            if (cur[j - 1] + 1 < best)
                best = cur[j - 1] + 1;
            if (prev[j - 1] + cost < best)
                best = prev[j - 1] + cost;
            cur[j] = best;
        }
        memcpy(prev, cur, sizeof(int32_t) * (lb + 1));
    }
    size_t longest = la > lb ? la : lb;
    int32_t allowed = (int32_t)(longest / 6);
    if (allowed < 1)
        allowed = 1;
    return prev[lb] <= allowed;
}

//! The file name stem for title: ASCII letters and digits only, lower case, words joined by '-'.
//! "" when nothing of it is ASCII (the file then keeps its name).
static void title_slug(const char* title, char* out, size_t cap)
{
    size_t n = 0;
    bool dash = true;
    size_t limit = cap - 1 < SLUG_MAX ? cap - 1 : SLUG_MAX;
    for (const unsigned char* p = (const unsigned char*)title; *p && n < limit; p++) {
        if (*p < 0x80 && isalnum(*p)) {
            out[n++] = (char)tolower(*p);
            dash = false;
        } else if (!dash) {
            out[n++] = '-';
            dash = true;
        }
    }
    while (n > 0 && out[n - 1] == '-')
        n--;
    out[n] = '\0';
}

//! The file name part of path, and its length without ".md".
static const char* base_name(const char* path, size_t* stem_len)
{
    const char* slash = strrchr(path, '/');
    const char* bslash = strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash))
        slash = bslash;
    const char* base = slash ? slash + 1 : path;
    size_t n = strlen(base);
    if (n > 3 && strcmp(base + n - 3, ".md") == 0)
        n -= 3;
    *stem_len = n;
    return base;
}

//! Whether dawn named the note's file itself: in dawn's notes directory, and named either the way
//! a new note is (2026-09-28_143005.md) or after its current title the way a live title does
//! (eid-plans.md, eid-plans-2.md). A file the writer named stays as it is.
static bool file_named_by_dawn(void)
{
    if (!app.session_path || !note_in_history_dir(app.session_path))
        return false;
    size_t n;
    const char* base = base_name(app.session_path, &n);
    // YYYY-MM-DD_HHMMSS
    static const char pattern[] = "dddd-dd-dd_dddddd";
    bool stamp = n == sizeof(pattern) - 1;
    for (size_t i = 0; stamp && i < n; i++)
        stamp = pattern[i] == 'd' ? isdigit((unsigned char)base[i]) != 0 : base[i] == pattern[i];
    if (stamp)
        return true;
    const char* title = note_title();
    if (!title || note_untitled() || !title_is_ai())
        return false;
    char slug[SLUG_MAX + 1];
    title_slug(title, slug, sizeof(slug));
    size_t sl = strlen(slug);
    if (sl == 0 || n < sl || strncmp(base, slug, sl) != 0)
        return false;
    if (n == sl)
        return true;
    // slug-N
    if (base[sl] != '-' || n == sl + 1)
        return false;
    for (size_t i = sl + 1; i < n; i++)
        if (!isdigit((unsigned char)base[i]))
            return false;
    return true;
}

// #endregion

// #region Giving a title

//! Set the title as the AI's, rename the file when dawn named it, and say so.
static void apply_title(const char* title)
{
    undo_forget();
    const char* old = note_title();
    const char* source = app.frontmatter ? fm_get_string(app.frontmatter, "title-source") : NULL;
    g_undo.old_title = old ? dawn_strdup(old) : NULL;
    g_undo.old_source = source ? dawn_strdup(source) : NULL;
    bool rename = file_named_by_dawn(); // judged against the old title, before it changes

    if (!app.frontmatter)
        app.frontmatter = fm_create();
    fm_set_string(app.frontmatter, "title", title);
    fm_set_string(app.frontmatter, "title-source", "ai");
    DAWN_BACKEND(app)->set_title(title);
    app.dirty = true;

    if (rename) {
        char slug[SLUG_MAX + 1];
        title_slug(title, slug, sizeof(slug));
        char* old_path = app.session_path ? dawn_strdup(app.session_path) : NULL;
        char* new_path = slug[0] ? note_rename(slug) : NULL;
        if (new_path) {
            g_undo.old_path = old_path;
            old_path = NULL;
            free(g_t.path);
            g_t.path = dawn_strdup(new_path);
        }
        free(old_path);
        free(new_path);
    }
    save_session();

    g_undo.valid = true;
    g_undo.path = app.session_path ? dawn_strdup(app.session_path) : NULL;
    g_undo.undo_pos = app.undo_pos;
    g_undo.undo_count = app.undo_count;
    g_undo.text_len = gap_len(&app.text);

    char msg[128];
    snprintf(msg, sizeof(msg), "renamed · %s", title);
    notice_post(NOTICE_AI_CHANGE, msg);
}

//! Whether the model's reply is usable as a title: one line of 2 to 6 words, at most 60 bytes,
//! no echo of the prompt and not the current title. Surrounding quotes and a final period are
//! stripped into out first.
static bool title_valid(const char* reply, const char* current, char* out, size_t cap)
{
    while (isspace((unsigned char)*reply))
        reply++;
    size_t n = strlen(reply);
    while (n > 0 && isspace((unsigned char)reply[n - 1]))
        n--;
    static const char* const quotes[][2] = { { "\"", "\"" }, { "'", "'" },
        { "\xe2\x80\x9c", "\xe2\x80\x9d" }, { "\xe2\x80\x98", "\xe2\x80\x99" } };
    for (size_t i = 0; i < sizeof(quotes) / sizeof(quotes[0]); i++) {
        size_t a = strlen(quotes[i][0]), b = strlen(quotes[i][1]);
        if (n >= a + b && strncmp(reply, quotes[i][0], a) == 0 && strncmp(reply + n - b, quotes[i][1], b) == 0) {
            reply += a;
            n -= a + b;
            break;
        }
    }
    if (n > 0 && reply[n - 1] == '.')
        n--;
    if (n == 0 || n > 60 || n >= cap || memchr(reply, '\n', n) || memchr(reply, '\r', n))
        return false;
    memcpy(out, reply, n);
    out[n] = '\0';
    if (strncasecmp(out, "title:", 6) == 0 || strncmp(out, "(From dawn", 10) == 0)
        return false;
    size_t words = 0;
    bool in_word = false;
    for (const char* p = out; *p; p++) {
        bool space = isspace((unsigned char)*p);
        if (!space && !in_word)
            words++;
        in_word = !space;
    }
    if (words < 2 || words > 6)
        return false;
    return !current || strcmp(out, current) != 0;
}

static void title_done(const char* reply, void* user_data)
{
    (void)user_data;
    g_t.busy = false;
    int64_t now = DAWN_BACKEND(app)->clock(DAWN_CLOCK_MS);
    if (!reply) {
        g_t.retry_at = now + TITLE_RETRY_MS;
        return;
    }
    // Still the same note, and still a title the AI may change (the writer may have set one).
    if (!app.session_path || !g_t.path || strcmp(app.session_path, g_t.path) != 0)
        return;
    if (!note_untitled() && !title_is_ai())
        return;
    g_t.last_ms = now;
    g_t.len_at_last = gap_len(&app.text);
    first_heading(g_t.heading_at_last, sizeof(g_t.heading_at_last));

    char title[81];
    if (!title_valid(reply, note_untitled() ? NULL : note_title(), title, sizeof(title))) {
        g_t.retry_at = now + TITLE_RETRY_MS; // keep the old title; try again later
        return;
    }
    if (!note_untitled() && nearly_same(title, note_title()))
        return; // nearly the old one: dropped silently
    g_t.count++;
    apply_title(title);
}

void title_tick(void)
{
    if (!app.ai_ready || !app.ai_ctx || g_t.busy)
        return;
    if (app.mode != MODE_WRITING || app.preview_mode || !app.session_path)
        return;

    // A different note: start counting for it; its current shape is the reference.
    if (!g_t.path || strcmp(g_t.path, app.session_path) != 0) {
        free(g_t.path);
        g_t.path = dawn_strdup(app.session_path);
        g_t.count = 0;
        g_t.last_ms = 0;
        g_t.retry_at = 0;
        g_t.len_at_last = gap_len(&app.text);
        first_heading(g_t.heading_at_last, sizeof(g_t.heading_at_last));
        if (g_undo.valid && (!g_undo.path || strcmp(g_undo.path, app.session_path) != 0))
            undo_forget();
    }

    // Only dawn's own kind of note (a file that came without frontmatter stays as it is), and only
    // a title the AI wrote or none yet.
    if (!app.write_fm)
        return;
    bool untitled = note_untitled();
    if (!untitled && !title_is_ai())
        return;
    if (g_t.count >= TITLE_MAX_PER_NOTE)
        return;

    int64_t now = DAWN_BACKEND(app)->clock(DAWN_CLOCK_MS);
    if (now < g_t.retry_at || (g_t.last_ms && now - g_t.last_ms < TITLE_EVERY_MS))
        return;
    size_t len = gap_len(&app.text);
    if (untitled) {
        if (len < TITLE_FIRST_CHARS)
            return;
    } else {
        // The note changed shape: its first heading changed, or it grew by half.
        char heading[96];
        first_heading(heading, sizeof(heading));
        bool reshaped = strcmp(heading, g_t.heading_at_last) != 0 || len * 2 >= g_t.len_at_last * 3;
        if (!reshaped)
            return;
    }
    if (session_idle_ms() < TITLE_IDLE_MS || !at_sentence_end() || !session_quiet_ready())
        return;

    char instruction[400];
    if (untitled)
        snprintf(instruction, sizeof(instruction),
            "(From dawn, not typed by the user.) Give the note a title: two to six words, in the note's "
            "language, with no quotes, no Markdown and no final period. Reply with only the title.");
    else
        snprintf(instruction, sizeof(instruction),
            "(From dawn, not typed by the user.) The note is titled \"%.80s\". Give it the title that fits "
            "it now (the same one if it still fits): two to six words, in the note's language, with no "
            "quotes, no Markdown and no final period. Reply with only the title.",
            note_title());
    g_t.busy = session_quiet(instruction, TITLE_REPLY_TOKENS, title_done, NULL);
    if (g_t.busy)
        g_t.last_ms = now; // asked: the next one waits its two minutes whatever comes back
}

// #endregion

// #region Undo and the writer's own titles

bool title_undo(void)
{
    if (!g_undo.valid)
        return false;
    bool right_after = app.session_path && g_undo.path && strcmp(app.session_path, g_undo.path) == 0
        && app.undo_pos == g_undo.undo_pos && app.undo_count == g_undo.undo_count
        && gap_len(&app.text) == g_undo.text_len;
    if (!right_after) {
        undo_forget();
        return false;
    }
    if (!app.frontmatter)
        app.frontmatter = fm_create();
    if (g_undo.old_title)
        fm_set_string(app.frontmatter, "title", g_undo.old_title);
    else
        fm_remove(app.frontmatter, "title");
    if (g_undo.old_source)
        fm_set_string(app.frontmatter, "title-source", g_undo.old_source);
    else
        fm_remove(app.frontmatter, "title-source");
    if (g_undo.old_path && note_rename_to(g_undo.old_path)) {
        free(g_t.path);
        g_t.path = dawn_strdup(app.session_path);
    }
    DAWN_BACKEND(app)->set_title(g_undo.old_title ? g_undo.old_title : "Dawn");
    app.dirty = true;
    save_session();
    // Undone right away reads as "not that": no more live titles for this note this time.
    g_t.count = TITLE_MAX_PER_NOTE;
    char msg[128];
    snprintf(msg, sizeof(msg), "title back · %s", g_undo.old_title ? g_undo.old_title : "untitled");
    notice_post(NOTICE_INFO, msg);
    undo_forget();
    return true;
}

void title_user_edited(void)
{
    if (app.frontmatter)
        fm_remove(app.frontmatter, "title-source");
    undo_forget();
}

// #endregion

#endif // HAS_LIBAI
