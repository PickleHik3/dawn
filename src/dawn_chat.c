// dawn_chat.c

#include "dawn_chat.h"
#include "cJSON.h"
#include "dawn_utils.h"
#include "dawn_file.h"
#include "dawn_gap.h"
#include "dawn_nav.h"
#include "dawn_block.h"
#include "dawn_fm.h"

#include <strings.h>

// #region Message Management

void chat_add(const char* text, bool is_user)
{
    app.chat_msgs = realloc(app.chat_msgs, sizeof(ChatMessage) * (size_t)(app.chat_count + 1));
    ChatMessage* m = &app.chat_msgs[app.chat_count++];
    m->text = dawn_strdup(text);
    m->len = strlen(text);
    m->is_user = is_user;
}

void chat_clear(void)
{
    for (int32_t i = 0; i < app.chat_count; i++) {
        free(app.chat_msgs[i].text);
    }
    free(app.chat_msgs);
    app.chat_msgs = NULL;
    app.chat_count = 0;
#if HAS_LIBAI
    // The chat is per note: without this the model still remembers the last note's conversation
    // and answers "summarize this" about that one.
    if (app.ai_ctx && app.ai_session)
        ai_clear_session_history(app.ai_ctx, app.ai_session);
#endif
}

// #endregion

#if HAS_LIBAI

// #region AI Streaming

static void apply_reply_edits(void);

static void ai_stream_cb(ai_context_t* context, const char* chunk, void* user_data)
{
    (void)context;
    (void)user_data;

    if (chunk) {
        // Skip null/empty chunks
        if (strcmp(chunk, "null") == 0 || strlen(chunk) == 0)
            return;

        // Check for error responses
        if (strncmp(chunk, "Error:", 6) == 0) {
            // Replace AI message with error
            if (app.chat_count > 0 && !app.chat_msgs[app.chat_count - 1].is_user) {
                ChatMessage* m = &app.chat_msgs[app.chat_count - 1];
                free(m->text);
                m->text = dawn_strdup(chunk);
                m->len = strlen(chunk);
            }
            app.ai_thinking = false;
            return;
        }

        if (app.chat_count > 0 && !app.chat_msgs[app.chat_count - 1].is_user) {
            ChatMessage* m = &app.chat_msgs[app.chat_count - 1];
            size_t chunk_len = strlen(chunk);

            // Reallocate and append
            char* new_text = realloc(m->text, m->len + chunk_len + 1);
            if (new_text) {
                m->text = new_text;
                memcpy(m->text + m->len, chunk, chunk_len);
                m->len += chunk_len;
                m->text[m->len] = '\0';
            }

            // Auto-scroll to bottom when streaming
            app.chat_scroll = 0;
        }
    } else {
        // Stream complete
        app.ai_thinking = false;
        apply_reply_edits();
    }
}

// #region Note Context

#define NOTE_CONTEXT_LIMIT 8000
#define SELECTION_CONTEXT_LIMIT 4000

//! [start, start + limit) of the note, shortened to end on a UTF-8 character boundary.
static char* note_slice(size_t start, size_t end, size_t limit, bool* cut)
{
    *cut = end - start > limit;
    if (*cut) {
        end = start + limit;
        while (end > start && ((unsigned char)gap_at(&app.text, end) & 0xC0) == 0x80)
            end--;
    }
    return gap_substr(&app.text, start, end);
}

static const char* note_title(void)
{
    return app.frontmatter ? fm_get_string(app.frontmatter, "title") : NULL;
}

