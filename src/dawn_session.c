// dawn_session.c

#include "dawn_session.h"

#if HAS_LIBAI

#include "ai.h"
#include "dawn_ai_queue.h"
#include "dawn_ai_tokens.h"
#include "dawn_chat.h"
#include "dawn_fm.h"
#include "dawn_gap.h"
#include "dawn_nav.h"
#include "dawn_notice.h"
#include "dawn_utils.h"

#include <stdint.h>
#include <string.h>
#include <time.h>

// #region Tunables

#define COMPACT_AT 0.70 //!< Plan a compaction once the conversation holds this share of the window
#define QUIET_IDLE_MS 8000 //!< A quiet moment: this long without typing and since the last reply
#define PRIME_IDLE_MS 2000 //!< Priming waits for the note to settle this long after typing
#define PRIME_TYPED_EDITS 8 //!< "Typed for a moment": this many frames with an edit since opening
#define PRIME_RETRY_MS 60000 //!< After a failed prime, wait this long before priming on our own
#define COMPACT_RETRY_MS 30000 //!< After a failed or displaced compaction, wait this long
//! The prime's reply limit. The spec asks for 1, but TAI closes any conversation whose reply hit
//! max_tokens (LiteRtTaiRuntime.java:460: lengthLimited), which would throw the primed cache away
//! at once; asking for a one-word reply with a little room lets the reply end on its own.
#define PRIME_REPLY_TOKENS 8
#define SUMMARY_REPLY_TOKENS 64
#define CHAT_REPLY_TOKENS 768 //!< Room kept for the next chat reply when budgeting a snapshot
#define KEEP_WARM_EVERY_MS (10 * 60 * 1000)
#define KEEP_WARM_MINUTES 15
#define LIVE_WITHIN_MS (30 * 60 * 1000) //!< The session is live while the writer did something this recently
#define REQUEST_MARGIN 48 //!< Tokens of slack per request for the chat template's own formatting
#define MAX_SECTIONS 256
#define MAX_REPLY_BYTES (64 * 1024)
//! One temperature for every request: TAI's reuse key includes it (optionsKey), so a title asked
//! at a different temperature than the chat would start a new conversation every time.
#define SESSION_TEMPERATURE 0.6

// #endregion

// #region Small helpers

typedef struct {
    char* data;
    size_t len;
    size_t cap;
} sbuf_t;

static void sb_add(sbuf_t* b, const char* s, size_t n)
{
    if (!s || n == 0)
        return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (b->len + n + 1 > cap)
            cap *= 2;
        char* grown = realloc(b->data, cap);
        if (!grown)
            return;
        b->data = grown;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void sb_str(sbuf_t* b, const char* s) { sb_add(b, s, s ? strlen(s) : 0); }

//! The buffer's text, never NULL (an empty buffer gives ""); the buffer is emptied.
static char* sb_take(sbuf_t* b)
{
    char* out = b->data ? b->data : dawn_strdup("");
    b->data = NULL;
    b->len = b->cap = 0;
    return out;
}

static int64_t now_ms(void) { return DAWN_BACKEND(app)->clock(DAWN_CLOCK_MS); }

//! Wall-clock milliseconds, to compare with the bridge's runtime check times (CLOCK_REALTIME).
static int64_t wall_ms(void) { return (int64_t)time(NULL) * 1000; }

static bool same_path(const char* a, const char* b)
{
    if (!a || !b)
        return a == b;
    return strcmp(a, b) == 0;
}

static uint64_t fnv_start(void) { return 1469598103934665603ULL; }

static uint64_t fnv_byte(uint64_t h, unsigned char c) { return (h ^ c) * 1099511628211ULL; }

// #endregion

// #region Sections and the "since last time" diff

//! One section of the note: from a heading line (or the start) to the next heading.
typedef struct {
    size_t start, end; //!< [start, end) in the note, the heading line included
    uint64_t key; //!< The heading's text and how many headings with that text came before
    uint64_t body; //!< Everything in the section
    char heading[96]; //!< The heading's text, "" for the part before the first heading
} NoteSection;

//! What the model knows of one section: seen means it has this section's text as hashed.
typedef struct {
    uint64_t key;
    uint64_t body;
    bool seen;
} KnownSection;

typedef struct {
    KnownSection* items;
    int32_t count;
    char* title; //!< The note's title as the model last heard it (NULL = none)
    bool valid; //!< False until a snapshot went out
} Baseline;

static void baseline_free(Baseline* b)
{
    free(b->items);
    free(b->title);
    memset(b, 0, sizeof(*b));
}

static const KnownSection* baseline_find(const Baseline* b, uint64_t key)
{
    for (int32_t i = 0; i < b->count; i++)
        if (b->items[i].key == key)
            return &b->items[i];
    return NULL;
}

//! Whether the line at pos starts an ATX heading: 1-6 '#' then a space.
static bool line_is_heading(const GapBuffer* gb, size_t pos, size_t len)
{
    size_t n = 0;
    while (pos + n < len && n < 7 && gap_at(gb, pos + n) == '#')
        n++;
    return n >= 1 && n <= 6 && pos + n < len && gap_at(gb, pos + n) == ' ';
}

static bool line_is_fence(const GapBuffer* gb, size_t pos, size_t len)
{
    size_t p = pos;
    while (p < len && p - pos < 3 && gap_at(gb, p) == ' ')
        p++;
    if (p + 3 > len)
        return false;
    char c = gap_at(gb, p);
    return (c == '`' || c == '~') && gap_at(gb, p + 1) == c && gap_at(gb, p + 2) == c;
}

static void section_heading_text(const GapBuffer* gb, size_t pos, size_t len, char* out, size_t cap)
{
    while (pos < len && gap_at(gb, pos) == '#')
        pos++;
    while (pos < len && gap_at(gb, pos) == ' ')
        pos++;
    size_t n = 0;
    while (pos < len && gap_at(gb, pos) != '\n' && n + 1 < cap)
        out[n++] = gap_at(gb, pos++);
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '#' || out[n - 1] == '\r'))
        n--;
    // Never end in the middle of a UTF-8 character.
    while (n > 0 && ((unsigned char)out[n - 1] & 0xC0) == 0x80)
        n--;
    if (n > 0 && ((unsigned char)out[n - 1] & 0xC0) == 0xC0)
        n--;
    out[n] = '\0';
}

