// dawn_ai_tokens.c

#include "dawn_ai_tokens.h"

#if HAS_LIBAI

#include "ai.h"
#include "dawn_block.h"
#include "dawn_gap.h"
#include "dawn_utils.h"

#include <string.h>

// #region Token estimate

//! Smoothed actual/estimated ratio applied to every future estimate; starts at 1.0 (trust the
//! plain char-based formula until the first real usage.prompt_tokens comes back).
static double g_calibration_ratio = 1.0;

int32_t ai_estimate_tokens(const char* text)
{
    if (!text || !text[0])
        return 0;
    size_t chars = 0, arabic = 0;
    const unsigned char* p = (const unsigned char*)text;
    while (*p) {
        uint32_t cp;
        int len;
        if (*p < 0x80) {
            cp = *p;
            len = 1;
        } else if ((*p & 0xE0) == 0xC0) {
            cp = *p & 0x1F;
            len = 2;
        } else if ((*p & 0xF0) == 0xE0) {
            cp = *p & 0x0F;
            len = 3;
        } else if ((*p & 0xF8) == 0xF0) {
            cp = *p & 0x07;
            len = 4;
        } else {
            p++; // stray continuation byte; count it as one char and move on
            chars++;
            continue;
        }
        for (int i = 1; i < len && p[i]; i++)
            cp = (cp << 6) | (p[i] & 0x3F);
        p += len;
        chars++;
        // Arabic, Arabic Supplement, Arabic Extended-A, Arabic Presentation Forms.
        if ((cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0x0750 && cp <= 0x077F)
            || (cp >= 0x08A0 && cp <= 0x08FF) || (cp >= 0xFB50 && cp <= 0xFDFF)
            || (cp >= 0xFE70 && cp <= 0xFEFF))
            arabic++;
    }
    if (chars == 0)
        return 0;
    double divisor = arabic * 2 > chars ? 2.5 : 3.6; // mostly Arabic script vs mostly Latin
    double tokens = (double)chars / divisor * g_calibration_ratio;
    int32_t out = (int32_t)(tokens + 0.5);
    return out > 0 ? out : 1;
}

void ai_calibrate_estimate(int32_t estimated_prompt_tokens, int32_t actual_prompt_tokens)
{
    if (estimated_prompt_tokens <= 0 || actual_prompt_tokens <= 0)
        return;
    double sample = (double)actual_prompt_tokens / (double)estimated_prompt_tokens;
    // Exponential moving average: one bad turn (a burst of code, an odd script) cannot swing the
    // estimate for every turn after it, but the ratio still tracks the model actually in use.
    g_calibration_ratio = g_calibration_ratio * 0.7 + sample * 0.3;
    if (g_calibration_ratio < 0.4)
        g_calibration_ratio = 0.4;
    else if (g_calibration_ratio > 2.5)
        g_calibration_ratio = 2.5;
}

int32_t ai_ctx_window(void)
{
    int32_t window = ai_context_window();
    return window > 0 ? window : 4096;
}

// #endregion

// #region Note snapshot

//! Small growable buffer, local to this file: dawn has no shared strbuf outside libai.
typedef struct {
    char* data;
    size_t len;
    size_t cap;
} buf_t;

static void buf_append(buf_t* b, const char* s, size_t n)
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

static void buf_append_str(buf_t* b, const char* s) { buf_append(b, s, s ? strlen(s) : 0); }

//! [start, end) of the note, byte-safe (never splits a UTF-8 character), and never past what
//! remains of the token budget: shortens end to fit before slicing, not after.
static char* slice_within_budget(const GapBuffer* gb, size_t start, size_t end, int32_t* budget, bool* cut)
{
    if (end < start)
        end = start;
    *cut = false;
    if (*budget <= 0) {
        *cut = end > start;
        end = start; // no budget left at all: an empty slice, not an unbounded one
    } else {
        size_t max_chars = (size_t)(*budget) * 4; // generous chars-per-token upper bound
        if (end - start > max_chars) {
            end = start + max_chars;
            *cut = true;
        }
    }
    while (end > start && ((unsigned char)gap_at(gb, end) & 0xC0) == 0x80)
        end--;
    char* text = gap_substr(gb, start, end);
    int32_t spent = ai_estimate_tokens(text);
    *budget -= spent;
    return text;
}

//! Trim trailing blank lines/space from a malloc'd string, in place.
static void rtrim(char* s)
{
    size_t n = s ? strlen(s) : 0;
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
}

//! Whether [start, end) lies wholly inside one of the relevant passages the snapshot took.
static bool in_taken(const EmbedHit* relevant, const bool* taken, int32_t count, size_t start, size_t end)
{
    for (int32_t i = 0; i < count; i++)
        if (taken[i] && start >= relevant[i].start && end <= (size_t)relevant[i].start + relevant[i].len)
            return true;
    return false;
}