//! What rides along with every question: the note's title, the note and the selection. Without
//! it a model that cannot call read_document (TAI drops tools for most on-device models without
//! saying so) has no idea what "this" is.
static char* note_context(void)
{
    size_t len = gap_len(&app.text);
    const char* title = note_title();
    char title_line[320] = "";
    if (title && title[0] && strcmp(title, "Untitled") != 0)
        snprintf(title_line, sizeof(title_line), "Its title is \"%s\".\n", title);
    if (len == 0)
        return dawn_strdup("The user's note is empty.");

    bool note_cut, sel_cut = false;
    char* note = note_slice(0, len, NOTE_CONTEXT_LIMIT, &note_cut);
    size_t s, e;
    get_selection(&s, &e);
    char* sel = s != e ? note_slice(s, e, SELECTION_CONTEXT_LIMIT, &sel_cut) : NULL;

    const char* fmt = "The user's open note is below. \"This\", \"the note\" and \"the document\" mean it.\n"
                      "%s<note>\n%s\n</note>\n%s%s%s%s";
    const char* cut_line = note_cut ? "(The note is longer; only its beginning is shown.)\n" : "";
    const char* sel_open = sel ? "The user has selected this part of it:\n<selection>\n" : "";
    const char* sel_close = sel ? (sel_cut ? "\n</selection>\n(The selection is longer; only its beginning is shown.)" : "\n</selection>") : "";
    size_t n = strlen(fmt) + strlen(title_line) + strlen(note) + strlen(cut_line) + strlen(sel_open)
        + (sel ? strlen(sel) : 0) + strlen(sel_close) + 1;
    char* out = malloc(n);
    if (out)
        snprintf(out, n, fmt, title_line, note, cut_line, sel_open, sel ? sel : "", sel_close);
    free(note);
    free(sel);
    return out;
}

// #endregion

// Tool callback for reading current document
char* document_tool_callback(const char* params_json, void* user_data)
{
    (void)user_data;

    cJSON* params = cJSON_Parse(params_json);
    const char* action = "full";
    int32_t offset = 0;
    int32_t length = -1;

    if (params) {
        cJSON* action_obj = cJSON_GetObjectItem(params, "action");
        if (action_obj && action_obj->valuestring) {
            action = action_obj->valuestring;
        }
        cJSON* offset_obj = cJSON_GetObjectItem(params, "offset");
        if (offset_obj && cJSON_IsNumber(offset_obj)) {
            offset = offset_obj->valueint;
        }
        cJSON* length_obj = cJSON_GetObjectItem(params, "length");
        if (length_obj && cJSON_IsNumber(length_obj)) {
            length = length_obj->valueint;
        }
    }

    cJSON* response = cJSON_CreateObject();
    size_t doc_len = gap_len(&app.text);

    if (strcmp(action, "info") == 0) {
        // Return document info
        cJSON_AddNumberToObject(response, "total_length", (double)doc_len);

        size_t sel_start, sel_end;
        get_selection(&sel_start, &sel_end);
        bool has_sel = (sel_start != sel_end);

        cJSON_AddBoolToObject(response, "has_selection", has_sel);
        if (has_sel) {
            cJSON_AddNumberToObject(response, "selection_start", (double)sel_start);
            cJSON_AddNumberToObject(response, "selection_end", (double)sel_end);
            cJSON_AddNumberToObject(response, "selection_length", (double)(sel_end - sel_start));
        }
        cJSON_AddNumberToObject(response, "cursor_position", (double)app.cursor);

    } else if (strcmp(action, "context") == 0) {
        char* context = note_context();
        cJSON_AddStringToObject(response, "text", context ? context : "");
        free(context);

    } else if (strcmp(action, "selection") == 0) {
        // Return selected text
        size_t sel_start, sel_end;
        get_selection(&sel_start, &sel_end);

        if (sel_start != sel_end) {
            char* selected = gap_substr(&app.text, sel_start, sel_end);
            cJSON_AddStringToObject(response, "text", selected);
            cJSON_AddNumberToObject(response, "start", (double)sel_start);
            cJSON_AddNumberToObject(response, "end", (double)sel_end);
            free(selected);
        } else {
            cJSON_AddStringToObject(response, "text", "");
            cJSON_AddStringToObject(response, "note", "No text selected");
        }

    } else if (strcmp(action, "range") == 0) {
        // Return text in range
        if (offset < 0)
            offset = 0;
        if ((size_t)offset >= doc_len) {
            cJSON_AddStringToObject(response, "text", "");
            cJSON_AddStringToObject(response, "note", "Offset beyond document end");
        } else {
            size_t start = (size_t)offset;
            size_t end = (length < 0) ? doc_len : (size_t)(offset + length);
            if (end > doc_len)
                end = doc_len;

            char* text = gap_substr(&app.text, start, end);
            cJSON_AddStringToObject(response, "text", text);
            cJSON_AddNumberToObject(response, "start", (double)start);
            cJSON_AddNumberToObject(response, "end", (double)end);
            free(text);
        }

    } else {
        // Default: return full document (truncated if very long)
        char* full_text = gap_to_str(&app.text);
        size_t max_len = 8000; // Limit to avoid overwhelming context

        if (doc_len > max_len) {
            full_text[max_len] = '\0';
            cJSON_AddStringToObject(response, "text", full_text);
            cJSON_AddBoolToObject(response, "truncated", true);
            cJSON_AddNumberToObject(response, "total_length", (double)doc_len);
        } else {
            cJSON_AddStringToObject(response, "text", full_text);
            cJSON_AddBoolToObject(response, "truncated", false);
        }
        free(full_text);
    }

    if (params)
        cJSON_Delete(params);

    char* json_str = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    return json_str;
}