//! Split the note into sections at its headings (outside code fences). Returns how many were
//! written to out; a note longer than max sections folds the rest into the last one.
static int32_t split_sections(const GapBuffer* gb, NoteSection* out, int32_t max)
{
    size_t len = gap_len(gb);
    int32_t count = 0;
    bool in_fence = false;
    size_t section_start = 0;
    bool have_first = false;

    for (size_t pos = 0; pos <= len;) {
        size_t eol = pos;
        while (eol < len && gap_at(gb, eol) != '\n')
            eol++;
        bool heading = false;
        if (pos < len) {
            if (line_is_fence(gb, pos, len))
                in_fence = !in_fence;
            else if (!in_fence && line_is_heading(gb, pos, len))
                heading = true;
        }
        if ((heading || pos >= len) && count < max) {
            // Close the section before this heading (or the end), unless it's an empty preamble.
            if (have_first || pos > section_start) {
                out[count].start = section_start;
                out[count].end = pos;
                count++;
            }
            section_start = pos;
            have_first = true;
            if (count >= max)
                break;
        }
        if (pos >= len)
            break;
        pos = eol + 1;
    }
    if (count == 0 || out[count - 1].end < len) {
        if (count < max) {
            out[count].start = section_start < len ? section_start : len;
            count++;
        }
        out[count - 1].end = len;
    }

    // Hash each one, and key it by its heading's text and occurrence.
    for (int32_t i = 0; i < count; i++) {
        NoteSection* s = &out[i];
        s->heading[0] = '\0';
        if (s->start < len && line_is_heading(gb, s->start, len))
            section_heading_text(gb, s->start, len, s->heading, sizeof(s->heading));
        uint64_t h = fnv_start();
        for (size_t p = s->start; p < s->end; p++)
            h = fnv_byte(h, (unsigned char)gap_at(gb, p));
        s->body = h;
        int32_t occurrence = 0;
        for (int32_t j = 0; j < i; j++)
            if (strcmp(out[j].heading, s->heading) == 0)
                occurrence++;
        uint64_t k = fnv_start();
        for (const char* c = s->heading; *c; c++)
            k = fnv_byte(k, (unsigned char)*c);
        k = fnv_byte(k, 0);
        k = fnv_byte(k, (unsigned char)(occurrence & 0xFF));
        k = fnv_byte(k, (unsigned char)((occurrence >> 8) & 0xFF));
        s->key = k;
    }
    return count;
}

static bool section_holds(const NoteSection* s, size_t pos, bool last)
{
    return pos >= s->start && (pos < s->end || (last && pos == s->end));
}

//! The baseline right after a snapshot: everything seen when the whole note went out, otherwise
//! only the section around the cursor (the snapshot's anchor).
static void baseline_from_snapshot(Baseline* b, const NoteSection* secs, int32_t n, bool whole)
{
    baseline_free(b);
    b->items = calloc((size_t)(n > 0 ? n : 1), sizeof(KnownSection));
    if (!b->items)
        return;
    b->count = n;
    for (int32_t i = 0; i < n; i++) {
        b->items[i].key = secs[i].key;
        b->items[i].body = secs[i].body;
        b->items[i].seen = whole || section_holds(&secs[i], app.cursor, i == n - 1);
    }
    const char* title = app.frontmatter ? fm_get_string(app.frontmatter, "title") : NULL;
    b->title = title ? dawn_strdup(title) : NULL;
    b->valid = true;
}

static bool baseline_all_seen(const Baseline* b)
{
    for (int32_t i = 0; i < b->count; i++)
        if (!b->items[i].seen)
            return false;
    return true;
}

//! [start, end) of the note, cut to at most max_bytes on a UTF-8 boundary, with "…" when cut.
static void add_note_range(sbuf_t* out, size_t start, size_t end, size_t max_bytes)
{
    bool cut = end - start > max_bytes;
    if (cut) {
        end = start + max_bytes;
        while (end > start && ((unsigned char)gap_at(&app.text, end) & 0xC0) == 0x80)
            end--;
    }
    char* text = gap_substr(&app.text, start, end);
    if (text) {
        size_t n = strlen(text);
        while (n > 0 && (text[n - 1] == '\n' || text[n - 1] == ' '))
            n--;
        sb_add(out, text, n);
        if (cut)
            sb_str(out, "…");
        sb_str(out, "\n");
    }
    free(text);
}

//! What changed in the note since the model last saw it, for one appended turn: sections whose
//! text changed, new sections, the section around the cursor if the model never saw it, removed
//! headings and a changed title. Kept within budget_tokens: when everything does not fit, only the
//! section around the cursor is shown (cut to fit) and the others are named. Returns NULL when
//! nothing changed. *next receives the baseline to adopt once the turn succeeds.
static char* build_diff(const Baseline* base, int32_t budget_tokens, Baseline* next)
{
    NoteSection* secs = malloc(sizeof(NoteSection) * MAX_SECTIONS);
    if (!secs)
        return NULL;
    int32_t n = split_sections(&app.text, secs, MAX_SECTIONS);
    size_t len = gap_len(&app.text);

    enum { SAME, SHOW, NAME_CHANGED, NAME_NEW, UNSEEN };
    uint8_t* st = calloc((size_t)(n > 0 ? n : 1), 1);
    baseline_free(next);
    next->items = calloc((size_t)(n > 0 ? n : 1), sizeof(KnownSection));
    if (!st || !next->items) {
        free(st);
        free(secs);
        free(next->items);
        next->items = NULL;
        return NULL;
    }
    next->count = n;
    next->valid = true;

    bool had_whole = baseline_all_seen(base);
    int32_t cursor_section = -1;
    int64_t show_bytes = 0;
    for (int32_t i = 0; i < n; i++) {
        const KnownSection* k = baseline_find(base, secs[i].key);
        bool at_cursor = section_holds(&secs[i], app.cursor, i == n - 1);
        if (at_cursor)
            cursor_section = i;
        if (k && k->seen && k->body == secs[i].body)
            st[i] = SAME;
        else if ((k && k->seen) || (!k && had_whole) || at_cursor)
            st[i] = SHOW;
        else if (!k)
            st[i] = NAME_NEW;
        else
            st[i] = UNSEEN;
        if (st[i] == SHOW)
            show_bytes += (int64_t)(secs[i].end - secs[i].start);
    }

    // Tokens to bytes, on the safe side: chars/3.6 is the estimate, so 3 bytes a token undercounts.
    int64_t budget_bytes = (int64_t)(budget_tokens > 0 ? budget_tokens : 0) * 3;
    bool squeeze = show_bytes > budget_bytes;
    if (squeeze)
        for (int32_t i = 0; i < n; i++)
            if (st[i] == SHOW && i != cursor_section)
                st[i] = NAME_CHANGED;

    sbuf_t out = { 0 };
    bool any = false;
    for (int32_t i = 0; i < n; i++) {
        next->items[i].key = secs[i].key;
        next->items[i].body = secs[i].body;
        const KnownSection* k = baseline_find(base, secs[i].key);
        switch (st[i]) {
        case SAME:
            next->items[i].seen = true;
            break;
        case SHOW: {
            if (!any)
                sb_str(&out, "What changed in the note since you last saw it:\n");
            any = true;
            char open[160];
            snprintf(open, sizeof(open), "<section%s%s%s%s>\n", secs[i].heading[0] ? " \"" : "",
                secs[i].heading, secs[i].heading[0] ? "\"" : "", k ? "" : " new");
            sb_str(&out, open);
            size_t max_bytes = squeeze ? (size_t)(budget_bytes > 64 ? budget_bytes - 64 : 64)
                                       : secs[i].end - secs[i].start;
            add_note_range(&out, secs[i].start, secs[i].end, max_bytes);
            sb_str(&out, "</section>\n");
            // Cut to fit means only part of it was seen.
            next->items[i].seen = !squeeze || (secs[i].end - secs[i].start) <= max_bytes;
            break;
        }
        default:
            next->items[i].seen = false;
            break;
        }
    }

    // Named but not shown: changed sections squeezed out, new ones in a note seen only in part.
    const char* labels[2] = { "Also changed, not shown:", "New sections, not shown:" };
    const uint8_t kinds[2] = { NAME_CHANGED, NAME_NEW };
    for (int32_t l = 0; l < 2; l++) {
        bool first = true;
        for (int32_t i = 0; i < n; i++) {
            if (st[i] != kinds[l])
                continue;
            if (first) {
                if (!any)
                    sb_str(&out, "What changed in the note since you last saw it:\n");
                any = true;
                sb_str(&out, labels[l]);
                first = false;
            } else {
                sb_str(&out, ",");
            }
            sb_str(&out, " \"");
            sb_str(&out, secs[i].heading[0] ? secs[i].heading : "(the start of the note)");
            sb_str(&out, "\"");
        }
        if (!first)
            sb_str(&out, ".\n");
    }

    // Removed: sections the model had seen whose heading is gone. Only their number is known here
    // (the baseline keeps hashes, not text); the model has the headings from before.
    int32_t removed = 0;
    for (int32_t j = 0; j < base->count; j++) {
        if (!base->items[j].seen)
            continue;
        bool present = false;
        for (int32_t i = 0; i < n && !present; i++)
            present = secs[i].key == base->items[j].key;
        if (!present)
            removed++;
    }
    if (removed > 0) {
        if (!any)
            sb_str(&out, "What changed in the note since you last saw it:\n");
        any = true;
        char line[96];
        if (removed == 1)
            snprintf(line, sizeof(line), "A section you saw was removed or renamed.\n");
        else
            snprintf(line, sizeof(line), "%d sections you saw were removed or renamed.\n", (int)removed);
        sb_str(&out, line);
    }

    const char* title = app.frontmatter ? fm_get_string(app.frontmatter, "title") : NULL;
    next->title = title ? dawn_strdup(title) : NULL;
    if (title && title[0] && strcmp(title, "Untitled") != 0 && (!base->title || strcmp(base->title, title) != 0)) {
        char line[200];
        snprintf(line, sizeof(line), "The note's title is now \"%s\".\n", title);
        sb_str(&out, line);
        any = true;
    }
    if (len == 0 && base->count > 0) {
        sb_str(&out, "The note is now empty.\n");
        any = true;
    }

    free(st);
    free(secs);
    if (!any) {
        free(out.data);
        return NULL;
    }
    return sb_take(&out);
}

