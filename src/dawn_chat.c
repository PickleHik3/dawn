// dawn_chat.c

#include "dawn_chat.h"
#include "cJSON.h"
#include "dawn_utils.h"
#include "dawn_file.h"
#include "dawn_gap.h"
#include "dawn_nav.h"
#include "dawn_block.h"

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
}

// #endregion

#if HAS_LIBAI

// #region AI Streaming

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
    }
}

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

//! The "text" argument of an edit call with carriage returns dropped, or NULL if there is none.
static char* edit_text_param(const char* params_json)
{
    cJSON* params = cJSON_Parse(params_json);
    cJSON* text = cJSON_GetObjectItemCaseSensitive(params, "text");
    char* out = NULL;
    if (cJSON_IsString(text) && text->valuestring) {
        out = dawn_strdup(text->valuestring);
        size_t w = 0;
        for (size_t r = 0; out[r]; r++)
            if (out[r] != '\r')
                out[w++] = out[r];
        out[w] = '\0';
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

static bool note_is_editable(void) { return app.mode == MODE_WRITING && !app.focus_mode && !app.preview_mode; }

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

static char* edit_done(size_t removed, size_t inserted)
{
    cJSON* response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "ok", true);
    cJSON_AddNumberToObject(response, "removed_chars", (double)removed);
    cJSON_AddNumberToObject(response, "inserted_chars", (double)inserted);
    char* json = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    return json;
}

static char* replace_selection_tool_callback(const char* params_json, void* user_data)
{
    (void)user_data;
    if (!note_is_editable())
        return edit_error("The note can't be edited right now.");
    size_t start, end;
    get_selection(&start, &end);
    if (start == end)
        return edit_error("Nothing is selected. Ask the user to select the text to change, or use insert_at_cursor.");
    char* text = edit_text_param(params_json);
    if (!text)
        return edit_error("Missing the \"text\" argument.");
    apply_edit(start, end, text);
    chat_note_edit("Replaced the selected text. Ctrl+Z undoes it.");
    char* result = edit_done(end - start, strlen(text));
    free(text);
    return result;
}

static char* insert_at_cursor_tool_callback(const char* params_json, void* user_data)
{
    (void)user_data;
    if (!note_is_editable())
        return edit_error("The note can't be edited right now.");
    char* text = edit_text_param(params_json);
    if (!text || !text[0]) {
        free(text);
        return edit_error("Missing the \"text\" argument.");
    }
    apply_edit(app.cursor, app.cursor, text);
    chat_note_edit("Added text at the cursor. Ctrl+Z undoes it.");
    char* result = edit_done(0, strlen(text));
    free(text);
    return result;
}

// #endregion

void ai_send(const char* prompt)
{
    if (!app.ai_ready || !app.ai_ctx)
        return;

    chat_add(prompt, true);
    chat_add("", false);

    app.ai_thinking = true;

    // Just send the prompt directly - AI can use tools to get context if needed
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

    // System prompt - be a helpful general assistant with tools
    static const char* instructions = "You are a helpful AI assistant in a writing app called Dawn. "
                                      "You can answer any questions the user asks - about their writing, general knowledge, coding, research, or anything else.\n\n"

                                      "TOOLS AVAILABLE:\n"
                                      "- read_document: Read the user's current document. Use this when they ask about 'my writing', 'this text', 'what I wrote', etc.\n"
                                      "- web_search: Search the web for current information. Use for facts, news, research, how-to questions, etc.\n"
                                      "- get_time: Get the current date and time.\n"
                                      "- past_sessions: Access the user's previous writing sessions.\n"
                                      "- replace_selection: Replace the text the user has selected with new text.\n"
                                      "- insert_at_cursor: Insert new text into the note at the user's cursor.\n\n"

                                      "WHEN TO USE TOOLS:\n"
                                      "- If the user mentions their writing/document/text -> use read_document first\n"
                                      "- If you need factual info you're unsure about -> use web_search\n"
                                      "- If asked about time/date -> use get_time\n"
                                      "- If asked about previous/past writing -> use past_sessions\n"
                                      "- If asked to rewrite, shorten, fix or reformat the selected text -> use replace_selection\n"
                                      "- If asked to write, continue or add something to the note -> use insert_at_cursor\n"
                                      "- Only change the note when the user asks you to; otherwise answer in the chat\n"
                                      "- After an edit, say in one sentence what you changed\n\n"

                                      "Be conversational, helpful, and concise. Give direct answers. "
                                      "Use **bold** for emphasis and format code with backticks.";

    static const char* tools_json = "["
                                    "{"
                                    "\"name\":\"read_document\","
                                    "\"description\":\"Read the user's current document in the editor. Actions: 'full' returns entire document, 'selection' returns selected text, 'info' returns document stats (length, selection range, cursor position), 'range' returns text at specific offset/length.\","
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
                                    "\"name\":\"replace_selection\","
                                    "\"description\":\"Replace the text the user has selected in the note with new text. Use when the user asks to rewrite, shorten, fix, translate or reformat their selection. Fails when nothing is selected.\","
                                    "\"input_schema\":{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"description\":\"The new text, in Markdown\"}},\"required\":[\"text\"]}"
                                    "},"
                                    "{"
                                    "\"name\":\"insert_at_cursor\","
                                    "\"description\":\"Insert new text into the note at the user's cursor. Use when the user asks you to write, continue, add or draft something in their note.\","
                                    "\"input_schema\":{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\",\"description\":\"The text to insert, in Markdown\"}},\"required\":[\"text\"]}"
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
        ai_register_tool(app.ai_ctx, app.ai_session, "replace_selection",
            replace_selection_tool_callback, NULL);
        ai_register_tool(app.ai_ctx, app.ai_session, "insert_at_cursor",
            insert_at_cursor_tool_callback, NULL);
    }
}

#endif // HAS_LIBAI