// #region Edit Tools

//! The note the question being answered was asked about. Edits land only while that note is
//! still the open one, so a slow reply cannot rewrite the note the user has since switched to.
static char* g_turn_path;

//! Append a line to the reply that is streaming, so every change the AI makes is stated in the
//! chat whether or not the model mentions it.
static void chat_note_edit(const char* note)
{
    if (app.chat_count == 0 || app.chat_msgs[app.chat_count - 1].is_user)
        return;
    ChatMessage* m = &app.chat_msgs[app.chat_count - 1];
    const char* sep = m->len > 0 && m->text[m->len - 1] != '\n' ? "\n\n" : "";
    size_t add = strlen(sep) + strlen(note) + 2;
    char* grown = realloc(m->text, m->len + add + 1);
    if (!grown)
        return;
    m->text = grown;
    m->len += (size_t)snprintf(m->text + m->len, add + 1, "%s%s\n\n", sep, note);
}

//! text with carriage returns dropped, in place.
static void drop_carriage_returns(char* text)
{
    size_t w = 0;
    for (size_t r = 0; text[r]; r++)
        if (text[r] != '\r')
            text[w++] = text[r];
    text[w] = '\0';
}

//! The "text" argument of an edit call with carriage returns dropped, or NULL if there is none.
static char* edit_text_param(const char* params_json)
{
    cJSON* params = cJSON_Parse(params_json);
    cJSON* text = cJSON_GetObjectItemCaseSensitive(params, "text");
    char* out = NULL;
    if (cJSON_IsString(text) && text->valuestring) {
        out = dawn_strdup(text->valuestring);
        drop_carriage_returns(out);
    }
    cJSON_Delete(params);
    return out;
}

static char* edit_error(const char* message)
{
    cJSON* response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "error", message);
    char* json = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    return json;
}

static bool note_is_editable(void)
{
    if (app.mode != MODE_WRITING || app.focus_mode || app.preview_mode)
        return false;
    if (g_turn_path && (!app.session_path || strcmp(g_turn_path, app.session_path) != 0))
        return false;
    return true;
}

//! Replace [start, end) with text as one undoable step and leave the cursor after it.
static void apply_edit(size_t start, size_t end, const char* text)
{
    size_t n = strlen(text);
    save_undo_state();
    if (end > start)
        gap_delete(&app.text, start, end - start);
    gap_insert_str(&app.text, start, text, n);
    app.selecting = false;
    app.cursor = start + n;
    save_undo_state();
    if (app.block_cache)
        block_cache_invalidate((BlockCache*)app.block_cache);
}