// #endregion

// #region Snapshot

//! The note as it is now for a whole snapshot, sized to budget_tokens: its title, what is and
//! isn't shown, and ai_note_snapshot()'s selection → section → outline → neighbours. *whole says
//! whether nothing was left out; *base (when given) becomes the matching baseline.
static char* note_context_budget(int32_t budget_tokens, bool* whole, Baseline* base)
{
    const char* title = app.frontmatter ? fm_get_string(app.frontmatter, "title") : NULL;
    char title_line[320] = "";
    if (title && title[0] && strcmp(title, "Untitled") != 0)
        snprintf(title_line, sizeof(title_line), "Its title is \"%s\".\n", title);

    NoteSection* secs = base ? malloc(sizeof(NoteSection) * MAX_SECTIONS) : NULL;
    int32_t n = secs ? split_sections(&app.text, secs, MAX_SECTIONS) : 0;

    if (gap_len(&app.text) == 0) {
        if (whole)
            *whole = true;
        if (base)
            baseline_from_snapshot(base, secs, n, true);
        free(secs);
        return dawn_strdup("The user's note is empty so far.");
    }

    if (budget_tokens < 128)
        budget_tokens = 128;
    size_t s, e;
    get_selection(&s, &e);
    AiSnapshotInfo info;
    char* snapshot = ai_note_snapshot(&app.text, app.block_cache, app.cursor, s, e, budget_tokens, &info);
    if (whole)
        *whole = info.whole_note;
    if (base)
        baseline_from_snapshot(base, secs, n, info.whole_note);
    free(secs);

    char explainer[256] = "";
    if (!info.whole_note) {
        if (info.section_heading[0])
            snprintf(explainer, sizeof(explainer),
                "You see the outline and the section \"%s\"; the rest of the note is not shown.\n",
                info.section_heading);
        else
            snprintf(explainer, sizeof(explainer),
                "You see the outline and the part around the cursor; the rest is not shown.\n");
    }

    sbuf_t out = { 0 };
    sb_str(&out, "The user's open note is below. \"This\", \"the note\" and \"the document\" mean it.\n");
    sb_str(&out, title_line);
    sb_str(&out, explainer);
    sb_str(&out, "<note>\n");
    sb_str(&out, snapshot);
    sb_str(&out, "\n</note>");
    free(snapshot);
    return sb_take(&out);
}

char* session_note_context(void)
{
    return note_context_budget((int32_t)(ai_ctx_window() * 0.55), NULL, NULL);
}

// #endregion

// #region State

typedef enum { JOB_NONE, JOB_PRIME, JOB_USER, JOB_QUIET, JOB_SUMMARY, JOB_REBUILD } JobKind;

//! The one job in flight. Everything dawn asks goes through here, one at a time.
static struct {
    JobKind kind;
    uint32_t token; //!< Passed as the stream's user_data; a chunk with another token is not ours
    ai_session_id_t conv; //!< The conversation it runs on
    ai_stream_id_t stream;
    int32_t est_message; //!< Estimated tokens of the message sent (for calibration and accounting)
    int32_t conv_before; //!< The conversation's size before this turn
    Baseline pending; //!< What the model will know of the note once this turn succeeds
    bool whole; //!< The model will have seen the whole note once this turn succeeds
    bool cancelled; //!< Displaced (a question came) or stopped: its end is not a success
    bool orphaned; //!< The conversation was reset under it: its end changes nothing
    bool got_output;
    int64_t started_ms;
    int64_t started_wall_ms;
    int64_t waking_since; //!< The model was asleep when this started (DAWN_CLOCK_MS), else 0
    bool reading; //!< It carries a whole snapshot the writer is waiting on ("reading the note…")
    sbuf_t reply;
    char error[64]; //!< The first "Error: …" chunk, "" when none
    int32_t held_from; //!< JOB_USER on a fresh conversation / JOB_REBUILD: the new session_held_from()
    SessionQuietDone done; //!< JOB_QUIET
    void* done_data;
} g_job;

static uint32_t g_next_token = 1;

//! The user's question, from session_ask() until its NULL chunk reached cb.
static struct {
    bool active;
    char* question;
    int32_t max_tokens;
    ai_stream_callback_t cb;
    int64_t start_at_ms; //!< Waiting out TAI's 409 backoff until then; 0 = as soon as possible
    bool waiting_busy; //!< In that backoff now
    bool live; //!< Its stream is running
    int32_t chat_index; //!< The chat message of the question
} g_user;

//! The conversation dawn holds. It is app.ai_session; g_conv_id notices when that changed under us.
static ai_session_id_t g_conv_id;
static int32_t g_conv_tokens; //!< Its size in tokens after the last successful turn (0 = empty)
static bool g_conv_tokens_exact; //!< That size came from usage, not an estimate
static bool g_primed; //!< It holds a snapshot of the note
static bool g_cache_lost; //!< TAI most likely dropped its KV cache (another app, an unload)
static char g_conv_model[128]; //!< The model it was built on, once known
static char* g_conv_path; //!< The note it is about
static Baseline g_base; //!< What the model knows of the note
static bool g_whole; //!< The model has seen the whole note as it is in g_base
static int32_t g_held_from; //!< session_held_from()

//! The last two question/answer exchanges, for a compaction's recap, and how many there were.
typedef struct {
    char* question;
    char* answer;
    int32_t chat_index;
} Exchange;
static Exchange g_log[2];
static int32_t g_log_count;
static int32_t g_exchanges_total;
static char* g_summary; //!< The model's one-line summary of what came before the recap