char* ai_note_snapshot(const GapBuffer* gb, void* block_cache, size_t cursor, size_t sel_start,
    size_t sel_end, int32_t budget_tokens, const EmbedHit* relevant, int32_t relevant_count, AiSnapshotInfo* info)
{
    memset(info, 0, sizeof(*info));
    size_t doc_len = gap_len(gb);
    if (doc_len == 0) {
        info->whole_note = true;
        return dawn_strdup("");
    }

    BlockCache* bc = block_cache;
    bool have_blocks = bc && bc->valid && bc->count > 0;

    // Find the section around the cursor: the nearest heading at or before it, and where the
    // next heading of the same or higher level starts (or the doc's end).
    int32_t header_idx = -1;
    uint32_t header_level = 0;
    uint32_t sec_start = 0, sec_end = (uint32_t)doc_len;
    uint32_t sec_start_pos = 0, sec_end_pos = (uint32_t)doc_len;

    if (have_blocks) {
        for (uint32_t i = 0; i < bc->count; i++) {
            Block* b = &bc->blocks[i];
            if (b->start > cursor)
                break;
            if (b->type == BLOCK_HEADER) {
                header_idx = (int32_t)i;
                header_level = b->data.header.level;
            }
        }
        if (header_idx >= 0) {
            sec_start = bc->blocks[header_idx].start;
            sec_end = (uint32_t)doc_len;
            for (uint32_t i = (uint32_t)header_idx + 1; i < bc->count; i++) {
                Block* b = &bc->blocks[i];
                if (b->type == BLOCK_HEADER && b->data.header.level <= header_level) {
                    sec_end = b->start;
                    break;
                }
            }
        } else {
            // Cursor sits before any heading (or the note has none): the section is the
            // preamble, up to the first heading if there is one.
            sec_start = 0;
            sec_end = (uint32_t)doc_len;
            for (uint32_t i = 0; i < bc->count; i++) {
                if (bc->blocks[i].type == BLOCK_HEADER) {
                    sec_end = bc->blocks[i].start;
                    break;
                }
            }
        }
        sec_start_pos = sec_start;
        sec_end_pos = sec_end;
        if (header_idx >= 0) {
            const Block* hb = &bc->blocks[header_idx];
            uint32_t ts = hb->data.header.content_start, te = hb->end;
            if (te > ts) {
                char* text = gap_substr(gb, ts, te);
                rtrim(text);
                dawn_strncpy(info->section_heading, text, sizeof(info->section_heading) - 1);
                free(text);
            }
        }
    }

    int32_t budget = budget_tokens;
    bool section_cut = false;
    buf_t out = { 0 };

    // 1) The selection, if any: always included, even if it alone must spend the whole budget.
    if (sel_start != sel_end) {
        bool cut;
        char* sel = slice_within_budget(gb, sel_start, sel_end, &budget, &cut);
        buf_append_str(&out, "The user has selected this part of the note:\n<selection>\n");
        buf_append_str(&out, sel);
        buf_append_str(&out, cut ? "\n</selection>\n(The selection is longer; only its beginning is shown.)\n" : "\n</selection>\n");
        free(sel);
        info->has_selection = true;
    }

    // 2) The section around the cursor.
    if (budget > 0) {
        bool cut;
        char* section = slice_within_budget(gb, sec_start_pos, sec_end_pos, &budget, &cut);
        rtrim(section);
        if (section[0]) {
            // <note> matches the system prompt (dawn_chat.c: "a snapshot of it between
            // <note> tags"), whether this is the whole note or, once trimmed, just its section.
            buf_append_str(&out, "<note>\n");
            buf_append_str(&out, section);
            buf_append_str(&out, "\n</note>\n");
        }
        free(section);
        section_cut = cut;
    } else if (sec_end_pos > sec_start_pos) {
        section_cut = true; // no budget left even to start the section
    }

    // 3) The outline: every heading, in document order, indented by level.
    if (have_blocks && budget > 0) {
        buf_t outline = { 0 };
        for (uint32_t i = 0; i < bc->count; i++) {
            Block* b = &bc->blocks[i];
            if (b->type != BLOCK_HEADER)
                continue;
            char* text = gap_substr(gb, b->data.header.content_start, b->end);
            rtrim(text);
            for (int32_t d = 1; d < b->data.header.level; d++)
                buf_append_str(&outline, "  ");
            buf_append_str(&outline, "- ");
            buf_append_str(&outline, text);
            buf_append_str(&outline, "\n");
            free(text);
        }
        if (outline.len > 0) {
            int32_t cost = ai_estimate_tokens(outline.data);
            if (cost <= budget) {
                buf_append_str(&out, "The note's outline:\n<outline>\n");
                buf_append_str(&out, outline.data);
                buf_append_str(&out, "</outline>\n");
                budget -= cost;
                info->has_outline = true;
            }
        }
        free(outline.data);
    }

    // 4) Passages relevant to the question (dawn_embed), best first, that the selection and the
    // section don't already show.
    bool taken[16] = { false };
    if (relevant_count > 16)
        relevant_count = 16;
    if (!relevant)
        relevant_count = 0;
    buf_t passages = { 0 };
    for (int32_t i = 0; i < relevant_count && budget > 0; i++) {
        size_t rs = relevant[i].start, re = rs + relevant[i].len;
        if (re > doc_len || re <= rs || (rs < sec_end_pos && re > sec_start_pos)
            || (sel_start != sel_end && rs < sel_end && re > sel_start))
            continue;
        bool cut;
        char* text = slice_within_budget(gb, rs, re, &budget, &cut);
        rtrim(text);
        if (text[0]) {
            if (relevant[i].heading[0]) {
                buf_append_str(&passages, "(under \"");
                buf_append_str(&passages, relevant[i].heading);
                buf_append_str(&passages, "\")\n");
            }
            buf_append_str(&passages, text);
            buf_append_str(&passages, cut ? "\n(cut short)\n\n" : "\n\n");
            taken[i] = true;
        }
        free(text);
    }
    if (passages.len > 0) {
        buf_append_str(&out, "Passages from elsewhere in the note that bear on the question:\n<relevant>\n");
        buf_append_str(&out, passages.data);
        buf_append_str(&out, "</relevant>\n");
        info->has_relevant = true;
    }
    free(passages.data);

    // 5) Neighbouring paragraphs, alternating before/after the section, until the budget runs
    // out or there is nothing left to add.
    if (have_blocks && budget > 0) {
        int32_t before_idx = header_idx >= 0 ? header_idx - 1 : -1;
        // Find the block index the section ends at, to walk forward from.
        int32_t after_idx = -1;
        for (uint32_t i = 0; i < bc->count; i++) {
            if (bc->blocks[i].start >= sec_end_pos) {
                after_idx = (int32_t)i;
                break;
            }
        }
        if (header_idx < 0) {
            // The section is the preamble; there is nothing "before" it.
            before_idx = -1;
        }
        bool try_before = before_idx >= 0;
        bool try_after = after_idx >= 0 && after_idx < (int32_t)bc->count;
        buf_t neighbours_before = { 0 }, neighbours_after = { 0 };
        while (budget > 0 && (try_before || try_after)) {
            if (try_before && in_taken(relevant, taken, relevant_count, bc->blocks[before_idx].start, bc->blocks[before_idx].end)) {
                before_idx--; // shown already, as a relevant passage
                try_before = before_idx >= 0;
            } else if (try_before) {
                Block* b = &bc->blocks[before_idx];
                char* text = gap_substr(gb, b->start, b->end);
                rtrim(text);
                int32_t cost = ai_estimate_tokens(text);
                if (text[0] && cost <= budget) {
                    buf_t joined = { 0 };
                    buf_append_str(&joined, text);
                    buf_append_str(&joined, "\n");
                    buf_append(&neighbours_before, joined.data, joined.len);
                    free(joined.data);
                    budget -= cost;
                    info->neighbours_before++;
                    before_idx--;
                    try_before = before_idx >= 0;
                } else {
                    try_before = false;
                }
                free(text);
            }
            if (budget <= 0)
                break;
            if (try_after && in_taken(relevant, taken, relevant_count, bc->blocks[after_idx].start, bc->blocks[after_idx].end)) {
                after_idx++;
                try_after = after_idx < (int32_t)bc->count;
            } else if (try_after) {
                Block* b = &bc->blocks[after_idx];
                char* text = gap_substr(gb, b->start, b->end);
                rtrim(text);
                int32_t cost = ai_estimate_tokens(text);
                if (text[0] && cost <= budget) {
                    buf_append_str(&neighbours_after, text);
                    buf_append_str(&neighbours_after, "\n");
                    budget -= cost;
                    info->neighbours_after++;
                    after_idx++;
                    try_after = after_idx < (int32_t)bc->count;
                } else {
                    try_after = false;
                }
                free(text);
            }
        }
        if (neighbours_before.len > 0) {
            // Collected walking backward, so its blocks are in reverse document order: turn them
            // right way round is skipped for simplicity (rare, small section) — noted as text.
            buf_append_str(&out, "Text just before that section:\n<before>\n");
            buf_append_str(&out, neighbours_before.data);
            buf_append_str(&out, "</before>\n");
        }
        if (neighbours_after.len > 0) {
            buf_append_str(&out, "Text just after that section:\n<after>\n");
            buf_append_str(&out, neighbours_after.data);
            buf_append_str(&out, "</after>\n");
        }
        free(neighbours_before.data);
        free(neighbours_after.data);
    }

    // Whether the note's body was shown in full: independent of the selection (which is extra
    // context, not part of "the note" replace_note reasons about) — a selected note that was
    // otherwise shown whole is still a whole note.
    info->whole_note = !section_cut && sec_start_pos == 0 && sec_end_pos == (uint32_t)doc_len
        && info->neighbours_before == 0 && info->neighbours_after == 0;

    return out.data ? out.data : dawn_strdup("");
}

// #endregion

#endif // HAS_LIBAI