//! title reduced to one clean line: no heading marks, "Title:" label, quotes or final period.
static bool clean_title(const char* raw, char* out, size_t cap)
{
    const char* s = raw;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    size_t n = 0;
    while (s[n] && s[n] != '\n' && s[n] != '\r')
        n++;
    while (n > 0 && (*s == '#' || *s == '*' || *s == '"' || *s == '\'' || *s == '`' || *s == ' ')) {
        s++;
        n--;
    }
    if (n >= 6 && strncasecmp(s, "title:", 6) == 0) {
        s += 6;
        n -= 6;
        while (n > 0 && (*s == ' ' || *s == '"' || *s == '\'' || *s == '*')) {
            s++;
            n--;
        }
    }
    while (n > 0 && strchr(" .*\"'`", s[n - 1]))
        n--;
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
            n--;
    }
    memcpy(out, s, n);
    out[n] = '\0';
    return n > 0;
}

//! Rename the note, in the frontmatter the next save writes and in the window title.
static bool set_note_title(const char* raw)
{
    char title[81];
    if (!clean_title(raw, title, sizeof(title)))
        return false;
    if (!app.frontmatter)
        app.frontmatter = fm_create();
    fm_set_string(app.frontmatter, "title", title);
    DAWN_BACKEND(app)->set_title(title);
    return true;
}

typedef enum { EDIT_REPLACE_NOTE, EDIT_REPLACE_SELECTION, EDIT_INSERT, EDIT_APPEND, EDIT_TITLE, EDIT_KIND_COUNT } EditKind;

//! Each edit's name, as a tool and as a <tag> in a plain reply, and what the chat says after it.
static const struct {
    const char* name;
    const char* done;
} EDITS[EDIT_KIND_COUNT] = {
    [EDIT_REPLACE_NOTE] = { "replace_note", "Rewrote the note. Ctrl+Z undoes it." },
    [EDIT_REPLACE_SELECTION] = { "replace_selection", "Replaced the selected text. Ctrl+Z undoes it." },
    [EDIT_INSERT] = { "insert_at_cursor", "Added text at the cursor. Ctrl+Z undoes it." },
    [EDIT_APPEND] = { "append_to_note", "Added text to the end of the note. Ctrl+Z undoes it." },
    [EDIT_TITLE] = { "set_title", "Renamed the note." },
};

//! Make one edit. Returns NULL when it was made, or why it was not.
static const char* edit_note(EditKind kind, const char* text)
{
    if (!note_is_editable())
        return "The note can't be edited right now.";
    if (!text || (kind != EDIT_REPLACE_NOTE && kind != EDIT_REPLACE_SELECTION && !text[0]))
        return "Missing the \"text\" argument.";
    size_t len = gap_len(&app.text);
    switch (kind) {
    case EDIT_REPLACE_NOTE:
        apply_edit(0, len, text);
        break;
    case EDIT_REPLACE_SELECTION: {
        size_t start, end;
        get_selection(&start, &end);
        if (start == end)
            return "Nothing is selected. Ask the user to select the text to change, or use insert_at_cursor.";
        apply_edit(start, end, text);
        break;
    }
    case EDIT_INSERT:
        apply_edit(app.cursor, app.cursor, text);
        break;
    case EDIT_APPEND: {
        // Start the addition on a line of its own, a blank line after whatever the note ends with.
        const char* sep = len == 0 || text[0] == '\n' ? ""
            : gap_at(&app.text, len - 1) != '\n'      ? "\n\n"
            : len > 1 && gap_at(&app.text, len - 2) != '\n' ? "\n"
                                                            : "";
        // and end it with a line break so the user's next words start clean.
        size_t tl = strlen(text);
        const char* end = text[tl - 1] != '\n' ? "\n" : "";
        size_t n = strlen(sep) + tl + strlen(end) + 1;
        char* joined = malloc(n);
        if (!joined)
            return "Out of memory.";
        snprintf(joined, n, "%s%s%s", sep, text, end);
        apply_edit(len, len, joined);
        free(joined);
        break;
    }
    case EDIT_TITLE:
        if (!set_note_title(text))
            return "The title is empty.";
        break;
    default:
        return "Unknown edit.";
    }
    return NULL;
}