// Activity and timing
static size_t g_seen_len, g_seen_cursor, g_seen_input_len;
static bool g_seen_valid;
static int32_t g_typed_edits;
static int64_t g_last_activity_ms;
static int64_t g_last_job_end_ms;
static int64_t g_last_job_end_wall_ms;
static int64_t g_prime_retry_at;
static int64_t g_compact_retry_at;
static int64_t g_last_keep_warm_ms;
static bool g_rebuild_after_summary;
static bool g_prime_wanted; //!< The chat opened: prime even without typing, loading the model if needed

// What the header says beyond the job in flight
static char g_no_memory[128]; //!< The model TAI couldn't load for lack of memory ("" = none)
static int64_t g_busy_seen_ms; //!< When TAI last said another generation holds the model

//! The last runtime answer, polled from session_tick() (every frame while something is going on,
//! every 10 s otherwise), so nothing else has to ask the bridge per frame.
static ai_runtime_info_t g_info;
static int64_t g_info_polled_ms;

//! The model requests go to, re-read from ai.json / state.json every couple of seconds.
static char g_model_id[128];
static bool g_model_pinned;
static int64_t g_model_read_ms;

static int32_t g_system_tokens; //!< Estimated size of the system prompt and tools

static int32_t system_tokens(void)
{
    if (g_system_tokens == 0)
        g_system_tokens = ai_estimate_tokens(ai_system_prompt()) + ai_estimate_tokens(ai_tools_json()) + 16;
    return g_system_tokens;
}

//! Forget everything about the conversation (not the job in flight).
static void conv_forget(void)
{
    g_conv_tokens = 0;
    g_conv_tokens_exact = false;
    g_primed = false;
    g_cache_lost = false;
    g_conv_model[0] = '\0';
    g_whole = false;
    baseline_free(&g_base);
}

static void log_clear(void)
{
    for (int32_t i = 0; i < 2; i++) {
        free(g_log[i].question);
        free(g_log[i].answer);
        memset(&g_log[i], 0, sizeof(g_log[i]));
    }
    g_log_count = 0;
    g_exchanges_total = 0;
    free(g_summary);
    g_summary = NULL;
}

static void log_push(const char* question, const char* answer, int32_t chat_index)
{
    if (g_log_count == 2) {
        free(g_log[0].question);
        free(g_log[0].answer);
        g_log[0] = g_log[1];
        g_log_count = 1;
    }
    g_log[g_log_count].question = dawn_strdup(question ? question : "");
    g_log[g_log_count].answer = dawn_strdup(answer ? answer : "");
    g_log[g_log_count].chat_index = chat_index;
    g_log_count++;
    g_exchanges_total++;
}

//! The conversation to use, created when there is none. A conversation that is not the one we
//! built on (ai_init_session() made a new one) starts from empty accounting.
static ai_session_id_t conv_current(void)
{
    if (!app.ai_session)
        app.ai_session = ai_new_conversation();
    if (app.ai_session != g_conv_id) {
        g_conv_id = app.ai_session;
        conv_forget();
    }
    return app.ai_session;
}

//! Drop the current conversation and start an empty one (another model, or a question that no
//! longer fits the old one).
static ai_session_id_t conv_replace(void)
{
    if (app.ai_session)
        ai_destroy_session(app.ai_ctx, app.ai_session);
    app.ai_session = 0;
    return conv_current();
}

static bool model_loaded_now(ai_runtime_info_t* info)
{
    *info = g_info;
    return info->state == AI_MODEL_LOADED && !info->loading;
}

//! Refresh g_info: every frame when urgent (the chat is open, something runs), else every 10 s.
static void runtime_poll(bool urgent)
{
    int64_t now = now_ms();
    if (!urgent && g_info_polled_ms && now - g_info_polled_ms < 10000)
        return;
    ai_runtime_info(&g_info);
    g_info_polled_ms = now;
}

static void model_refresh(void)
{
    int64_t now = now_ms();
    if (g_model_read_ms && now - g_model_read_ms < 2000)
        return;
    g_model_read_ms = now;
    if (!ai_active_model(g_model_id, sizeof(g_model_id), &g_model_pinned))
        g_model_id[0] = '\0';
}

const char* session_model_id(void)
{
    model_refresh();
    if (g_model_id[0])
        return g_model_id;
    return g_info.loaded_model;
}

// #endregion

// #region Running jobs

static void session_stream_cb(ai_context_t* context, const char* chunk, void* user_data);

//! Start job kind on conv with message; the rest of g_job (pending, held_from, done) is set by
//! the caller beforehand. Returns false when the request could not start.
static bool job_start(JobKind kind, ai_session_id_t conv, const char* message, int32_t max_tokens)
{
    ai_runtime_info(&g_info);
    bool loaded = g_info.state == AI_MODEL_LOADED && !g_info.loading;

    g_job.kind = kind;
    g_job.token = g_next_token++;
    if (g_job.token == 0)
        g_job.token = g_next_token++;
    g_job.conv = conv;
    g_job.cancelled = false;
    g_job.orphaned = false;
    g_job.got_output = false;
    g_job.error[0] = '\0';
    g_job.started_ms = now_ms();
    g_job.started_wall_ms = wall_ms();
    g_job.waking_since = loaded ? 0 : g_job.started_ms;
    g_job.conv_before = g_conv_tokens;
    g_job.est_message = ai_estimate_tokens(message) + (g_conv_tokens == 0 ? system_tokens() : 0);
    free(g_job.reply.data);
    memset(&g_job.reply, 0, sizeof(g_job.reply));

    ai_generation_params_t params = {
        .temperature = SESSION_TEMPERATURE,
        .max_tokens = max_tokens,
        .include_reasoning = false,
        .seed = 0,
    };
    g_job.stream = ai_generate_response_stream(app.ai_ctx, conv, message, &params, session_stream_cb,
        (void*)(uintptr_t)g_job.token);
    if (g_job.stream == AI_INVALID_ID) {
        g_job.kind = JOB_NONE;
        baseline_free(&g_job.pending);
        return false;
    }
    ai_queue_set_lane(kind == JOB_USER || kind == JOB_PRIME ? AI_LANE_USER : AI_LANE_QUIET);
    return true;
}

//! Cancel the job in flight on the server; its NULL chunk still comes and ends it.
static void job_cancel(void)
{
    if (g_job.kind == JOB_NONE || g_job.cancelled)
        return;
    g_job.cancelled = true;
    if (g_job.stream != AI_INVALID_ID)
        ai_cancel_stream(app.ai_ctx, g_job.stream);
}

//! The question's cb, called once more with chunk and cleared when chunk is NULL.
static void user_deliver(const char* chunk)
{
    ai_stream_callback_t cb = g_user.cb;
    if (!chunk) {
        free(g_user.question);
        memset(&g_user, 0, sizeof(g_user));
        app.ai_stream = AI_INVALID_ID;
    }
    if (cb)
        cb(app.ai_ctx, chunk, NULL);
}

//! Edit blocks' bodies cut to a stub, for a recap: the note carries their result already.
static char* recap_answer(const char* answer)
{
    static const char* const tags[] = { "replace_note", "replace_selection", "insert_at_cursor", "append_to_note" };
    char* out = dawn_strdup(answer ? answer : "");
    for (size_t t = 0; out && t < sizeof(tags) / sizeof(tags[0]); t++) {
        char open[40], close[40];
        snprintf(open, sizeof(open), "<%s>", tags[t]);
        snprintf(close, sizeof(close), "</%s>", tags[t]);
        char* from = out;
        char* at;
        while ((at = strstr(from, open))) {
            char* body = at + strlen(open);
            char* end = strstr(body, close);
            if (!end)
                break;
            static const char stub[] = "(made in the note)";
            size_t stub_len = sizeof(stub) - 1;
            if ((size_t)(end - body) <= stub_len) {
                from = end + strlen(close);
                continue;
            }
            memcpy(body, stub, stub_len);
            memmove(body + stub_len, end, strlen(end) + 1);
            from = body + stub_len + strlen(close);
        }
    }
    return out;
}

//! Adopt a successful turn: the conversation's new size, what the model now knows of the note.
static void commit_turn(int32_t prompt_tokens, int32_t completion_tokens, bool have_usage)
{
    if (have_usage && prompt_tokens > 0) {
        // The new message's true size is what the prompt grew by; calibrate the estimate on it.
        int32_t grew = prompt_tokens - (g_job.conv_before > 0 && g_conv_tokens_exact ? g_job.conv_before : 0);
        if (grew > 0 && (g_job.conv_before == 0 || g_conv_tokens_exact))
            ai_calibrate_estimate(g_job.est_message, grew);
        g_conv_tokens = prompt_tokens + (completion_tokens > 0 ? completion_tokens : 0);
        g_conv_tokens_exact = true;
    } else {
        g_conv_tokens = g_job.conv_before + g_job.est_message + ai_estimate_tokens(g_job.reply.data) + REQUEST_MARGIN;
        g_conv_tokens_exact = false;
    }
    if (g_job.pending.valid) {
        baseline_free(&g_base);
        g_base = g_job.pending;
        memset(&g_job.pending, 0, sizeof(g_job.pending));
        g_whole = g_job.whole;
        g_primed = true;
    }
    g_cache_lost = false;
}

static void pump_user(void);

//! The job's NULL chunk arrived: settle it.
static void job_finish(void)
{
    JobKind kind = g_job.kind;
    int32_t prompt_tokens = 0, completion_tokens = 0;
    bool have_usage = ai_take_usage(&prompt_tokens, &completion_tokens);
    bool ok = !g_job.cancelled && !g_job.orphaned && g_job.error[0] == '\0';
    g_job.kind = JOB_NONE;
    g_job.stream = AI_INVALID_ID;
    ai_queue_set_lane(AI_LANE_NONE);
    g_last_job_end_ms = now_ms();
    g_last_job_end_wall_ms = wall_ms();
    if (!g_job.cancelled && !g_job.orphaned) {
        if (strcmp(g_job.error, "Error: insufficient_memory") == 0)
            snprintf(g_no_memory, sizeof(g_no_memory), "%s", session_model_id());
        else if (strcmp(g_job.error, "Error: generation_active") == 0)
            g_busy_seen_ms = g_last_job_end_ms;
        else if (!g_job.error[0]) {
            g_no_memory[0] = '\0';
            g_busy_seen_ms = 0;
        }
    }

    switch (kind) {
    case JOB_USER: {
        g_user.live = false;
        if (g_job.orphaned) {
            user_deliver(NULL);
            break;
        }
        // TAI serves one generation at a time: wait and send the same question again (the
        // conversation did not change, so the message is built afresh the same way).
        if (!g_job.cancelled && strcmp(g_job.error, "Error: generation_active") == 0) {
            int32_t delay = ai_queue_user_busy();
            if (delay >= 0) {
                g_user.start_at_ms = now_ms() + delay;
                g_user.waiting_busy = true;
                ai_queue_set_lane(AI_LANE_USER); // hold the lane: nothing quiet slips in meanwhile
                snprintf(app.ai_status, sizeof(app.ai_status), "waiting for the model…");
                break;
            }
            user_deliver(g_job.error);
        } else if (!g_job.cancelled && strcmp(g_job.error, "Error: insufficient_memory") == 0) {
            char line[160], text[200];
            session_header(NULL, 0, line, sizeof(line));
            snprintf(text, sizeof(text), "Error: %s", line[0] ? line : "not enough memory for the model");
            user_deliver(text);
        } else if (!g_job.cancelled && strcmp(g_job.error, "Error: context_full") == 0) {
            user_deliver("Error: That doesn't fit in the model's memory. Ask something shorter.");
        }
        if (ok) {
            if (g_job.held_from >= 0)
                g_held_from = g_job.held_from;
            commit_turn(prompt_tokens, completion_tokens, have_usage);
            log_push(g_user.question, g_job.reply.data, g_user.chat_index);
        }
        user_deliver(NULL);
        break;
    }
    case JOB_PRIME:
        if (ok) {
            commit_turn(prompt_tokens, completion_tokens, have_usage);
            g_prime_wanted = false;
        } else if (!g_job.cancelled && !g_job.orphaned) {
            // Another app holds the model: try again soon. Anything else: give it a while, and
            // the chat's own request stops asking (the header says what happened).
            bool busy = strcmp(g_job.error, "Error: generation_active") == 0;
            g_prime_retry_at = now_ms() + (busy ? 5000 : PRIME_RETRY_MS);
            if (!busy)
                g_prime_wanted = false;
        }
        break;
    case JOB_QUIET: {
        SessionQuietDone done = g_job.done;
        void* data = g_job.done_data;
        g_job.done = NULL;
        g_job.done_data = NULL;
        if (ok)
            commit_turn(prompt_tokens, completion_tokens, have_usage);
        if (done)
            done(ok ? (g_job.reply.data ? g_job.reply.data : "") : NULL, data);
        break;
    }
    case JOB_SUMMARY:
        if (ok) {
            commit_turn(prompt_tokens, completion_tokens, have_usage);
            free(g_summary);
            g_summary = dawn_strdup(g_job.reply.data ? g_job.reply.data : "");
            g_rebuild_after_summary = true;
        } else {
            g_compact_retry_at = now_ms() + COMPACT_RETRY_MS;
        }
        break;
    case JOB_REBUILD:
        if (ok && app.ai_session == g_conv_id) {
            // Switch: the old conversation goes, the primed one takes its place.
            ai_session_id_t old = app.ai_session;
            app.ai_session = g_job.conv;
            g_conv_id = g_job.conv;
            if (old)
                ai_destroy_session(app.ai_ctx, old);
            g_conv_tokens = 0;
            g_conv_tokens_exact = false;
            g_job.conv_before = 0;
            g_conv_model[0] = '\0';
            commit_turn(prompt_tokens, completion_tokens, have_usage);
            if (g_job.held_from >= 0)
                g_held_from = g_job.held_from;
        } else {
            ai_destroy_session(app.ai_ctx, g_job.conv);
            if (!g_job.orphaned)
                g_compact_retry_at = now_ms() + COMPACT_RETRY_MS;
        }
        break;
    default:
        break;
    }
    baseline_free(&g_job.pending);
    free(g_job.reply.data);
    memset(&g_job.reply, 0, sizeof(g_job.reply));
    pump_user();
}

static void session_stream_cb(ai_context_t* context, const char* chunk, void* user_data)
{
    (void)context;
    uint32_t token = (uint32_t)(uintptr_t)user_data;
    if (g_job.kind == JOB_NONE || token != g_job.token)
        return; // a job that is long settled; nothing of it matters now

    if (!chunk) {
        job_finish();
        return;
    }
    if (strcmp(chunk, "null") == 0 || !chunk[0])
        return;
    if (strncmp(chunk, "Error:", 6) == 0) {
        if (!g_job.error[0])
            snprintf(g_job.error, sizeof(g_job.error), "%s", chunk);
        // The sentinels are settled at the end (retry, a plain-words message); other errors
        // are the reply the user sees.
        bool sentinel = strcmp(chunk, "Error: generation_active") == 0
            || strcmp(chunk, "Error: insufficient_memory") == 0 || strcmp(chunk, "Error: context_full") == 0;
        if (g_job.kind == JOB_USER && !g_job.orphaned && !sentinel)
            user_deliver(chunk);
        return;
    }
    g_job.got_output = true;
    g_job.waking_since = 0;
    if (g_job.reply.len < MAX_REPLY_BYTES)
        sb_str(&g_job.reply, chunk);
    // While stopping, the chat drops what still arrives itself.
    if (g_job.kind == JOB_USER && !g_job.orphaned)
        user_deliver(chunk);
}