static char* edit_tool(EditKind kind, const char* params_json)
{
    char* text = edit_text_param(params_json);
    const char* error = edit_note(kind, text);
    char* result;
    if (error) {
        result = edit_error(error);
    } else {
        char done[160];
        if (kind == EDIT_TITLE)
            snprintf(done, sizeof(done), "Renamed the note to \"%s\".", note_title());
        chat_note_edit(kind == EDIT_TITLE ? done : EDITS[kind].done);
        cJSON* response = cJSON_CreateObject();
        cJSON_AddBoolToObject(response, "ok", true);
        result = cJSON_PrintUnformatted(response);
        cJSON_Delete(response);
    }
    free(text);
    return result;
}

static char* replace_note_tool_callback(const char* p, void* u) { (void)u; return edit_tool(EDIT_REPLACE_NOTE, p); }
static char* replace_selection_tool_callback(const char* p, void* u) { (void)u; return edit_tool(EDIT_REPLACE_SELECTION, p); }
static char* insert_at_cursor_tool_callback(const char* p, void* u) { (void)u; return edit_tool(EDIT_INSERT, p); }
static char* append_to_note_tool_callback(const char* p, void* u) { (void)u; return edit_tool(EDIT_APPEND, p); }
static char* set_title_tool_callback(const char* p, void* u) { (void)u; return edit_tool(EDIT_TITLE, p); }

//! The first complete <tag>…</tag> edit block in from, or false when there is none.
static bool find_edit_block(const char* from, EditKind* kind, const char** open,
    const char** body, const char** close, const char** after)
{
    const char* best = NULL;
    for (int k = 0; k < EDIT_KIND_COUNT; k++) {
        char tag[40];
        snprintf(tag, sizeof(tag), "<%s>", EDITS[k].name);
        const char* at = strstr(from, tag);
        if (!at)
            continue;
        char end_tag[40];
        snprintf(end_tag, sizeof(end_tag), "</%s>", EDITS[k].name);
        const char* end = strstr(at + strlen(tag), end_tag);
        if (!end || (best && at >= best))
            continue;
        best = at;
        *kind = (EditKind)k;
        *open = at;
        *body = at + strlen(tag);
        *close = end;
        *after = end + strlen(end_tag);
    }
    return best != NULL;
}

//! Models that cannot call tools edit by writing <replace_note>…</replace_note> and the like in
//! the reply. Once the reply is complete, make those edits and show what was done in their place.
static void apply_reply_edits(void)
{
    if (app.chat_count == 0 || app.chat_msgs[app.chat_count - 1].is_user)
        return;
    ChatMessage* m = &app.chat_msgs[app.chat_count - 1];
    if (!m->text || !strchr(m->text, '<'))
        return;

    size_t cap = m->len + 1;
    char* shown = malloc(cap);
    if (!shown)
        return;
    size_t shown_len = 0;
    char notes[1024] = "";
    size_t notes_len = 0;

    const char* from = m->text;
    EditKind kind;
    const char *open, *body, *close, *after;
    bool any = false;
    while (find_edit_block(from, &kind, &open, &body, &close, &after)) {
        any = true;
        memcpy(shown + shown_len, from, (size_t)(open - from));
        shown_len += (size_t)(open - from);

        // The block's own line breaks are layout, not content.
        const char* b = body;
        const char* e = close;
        if (b < e && *b == '\n')
            b++;
        if (e > b && e[-1] == '\n')
            e--;
        char* text = malloc((size_t)(e - b) + 1);
        if (text) {
            memcpy(text, b, (size_t)(e - b));
            text[e - b] = '\0';
            drop_carriage_returns(text);
        }
        const char* error = edit_note(kind, text);
        char line[256];
        if (error)
            snprintf(line, sizeof(line), "Couldn't %s: %s", EDITS[kind].name, error);
        else if (kind == EDIT_TITLE)
            snprintf(line, sizeof(line), "Renamed the note to \"%s\".", note_title());
        else
            snprintf(line, sizeof(line), "%s", EDITS[kind].done);
        free(text);
        notes_len += (size_t)snprintf(notes + notes_len, sizeof(notes) - notes_len, "%s%s",
            notes_len ? "\n" : "", line);
        if (notes_len >= sizeof(notes))
            notes_len = sizeof(notes) - 1;
        from = after;
    }
    if (!any) {
        free(shown);
        return;
    }
    size_t rest = strlen(from);
    memcpy(shown + shown_len, from, rest);
    shown_len += rest;
    shown[shown_len] = '\0';

    // Collapse the blank lines the removed blocks leave behind, then put the notes at the end.
    size_t w = 0;
    for (size_t r = 0; r < shown_len; r++) {
        if (shown[r] == '\n' && w >= 2 && shown[w - 1] == '\n' && shown[w - 2] == '\n')
            continue;
        shown[w++] = shown[r];
    }
    while (w > 0 && (shown[w - 1] == '\n' || shown[w - 1] == ' '))
        w--;
    size_t start = 0;
    while (start < w && shown[start] == '\n')
        start++;

    size_t n = (w - start) + notes_len + 3;
    char* text = malloc(n);
    if (text) {
        snprintf(text, n, "%.*s%s%s", (int)(w - start), shown + start, w > start ? "\n\n" : "", notes);
        free(m->text);
        m->text = text;
        m->len = strlen(text);
    }
    free(shown);
}