//! Room left for a new message and its reply in the current conversation.
static int32_t room_left(int32_t max_tokens)
{
    int32_t used = g_conv_tokens > 0 ? g_conv_tokens : system_tokens();
    return ai_ctx_window() - used - max_tokens - REQUEST_MARGIN;
}

//! Start the user's question now.
static void user_start(void)
{
    ai_session_id_t conv = conv_current();
    if (!conv) {
        user_deliver("Error: Couldn't start the request.");
        user_deliver(NULL);
        return;
    }
    int32_t window = ai_ctx_window();
    int32_t q_tokens = ai_estimate_tokens(g_user.question);
    g_job.held_from = -1;
    baseline_free(&g_job.pending);

    sbuf_t msg = { 0 };
    bool fresh = !g_primed || g_conv_tokens == 0;
    if (!fresh) {
        int32_t room = room_left(g_user.max_tokens) - q_tokens - 16;
        int32_t budget = room < (int32_t)(window * 0.3) ? room : (int32_t)(window * 0.3);
        char* diff = budget > 64 ? build_diff(&g_base, budget, &g_job.pending) : NULL;
        size_t sel_s, sel_e;
        get_selection(&sel_s, &sel_e);
        char* selection = sel_s != sel_e ? gap_substr(&app.text, sel_s, sel_e) : NULL;
        sb_str(&msg, g_user.question);
        if (diff || selection)
            sb_str(&msg, "\n\n---\n");
        if (diff)
            sb_str(&msg, diff);
        if (selection) {
            sb_str(&msg, "The user has selected this text (it is what they mean by \"this\"):\n<selection>\n");
            sb_str(&msg, selection);
            sb_str(&msg, "\n</selection>");
        }
        free(diff);
        free(selection);
        g_job.whole = g_whole && (!g_job.pending.valid || baseline_all_seen(&g_job.pending));
        if (ai_estimate_tokens(msg.data) + g_user.max_tokens + REQUEST_MARGIN > window - g_conv_tokens || budget <= 64) {
            // It does not fit the conversation any more (spec: "a fresh one without the recap").
            free(msg.data);
            memset(&msg, 0, sizeof(msg));
            baseline_free(&g_job.pending);
            fresh = true;
            conv = conv_replace();
            g_job.held_from = g_user.chat_index;
            log_clear();
        }
    }
    if (fresh) {
        int32_t room = window - system_tokens() - q_tokens - g_user.max_tokens - REQUEST_MARGIN - 32;
        int32_t budget = room < (int32_t)(window * 0.55) ? room : (int32_t)(window * 0.55);
        bool whole = false;
        char* context = note_context_budget(budget, &whole, &g_job.pending);
        sb_str(&msg, g_user.question);
        sb_str(&msg, "\n\n---\n");
        sb_str(&msg, context);
        free(context);
        g_job.whole = whole;
    }
    char* message = sb_take(&msg);
    g_job.reading = fresh;
    bool started = job_start(JOB_USER, conv, message, g_user.max_tokens);
    free(message);
    if (!started) {
        user_deliver("Error: Couldn't start the request.");
        user_deliver(NULL);
        return;
    }
    g_user.live = true;
    g_user.waiting_busy = false;
    app.ai_stream = g_job.stream;
}

//! Start the waiting question if nothing stands in its way.
static void pump_user(void)
{
    if (!g_user.active || g_user.live)
        return;
    if (g_job.kind != JOB_NONE) {
        // A quiet job gives way; priming does not (the question would need the note read anyway).
        if (g_job.kind != JOB_PRIME)
            job_cancel();
        return;
    }
    if (g_user.start_at_ms && now_ms() < g_user.start_at_ms)
        return;
    user_start();
}

//! Prime the conversation: the system prompt and a snapshot as a first turn with a tiny reply.
static bool prime_start(void)
{
    ai_session_id_t conv = conv_current();
    if (!conv || g_primed || g_conv_tokens > 0)
        return false;
    int32_t window = ai_ctx_window();
    // Leave room for the next reply and a few turns: with 4k that leaves the outline and the
    // current section, with 8k and more most notes fit whole.
    int32_t room = window - system_tokens() - CHAT_REPLY_TOKENS - window / 4 - REQUEST_MARGIN;
    int32_t budget = room < (int32_t)(window * 0.55) ? room : (int32_t)(window * 0.55);
    bool whole = false;
    baseline_free(&g_job.pending);
    char* context = note_context_budget(budget, &whole, &g_job.pending);
    sbuf_t msg = { 0 };
    sb_str(&msg, "(From dawn, not typed by the user.) This is the note I'm writing; read it now, "
                 "I'll ask about it later. Reply with only: ok\n\n");
    sb_str(&msg, context);
    free(context);
    char* message = sb_take(&msg);
    g_job.whole = whole;
    g_job.held_from = -1;
    g_job.reading = true;
    bool started = job_start(JOB_PRIME, conv, message, PRIME_REPLY_TOKENS);
    free(message);
    return started;
}

//! Compaction, step one: the model sums up what came before the last two exchanges.
static bool summary_start(void)
{
    static const char* instruction = "(From dawn, not typed by the user.) In one sentence of at most 40 words, "
                                     "sum up what we have discussed so far (not the note itself). Reply with only that sentence.";
    if (room_left(SUMMARY_REPLY_TOKENS) < ai_estimate_tokens(instruction))
        return false;
    baseline_free(&g_job.pending);
    g_job.held_from = -1;
    g_job.reading = false;
    return job_start(JOB_SUMMARY, g_conv_id, instruction, SUMMARY_REPLY_TOKENS);
}

//! Compaction, step two: a new conversation with the same system prompt, the recap and a fresh
//! snapshot, primed; job_finish() switches to it once that worked.
static bool rebuild_start(void)
{
    ai_session_id_t conv = ai_new_conversation();
    if (!conv)
        return false;
    int32_t recap_tokens = 0;
    for (int32_t i = 0; i < g_log_count; i++) {
        char* answer = recap_answer(g_log[i].answer);
        ai_add_message_to_history(app.ai_ctx, conv, "user", g_log[i].question);
        ai_add_message_to_history(app.ai_ctx, conv, "assistant", answer ? answer : "");
        recap_tokens += ai_estimate_tokens(g_log[i].question) + ai_estimate_tokens(answer) + 8;
        free(answer);
    }
    int32_t window = ai_ctx_window();
    int32_t room = window - system_tokens() - recap_tokens - CHAT_REPLY_TOKENS - window / 4 - REQUEST_MARGIN;
    int32_t budget = room < (int32_t)(window * 0.55) ? room : (int32_t)(window * 0.55);
    bool whole = false;
    baseline_free(&g_job.pending);
    char* context = note_context_budget(budget, &whole, &g_job.pending);
    sbuf_t msg = { 0 };
    sb_str(&msg, "(From dawn, not typed by the user.) ");
    if (g_summary && g_summary[0] && g_exchanges_total > g_log_count) {
        sb_str(&msg, "Earlier in our conversation: ");
        sb_str(&msg, g_summary);
        sb_str(&msg, "\n");
    }
    sb_str(&msg, "Here is the note as it is now; read it, I'll ask about it later. Reply with only: ok\n\n");
    sb_str(&msg, context);
    free(context);
    char* message = sb_take(&msg);
    g_job.whole = whole;
    g_job.held_from = g_log_count > 0 ? g_log[0].chat_index : app.chat_count;
    // Accounting for the new conversation starts from nothing.
    int32_t saved_tokens = g_conv_tokens;
    g_conv_tokens = 0;
    g_job.reading = false; // the writer never sees this one happen
    bool started = job_start(JOB_REBUILD, conv, message, PRIME_REPLY_TOKENS);
    g_job.est_message += recap_tokens;
    g_conv_tokens = saved_tokens;
    free(message);
    if (!started)
        ai_destroy_session(app.ai_ctx, conv);
    return started;
}