// #endregion

void ai_send(const char* prompt)
{
    if (!app.ai_ready || !app.ai_ctx)
        return;

    chat_add(prompt, true);
    chat_add("", false);

    app.ai_thinking = true;
    free(g_turn_path);
    g_turn_path = app.session_path ? dawn_strdup(app.session_path) : NULL;

    // The bridge attaches the note to the question itself (read_document's "context" action),
    // so this is only the user's own words, and the session history stays small.
    ai_generation_params_t params = {
        .temperature = 0.7,
        .max_tokens = 4096,
        .include_reasoning = false,
        .seed = 0
    };

    ai_generate_response_stream(app.ai_ctx, app.ai_session, prompt, &params,
        ai_stream_cb, NULL);
}

void ai_init_session(void)
{
    if (app.ai_session || !app.ai_ctx)
        return;

    // Written for the small on-device models TAI serves, which mostly cannot call tools: the note
    // arrives with every message, and edits can be made by writing a tagged block in the reply.
    static const char* instructions = "You are the assistant inside Dawn, a writing app. "
                                      "Every message from the user comes with their open note between <note> tags, and any text they have selected between <selection> tags. "
                                      "When they say \"this\", \"the note\", \"the document\", \"my writing\" or \"what I wrote\", they mean that note: "
                                      "answer from it directly and never ask them to paste or share it. "
                                      "You can also answer general questions about anything.\n\n"

                                      "CHANGING THE NOTE\n"
                                      "Only change the note when the user asks you to; otherwise answer in the chat. "
                                      "To change it, put one of these blocks in your reply:\n"
                                      "<replace_note>the whole new note</replace_note> to rewrite, fix, translate, reformat or reorganize the whole note\n"
                                      "<replace_selection>the new text</replace_selection> to rewrite only the selected text\n"
                                      "<insert_at_cursor>the new text</insert_at_cursor> to add text where the user's cursor is\n"
                                      "<append_to_note>the new text</append_to_note> to add text at the end of the note\n"
                                      "<set_title>a short title</set_title> to rename the note\n"
                                      "Write the new text in Markdown, and after the block say in one short sentence what you changed. "
                                      "Dawn makes the change and the user can undo it with Ctrl+Z. "
                                      "If tools with these same names are available, you may call them instead of writing blocks.\n\n"

                                      "OTHER TOOLS, when available: web_search for facts you are unsure of, get_time for the date and time, "
                                      "past_sessions for the user's earlier notes.\n\n"

                                      "Be conversational, helpful, and concise. Give direct answers. "
                                      "Use **bold** for emphasis and format code with backticks.";

    static const char* tools_json = "["
                                    "{"
                                    "\"name\":\"read_document\","
                                    "\"description\":\"Read the user's current document in the editor. The note already comes with every message; use this for a part beyond what was shown. Actions: 'full' returns entire document, 'selection' returns selected text, 'info' returns document stats (length, selection range, cursor position), 'range' returns text at specific offset/length.\","
                                    "\"input_schema\":{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\"},\"offset\":{\"type\":\"integer\"},\"length\":{\"type\":\"integer\"}},\"required\":[]}"
                                    "},"
                                    "{"
                                    "\"name\":\"web_search\","
                                    "\"description\":\"Search the web for information. Use for any factual questions, current events, research, coding help, how-to guides, definitions, etc.\","
                                    "\"input_schema\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\"}},\"required\":[\"query\"]}"
                                    "},"
                                    "{"
                                    "\"name\":\"get_time\","
                                    "\"description\":\"Get the current date and time.\","
                                    "\"input_schema\":{\"type\":\"object\",\"properties\":{},\"required\":[]}"
                                    "},"
                                    "{"
                                    "\"name\":\"past_sessions\","
                                    "\"description\":\"Access user's past writing sessions. Use action 'list' to see all sessions, or 'read' with a filename to read a specific session.\","
                                    "\"input_schema\":{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\"},\"filename\":{\"type\":\"string\"}},\"required\":[\"action\"]}"
                                    "},"
                                    "{"
                                    "\"name\":\"replace_note\","
                                    "\"description\":\"Replace the whole note with new text. Use when the user asks to rewrite, fix, translate, reformat or reorganize the whole note.\","
                                    "\"input_schema\":{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"description\":\"The whole new note, in Markdown\"}},\"required\":[\"text\"]}"
                                    "},"
                                    "{"
                                    "\"name\":\"replace_selection\","
                                    "\"description\":\"Replace the text the user has selected in the note with new text. Use when the user asks to rewrite, shorten, fix, translate or reformat their selection. Fails when nothing is selected.\","
                                    "\"input_schema\":{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"description\":\"The new text, in Markdown\"}},\"required\":[\"text\"]}"
                                    "},"
                                    "{"
                                    "\"name\":\"insert_at_cursor\","
                                    "\"description\":\"Insert new text into the note at the user's cursor. Use when the user asks you to write, continue, add or draft something in their note.\","
                                    "\"input_schema\":{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"description\":\"The text to insert, in Markdown\"}},\"required\":[\"text\"]}"
                                    "},"
                                    "{"
                                    "\"name\":\"append_to_note\","
                                    "\"description\":\"Add new text at the end of the note, such as a summary or a conclusion.\","
                                    "\"input_schema\":{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"description\":\"The text to add, in Markdown\"}},\"required\":[\"text\"]}"
                                    "},"
                                    "{"
                                    "\"name\":\"set_title\","
                                    "\"description\":\"Rename the note. Use when the user asks for a title or a new name for the note.\","
                                    "\"input_schema\":{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"description\":\"The new title, a few words\"}},\"required\":[\"text\"]}"
                                    "}"
                                    "]";

    ai_session_config_t config = {
        .instructions = instructions,
        .tools_json = tools_json,
        .enable_guardrails = false,
        .prewarm = true
    };

    app.ai_session = ai_create_session(app.ai_ctx, &config);

    if (app.ai_session) {
        ai_register_tool(app.ai_ctx, app.ai_session, "read_document",
            document_tool_callback, NULL);
        ai_register_tool(app.ai_ctx, app.ai_session, "web_search",
            search_tool_callback, NULL);
        ai_register_tool(app.ai_ctx, app.ai_session, "get_time",
            time_tool_callback, NULL);
        ai_register_tool(app.ai_ctx, app.ai_session, "past_sessions",
            sessions_tool_callback, (void*)history_dir());
        ai_register_tool(app.ai_ctx, app.ai_session, "replace_note",
            replace_note_tool_callback, NULL);
        ai_register_tool(app.ai_ctx, app.ai_session, "replace_selection",
            replace_selection_tool_callback, NULL);
        ai_register_tool(app.ai_ctx, app.ai_session, "insert_at_cursor",
            insert_at_cursor_tool_callback, NULL);
        ai_register_tool(app.ai_ctx, app.ai_session, "append_to_note",
            append_to_note_tool_callback, NULL);
        ai_register_tool(app.ai_ctx, app.ai_session, "set_title",
            set_title_tool_callback, NULL);
    }
}

// #region Automatic Title

#define TITLE_MIN_CHARS 160
#define TITLE_NOTE_LIMIT 3000
#define TITLE_RETRY_SECS 60

static ai_session_id_t g_title_session;
static char* g_title_path; //!< The note a title was asked for (or is being asked for)
static char* g_title_reply;
static size_t g_title_reply_len;
static bool g_title_busy;
static bool g_title_failed;
static int64_t g_title_retry_at;

static bool note_is_untitled(void)
{
    const char* title = note_title();
    return !title || !title[0] || strcmp(title, "Untitled") == 0;
}

static void title_stream_cb(ai_context_t* context, const char* chunk, void* user_data)
{
    (void)context;
    (void)user_data;
    if (chunk) {
        if (strncmp(chunk, "Error:", 6) == 0)
            g_title_failed = true;
        else if (strcmp(chunk, "null") != 0 && g_title_reply_len < 1024) {
            size_t n = strlen(chunk);
            char* grown = realloc(g_title_reply, g_title_reply_len + n + 1);
            if (grown) {
                g_title_reply = grown;
                memcpy(g_title_reply + g_title_reply_len, chunk, n + 1);
                g_title_reply_len += n;
            }
        }
        return;
    }

    g_title_busy = false;
    if (g_title_failed || !g_title_reply) {
        // TAI off or busy: try this note again in a while.
        free(g_title_path);
        g_title_path = NULL;
        g_title_retry_at = DAWN_BACKEND(app)->clock(DAWN_CLOCK_SEC) + TITLE_RETRY_SECS;
    } else if (app.session_path && g_title_path && strcmp(app.session_path, g_title_path) == 0
        && note_is_untitled() && set_note_title(g_title_reply)) {
        save_session();
    }
    free(g_title_reply);
    g_title_reply = NULL;
    g_title_reply_len = 0;
}