// #endregion

// #region Public: jobs

bool session_ask(const char* question, int32_t max_tokens, ai_stream_callback_t cb)
{
    if (!app.ai_ready || !app.ai_ctx || !question || !cb)
        return false;
    if (g_user.active) {
        // One question at a time; the chat never sends a second while one is out.
        return false;
    }
    g_user.active = true;
    g_user.question = dawn_strdup(question);
    g_user.max_tokens = max_tokens > 0 ? max_tokens : CHAT_REPLY_TOKENS;
    g_user.cb = cb;
    g_user.start_at_ms = 0;
    g_user.waiting_busy = false;
    g_user.live = false;
    g_user.chat_index = app.chat_count >= 2 ? app.chat_count - 2 : 0;
    ai_queue_user_reset();
    g_rebuild_after_summary = false; // a compaction in progress gives way and starts over later
    pump_user();
    return true;
}

void session_stop(void)
{
    if (!g_user.active)
        return;
    if (g_user.live) {
        if (g_job.kind == JOB_USER)
            job_cancel();
        return;
    }
    // Still waiting (behind priming, or out TAI's backoff): end it here and now.
    if (g_user.waiting_busy)
        ai_queue_set_lane(AI_LANE_NONE);
    user_deliver(NULL);
}

bool session_user_live(void) { return g_user.live && g_job.kind == JOB_USER; }

bool session_quiet_ready(void)
{
    if (!app.ai_ready || !app.ai_ctx || !g_primed || g_cache_lost || g_job.kind != JOB_NONE || g_user.active)
        return false;
    if (app.ai_input_len > 0) // a question is being typed
        return false;
    if (g_conv_tokens >= (int32_t)(ai_ctx_window() * COMPACT_AT))
        return false; // the compaction comes first
    ai_runtime_info_t info;
    return model_loaded_now(&info) && app.ai_session == g_conv_id;
}

bool session_quiet(const char* instruction, int32_t max_tokens, SessionQuietDone done, void* user_data)
{
    if (!instruction || !session_quiet_ready())
        return false;
    if (max_tokens > 32)
        max_tokens = 32;
    int32_t room = room_left(max_tokens) - ai_estimate_tokens(instruction) - 16;
    if (room <= 0)
        return false;
    int32_t budget = room < (int32_t)(ai_ctx_window() * 0.2) ? room : (int32_t)(ai_ctx_window() * 0.2);
    baseline_free(&g_job.pending);
    char* diff = budget > 64 ? build_diff(&g_base, budget, &g_job.pending) : NULL;
    sbuf_t msg = { 0 };
    sb_str(&msg, instruction);
    if (diff) {
        sb_str(&msg, "\n\n---\n");
        sb_str(&msg, diff);
    }
    free(diff);
    char* message = sb_take(&msg);
    if (ai_estimate_tokens(message) + max_tokens + REQUEST_MARGIN > ai_ctx_window() - g_conv_tokens) {
        free(message);
        baseline_free(&g_job.pending);
        return false;
    }
    g_job.whole = g_whole && (!g_job.pending.valid || baseline_all_seen(&g_job.pending));
    g_job.held_from = -1;
    g_job.reading = false;
    g_job.done = done;
    g_job.done_data = user_data;
    bool started = job_start(JOB_QUIET, g_conv_id, message, max_tokens);
    free(message);
    if (!started) {
        g_job.done = NULL;
        g_job.done_data = NULL;
    }
    return started;
}

// #endregion

// #region Public: lifecycle

void session_reset(void)
{
    if (g_job.kind != JOB_NONE) {
        job_cancel();
        g_job.orphaned = true;
    }
    if (g_user.active && !g_user.live) {
        if (g_user.waiting_busy)
            ai_queue_set_lane(AI_LANE_NONE);
        user_deliver(NULL);
    }
    if (app.ai_ctx && app.ai_session)
        ai_destroy_session(app.ai_ctx, app.ai_session);
    app.ai_session = 0;
    g_conv_id = 0;
    conv_forget();
    log_clear();
    free(g_conv_path);
    g_conv_path = NULL;
    g_held_from = 0;
    g_seen_valid = false;
    g_typed_edits = 0;
    g_rebuild_after_summary = false;
    g_prime_retry_at = 0;
    g_prime_wanted = false;
}

void session_chat_opened(void)
{
    if (!app.ai_ready || !app.ai_ctx)
        return;
    ai_models_refresh();
    g_no_memory[0] = '\0'; // asked again: say it again if it still doesn't fit
    if (g_primed)
        return;
    g_prime_wanted = true;
    g_prime_retry_at = 0;
    if (g_job.kind == JOB_NONE && !g_user.active && app.mode == MODE_WRITING)
        prime_start();
}

//! Watch the note for edits (and the chat input), for "typed for a moment" and quiet moments.
static void track_activity(int64_t now)
{
    size_t len = gap_len(&app.text);
    if (!g_seen_valid) {
        g_seen_len = len;
        g_seen_cursor = app.cursor;
        g_seen_input_len = app.ai_input_len;
        g_seen_valid = true;
        return;
    }
    bool edited = len != g_seen_len;
    if (edited || app.cursor != g_seen_cursor || app.ai_input_len != g_seen_input_len) {
        g_last_activity_ms = now;
        if (edited)
            g_typed_edits++;
    }
    g_seen_len = len;
    g_seen_cursor = app.cursor;
    g_seen_input_len = app.ai_input_len;
}

//! Whether TAI has most likely thrown our KV cache away: the model was unloaded or swapped, or
//! someone else generated since our last turn (one live conversation, one slot).
static void watch_cache(const ai_runtime_info_t* info)
{
    if (!g_primed || g_cache_lost || g_job.kind != JOB_NONE || g_user.active)
        return;
    if (info->checked_at_ms <= g_last_job_end_wall_ms + 1500)
        return; // not a fresh enough answer to judge by
    if (info->state == AI_MODEL_LOADED && info->loaded_model[0] && !g_conv_model[0])
        snprintf(g_conv_model, sizeof(g_conv_model), "%s", info->loaded_model);
    bool lost = info->state == AI_MODEL_NOT_LOADED || info->generating
        || (info->loaded_model[0] && g_conv_model[0] && strcmp(info->loaded_model, g_conv_model) != 0);
    if (lost)
        g_cache_lost = true;
}