void ai_title_tick(void)
{
    if (!app.ai_ready || !app.ai_ctx || g_title_busy || app.ai_thinking)
        return;
    if (app.mode != MODE_WRITING || app.preview_mode || !app.session_path || !note_is_untitled())
        return;
    if (g_title_path && strcmp(g_title_path, app.session_path) == 0)
        return; // asked once for this note already
    if (gap_len(&app.text) < TITLE_MIN_CHARS)
        return;
    if (DAWN_BACKEND(app)->clock(DAWN_CLOCK_SEC) < g_title_retry_at)
        return;

    if (!g_title_session) {
        ai_session_config_t config = {
            .instructions = "You name notes. Reply with only a short, specific title for the note the user gives you: "
                            "two to six words, in the note's language, with no quotes, no Markdown and no final period.",
            .tools_json = NULL,
            .enable_guardrails = false,
            .prewarm = false
        };
        g_title_session = ai_create_session(app.ai_ctx, &config);
        if (!g_title_session)
            return;
    } else {
        ai_clear_session_history(app.ai_ctx, g_title_session);
    }

    bool cut;
    char* note = note_slice(0, gap_len(&app.text), TITLE_NOTE_LIMIT, &cut);
    size_t n = strlen(note) + 64;
    char* prompt = malloc(n);
    if (!prompt) {
        free(note);
        return;
    }
    snprintf(prompt, n, "Write a title for this note.\n\n<note>\n%s\n</note>", note);
    free(note);

    ai_generation_params_t params = {
        .temperature = 0.3,
        .max_tokens = 24,
        .include_reasoning = false,
        .seed = 0
    };
    free(g_title_path);
    g_title_path = dawn_strdup(app.session_path);
    g_title_failed = false;
    g_title_busy = true;
    if (!ai_generate_response_stream(app.ai_ctx, g_title_session, prompt, &params, title_stream_cb, NULL)) {
        g_title_busy = false;
        g_title_retry_at = DAWN_BACKEND(app)->clock(DAWN_CLOCK_SEC) + TITLE_RETRY_SECS;
        free(g_title_path);
        g_title_path = NULL;
    }
    free(prompt);
}

// #endregion

#endif // HAS_LIBAI