void session_tick(void)
{
    if (!app.ai_ready || !app.ai_ctx)
        return;
    int64_t now = now_ms();

    // A different note (or none): what the conversation knew is about another note.
    if (!same_path(g_conv_path, app.session_path)) {
        if (g_conv_path || g_primed || g_job.kind != JOB_NONE)
            session_reset();
        g_conv_path = app.session_path ? dawn_strdup(app.session_path) : NULL;
        g_held_from = app.chat_count; // a chat loaded from disk is scrollback the model never heard
    }
    track_activity(now);

    bool urgent = app.ai_open || g_job.kind != JOB_NONE || g_user.active || g_prime_wanted
        || (!g_primed && g_typed_edits >= PRIME_TYPED_EDITS);
    if (urgent || g_primed)
        runtime_poll(urgent);
    ai_runtime_info_t info = g_info;
    watch_cache(&info);

    // Waking ends once TAI says the model is in.
    if (g_job.kind != JOB_NONE && g_job.waking_since && info.state == AI_MODEL_LOADED && !info.loading
        && info.checked_at_ms > g_job.started_wall_ms + 500)
        g_job.waking_since = 0;

    pump_user();
    if (g_job.kind != JOB_NONE || g_user.active)
        return;

    bool writing = app.mode == MODE_WRITING && !app.preview_mode && app.session_path;
    bool loaded = info.state == AI_MODEL_LOADED && !info.loading;
    bool quiet = now - g_last_activity_ms >= QUIET_IDLE_MS && now - g_last_job_end_ms >= QUIET_IDLE_MS
        && app.ai_input_len == 0;

    // Keep-warm: so TAI's idle unload does not throw the primed cache away while the writer is here.
    if (writing && g_primed && !g_cache_lost && loaded && now - g_last_activity_ms < LIVE_WITHIN_MS
        && now - g_last_keep_warm_ms >= KEEP_WARM_EVERY_MS && now - g_last_job_end_ms >= KEEP_WARM_EVERY_MS) {
        ai_keep_warm(KEEP_WARM_MINUTES);
        g_last_keep_warm_ms = now;
    }

    // The chat asked for the note to be read (and may load the model for it); retries after
    // TAI said "busy" come through here.
    if (writing && g_prime_wanted && !g_primed && now >= g_prime_retry_at) {
        prime_start();
        return;
    }

    if (!writing || !loaded)
        return;

    // Compaction: at 70% of the window, or to re-read the note after the cache was lost.
    int32_t window = ai_ctx_window();
    if (g_rebuild_after_summary && quiet) {
        g_rebuild_after_summary = false;
        if (!rebuild_start())
            g_compact_retry_at = now + COMPACT_RETRY_MS;
        return;
    }
    bool compact_due = g_primed && (g_conv_tokens >= (int32_t)(window * COMPACT_AT) || g_cache_lost);
    if (compact_due && quiet && now >= g_compact_retry_at) {
        // A lost cache would make the summary re-read the whole old conversation: skip it then.
        bool started = !g_cache_lost && g_exchanges_total > g_log_count ? summary_start() : false;
        if (!started)
            started = rebuild_start();
        if (!started)
            g_compact_retry_at = now + COMPACT_RETRY_MS;
        return;
    }

    // Priming on our own: only with the model already loaded, once the writer has typed for a
    // moment and the note has settled.
    if (!g_primed && g_typed_edits >= PRIME_TYPED_EDITS && now - g_last_activity_ms >= PRIME_IDLE_MS
        && now >= g_prime_retry_at) {
        prime_start();
        return;
    }
}

// #endregion

// #region Public: what the chat shows

//! "Gemma 4 E4B" and "Gemma 4 E2B" become "E4B" and "E2B": the words both names share in front
//! say nothing about the difference, and the header line is narrow.
static void short_names(const char* a, const char* b, const char** a_out, const char** b_out)
{
    size_t cut = 0;
    for (size_t i = 0; a[i] && b[i] && a[i] == b[i]; i++)
        if (a[i] == ' ')
            cut = i + 1;
    *a_out = a[cut] ? a + cut : a;
    *b_out = b[cut] ? b + cut : b;
}

void session_header(char* name, size_t name_cap, char* line, size_t line_cap)
{
    ai_model_info_t models[AI_MAX_MODELS];
    int32_t count = ai_models(models, AI_MAX_MODELS);
    const char* id = session_model_id();
    const char* shown = NULL;
    for (int32_t i = 0; i < count && i < AI_MAX_MODELS && id[0]; i++)
        if (strcmp(models[i].id, id) == 0)
            shown = models[i].name;
    if (name && name_cap > 0)
        snprintf(name, name_cap, "%s", shown ? shown : id[0] ? id : "AI");
    if (!line || line_cap == 0)
        return;
    line[0] = '\0';

    if (g_no_memory[0]) {
        // Name the smallest other model that could fit instead.
        const char* failed = g_no_memory;
        int64_t failed_size = 0;
        for (int32_t i = 0; i < count && i < AI_MAX_MODELS; i++)
            if (strcmp(models[i].id, g_no_memory) == 0) {
                failed = models[i].name;
                failed_size = models[i].size_bytes;
            }
        const char* smaller = NULL;
        int64_t smaller_size = 0;
        for (int32_t i = 0; i < count && i < AI_MAX_MODELS; i++) {
            if (strcmp(models[i].id, g_no_memory) == 0 || models[i].size_bytes <= 0)
                continue;
            if (failed_size > 0 && models[i].size_bytes >= failed_size)
                continue;
            if (!smaller || models[i].size_bytes < smaller_size) {
                smaller = models[i].name;
                smaller_size = models[i].size_bytes;
            }
        }
        if (smaller) {
            const char *a, *b;
            short_names(failed, smaller, &a, &b);
            snprintf(line, line_cap, "not enough memory for %s · try %s", a, b);
        } else {
            snprintf(line, line_cap, "not enough memory for %s", failed);
        }
        return;
    }
    int64_t now = now_ms();
    if (g_user.waiting_busy || (g_busy_seen_ms && now - g_busy_seen_ms < 6000 && !g_primed)) {
        snprintf(line, line_cap, "waiting for the model");
        return;
    }
    int64_t waking = session_waking_since();
    if (waking) {
        snprintf(line, line_cap, "waking the model · %lld s", (long long)((now - waking) / 1000));
        return;
    }
    if (session_reading()) {
        snprintf(line, line_cap, "reading the note…");
        return;
    }
    if (g_primed && !g_cache_lost && same_path(g_conv_path, app.session_path))
        snprintf(line, line_cap, "has read this note");
}

bool session_reading(void)
{
    return g_job.kind != JOB_NONE && g_job.reading && !g_job.got_output && !g_job.orphaned;
}

bool session_chat_may_open(void)
{
    static bool told;
    if (!app.ai_ready || !app.ai_ctx)
        return false;
    ai_runtime_info(&g_info);
    int32_t models = ai_models(NULL, 0);
    // Unknown (not asked yet) counts as available: the chat opens and the first request tells.
    bool off = g_info.reachable == 0 || models == 0;
    if (off && !told) {
        notice_post(NOTICE_INFO, "AI is off");
        told = true;
    }
    return !off;
}

int64_t session_waking_since(void)
{
    if (g_job.kind == JOB_NONE || g_job.got_output)
        return 0;
    return g_job.waking_since;
}

int32_t session_held_from(void)
{
    if (g_held_from < 0)
        return 0;
    return g_held_from > app.chat_count ? app.chat_count : g_held_from;
}

bool session_saw_whole_note(void)
{
    if (g_job.kind == JOB_USER)
        return g_job.whole;
    return g_whole;
}

// #endregion

#endif // HAS_LIBAI
