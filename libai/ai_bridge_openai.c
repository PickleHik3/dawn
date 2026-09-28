/**
 * @file ai_bridge_openai.c
 * @brief ai_bridge.h over an OpenAI-compatible Chat Completions endpoint.
 *
 * Replaces the FoundationModels bridge on every platform that is not macOS. The endpoint is
 * Termux Launcher's TAI by default, or any OpenAI-compatible server named in
 * <config>/dawn/ai.json:
 *
 *   { "provider": "tai", "model": "optional-model-id" }
 *   { "provider": "openai", "base_url": "https://host/v1", "api_key": "optional", "model": "id" }
 *
 * The file is only ever read, and it is read again for every question, so an edit takes effect
 * on the next message. In TAI mode the address comes from the first line of
 * ~/.launcherctl/endpoint and the key from ~/.launcherctl/token, both written by the launcher;
 * the token is optional because the launcher can serve without one.
 *
 * Threading: each question runs on its own worker thread, but dawn's state is only ever touched
 * on dawn's own thread. Response chunks are queued and delivered from ai_bridge_pump(), which the
 * frame loop calls, and tools that read or change the document run there too while the worker
 * waits. Tools that only reach the network or the clock run on the worker, so a slow web search
 * does not freeze typing. Progress reports (waiting, writing, which tool runs) travel the same
 * queue, and a stop request is a flag the worker checks between chunks, between tool calls and
 * from curl's progress callback.
 */

#include "ai_bridge.h"
#include "ai_speak.h"

#include "cJSON.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define MAX_SESSIONS 254
#define MAX_STREAMS 254
#define MAX_TOOL_ROUNDS 5
#define MAX_TOOL_CALLS 8
#define DOC_CONTEXT_LIMIT 8000

// #region Small helpers

typedef struct {
    char* data;
    size_t len;
    size_t cap;
} strbuf_t;

static void sb_append(strbuf_t* sb, const char* s, size_t n)
{
    if (!s || n == 0)
        return;
    if (sb->len + n + 1 > sb->cap) {
        size_t cap = sb->cap ? sb->cap : 256;
        while (sb->len + n + 1 > cap)
            cap *= 2;
        char* grown = realloc(sb->data, cap);
        if (!grown)
            return;
        sb->data = grown;
        sb->cap = cap;
    }
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

static void sb_free(strbuf_t* sb)
{
    free(sb->data);
    sb->data = NULL;
    sb->len = sb->cap = 0;
}

static char* dup_printf(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (n < 0)
        return NULL;
    char* out = malloc((size_t)n + 1);
    if (!out)
        return NULL;
    va_start(args, fmt);
    vsnprintf(out, (size_t)n + 1, fmt, args);
    va_end(args);
    return out;
}

static char* dup_str(const char* s) { return s ? dup_printf("%s", s) : NULL; }

//! Reads a small text file whole, or returns NULL when it is missing or empty.
static char* read_text_file(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return NULL;
    strbuf_t sb = { 0 };
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && sb.len < (1u << 20))
        sb_append(&sb, buf, n);
    fclose(f);
    return sb.data;
}

//! The first line of s with surrounding whitespace removed, in place.
static char* first_line_trimmed(char* s)
{
    if (!s)
        return NULL;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    char* end = s;
    while (*end && *end != '\n' && *end != '\r')
        end++;
    while (end > s && (end[-1] == ' ' || end[-1] == '\t'))
        end--;
    *end = '\0';
    return s;
}

// #endregion

// #region Configuration

typedef struct {
    bool tai;
    char* base_url; //!< Ends in /v1 (or wherever chat/completions lives), no trailing slash
    char* api_key; //!< NULL when none is configured
    char* model; //!< NULL to let the server choose
    char* error; //!< Set instead of the rest when the configuration cannot be used
} config_t;

static void config_free(config_t* c)
{
    free(c->base_url);
    free(c->api_key);
    free(c->model);
    free(c->error);
    memset(c, 0, sizeof(*c));
}

static char* config_path(void)
{
    const char* xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && xdg[0] == '/')
        return dup_printf("%s/dawn/ai.json", xdg);
    const char* home = getenv("HOME");
    return dup_printf("%s/.config/dawn/ai.json", home ? home : ".");
}

static char* json_string(cJSON* obj, const char* key)
{
    cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0])
        return NULL;
    return dup_str(item->valuestring);
}

static void strip_trailing_slashes(char* s)
{
    size_t n = s ? strlen(s) : 0;
    while (n > 0 && s[n - 1] == '/')
        s[--n] = '\0';
}

static config_t config_load(void)
{
    config_t c = { 0 };
    char* path = config_path();
    char* text = path ? read_text_file(path) : NULL;
    cJSON* root = NULL;
    char* provider = NULL;

    if (text) {
        root = cJSON_Parse(text);
        if (!cJSON_IsObject(root)) {
            c.error = dup_printf("Error: %s isn't valid JSON.", path);
            goto done;
        }
        provider = json_string(root, "provider");
        c.model = json_string(root, "model");
    }

    if (!provider || strcmp(provider, "tai") == 0) {
        c.tai = true;
        const char* home = getenv("HOME");
        char* endpoint_path = dup_printf("%s/.launcherctl/endpoint", home ? home : ".");
        char* token_path = dup_printf("%s/.launcherctl/token", home ? home : ".");
        char* endpoint = read_text_file(endpoint_path);
        char* token = read_text_file(token_path);
        char* base = first_line_trimmed(endpoint);
        char* key = first_line_trimmed(token);
        if (base && base[0]) {
            strip_trailing_slashes(base);
            c.base_url = dup_printf("%s/v1", base);
        } else {
            c.error = dup_str("Error: Can't find TAI. Turn on AI in Termux Launcher's settings.");
        }
        if (key && key[0])
            c.api_key = dup_str(key);
        free(endpoint);
        free(token);
        free(endpoint_path);
        free(token_path);
    } else if (strcmp(provider, "openai") == 0) {
        c.base_url = json_string(root, "base_url");
        c.api_key = json_string(root, "api_key");
        if (c.base_url)
            strip_trailing_slashes(c.base_url);
        else
            c.error = dup_printf("Error: Add a base_url to %s.", path);
    } else {
        c.error = dup_printf("Error: Unknown provider \"%s\" in %s. Use \"tai\" or \"openai\".",
            provider, path);
    }

done:
    free(provider);
    cJSON_Delete(root);
    free(text);
    free(path);
    return c;
}

bool ai_bridge_endpoint(char** base_url, char** api_key, char** error)
{
    config_t c = config_load();
    bool ok = !c.error && c.tai && c.base_url;
    if (error) {
        if (c.error)
            *error = dup_str(c.error);
        else if (!c.tai)
            *error = dup_str("Error: Read aloud needs Termux Launcher's TAI.");
        else
            *error = NULL;
    }
    if (ok) {
        if (base_url) {
            *base_url = c.base_url;
            c.base_url = NULL;
        }
        if (api_key) {
            *api_key = c.api_key;
            c.api_key = NULL;
        }
    }
    config_free(&c);
    return ok;
}

// #endregion

// #region Runtime status, context window, usage (P1 "AI foundations")

static pthread_mutex_t g_runtime_lock = PTHREAD_MUTEX_INITIALIZER;

static ai_bridge_model_state_t g_model_state = AI_BRIDGE_MODEL_UNKNOWN;
static int64_t g_model_state_checked_at_ms; //!< 0 = never
static bool g_model_state_fetching;

static int32_t g_context_window; //!< 0 = not fetched yet
static bool g_context_window_fetching;

static int32_t g_usage_prompt_tokens = -1; //!< -1 = none waiting
static int32_t g_usage_completion_tokens = -1;

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static size_t write_to_strbuf(char* data, size_t size, size_t count, void* userp)
{
    strbuf_t* sb = userp;
    size_t n = size * count;
    sb_append(sb, data, n);
    return n;
}

//! One quick GET, reusing the configured endpoint. Body is returned malloc'd (caller frees), or
//! NULL on any failure; *status_out gets the HTTP status when the request completed at all.
static char* http_get(const config_t* cfg, const char* path, long* status_out)
{
    CURL* curl = curl_easy_init();
    if (!curl)
        return NULL;
    char* url = dup_printf("%s%s", cfg->base_url, path);
    struct curl_slist* headers = NULL;
    char* auth = cfg->api_key ? dup_printf("Authorization: Bearer %s", cfg->api_key) : NULL;
    if (auth)
        headers = curl_slist_append(headers, auth);
    strbuf_t body = { 0 };
    curl_easy_setopt(curl, CURLOPT_URL, url);
    if (headers)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_strbuf);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "dawn");
    CURLcode rc = curl_easy_perform(curl);
    if (status_out)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status_out);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(auth);
    free(url);
    if (rc != CURLE_OK) {
        sb_free(&body);
        return NULL;
    }
    return body.data ? body.data : dup_str("");
}

//! A number that might be at the top level of root, or (the first one found) inside a "data"
//! array of objects, since it is unclear which shape the endpoint uses for this field.
static bool find_number_field(cJSON* root, const char* key, double* out)
{
    cJSON* item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(item)) {
        *out = item->valuedouble;
        return true;
    }
    cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON* entry;
    cJSON_ArrayForEach(entry, data)
    {
        item = cJSON_GetObjectItemCaseSensitive(entry, key);
        if (cJSON_IsNumber(item)) {
            *out = item->valuedouble;
            return true;
        }
    }
    return false;
}

static void* fetch_context_window_thread(void* arg)
{
    (void)arg;
    config_t cfg = config_load();
    if (!cfg.error) {
        char* body = http_get(&cfg, "/models", NULL);
        cJSON* root = body ? cJSON_Parse(body) : NULL;
        double window;
        if (root && find_number_field(root, "_endpoint_context_window", &window) && window > 0) {
            pthread_mutex_lock(&g_runtime_lock);
            g_context_window = (int32_t)window;
            pthread_mutex_unlock(&g_runtime_lock);
        }
        cJSON_Delete(root);
        free(body);
    }
    config_free(&cfg);
    pthread_mutex_lock(&g_runtime_lock);
    g_context_window_fetching = false;
    pthread_mutex_unlock(&g_runtime_lock);
    return NULL;
}

int32_t ai_bridge_context_window(void)
{
    pthread_mutex_lock(&g_runtime_lock);
    int32_t window = g_context_window;
    bool start = false;
    if (window == 0 && !g_context_window_fetching) {
        g_context_window_fetching = true;
        start = true;
    }
    pthread_mutex_unlock(&g_runtime_lock);
    if (start) {
        pthread_t t;
        if (pthread_create(&t, NULL, fetch_context_window_thread, NULL) == 0)
            pthread_detach(t);
        else {
            pthread_mutex_lock(&g_runtime_lock);
            g_context_window_fetching = false;
            pthread_mutex_unlock(&g_runtime_lock);
        }
    }
    return window > 0 ? window : 4096; // TAI's common default until the real figure lands
}

//! A case-insensitive strstr(): plain strcasestr() is not on every platform this file builds for
//! (mingw/MSVC included), so a small one of our own avoids depending on it.
static bool contains_ci(const char* haystack, const char* needle)
{
    if (!haystack || !needle || !needle[0])
        return false;
    size_t hn = strlen(haystack), nn = strlen(needle);
    for (size_t i = 0; i + nn <= hn; i++)
        if (strncasecmp(haystack + i, needle, nn) == 0)
            return true;
    return false;
}

//! Whatever field name and shape /v1/ai/runtime (or /v1/ai/status) turns out to use for "a model
//! is resident", this looks for the likely ones. Anything not recognized stays UNKNOWN, which the
//! caller treats the same as "not loaded" (never trigger a load for a QUIET job).
static ai_bridge_model_state_t model_state_from(cJSON* root)
{
    if (!root)
        return AI_BRIDGE_MODEL_UNKNOWN;
    // TAI's /v1/ai/runtime answers {"ok":true,"runtime":{"loaded":…,"loadedModelId":…,"state":…}}
    // (TaiManager.runtimeStatus, TaiRuntimeState.toJson): the model state is one level down.
    cJSON* nested = cJSON_GetObjectItemCaseSensitive(root, "runtime");
    if (cJSON_IsObject(nested))
        root = nested;
    static const char* const bool_keys[] = { "loaded", "model_loaded", "is_loaded", "resident" };
    for (size_t i = 0; i < sizeof(bool_keys) / sizeof(bool_keys[0]); i++) {
        cJSON* v = cJSON_GetObjectItemCaseSensitive(root, bool_keys[i]);
        if (cJSON_IsBool(v))
            return cJSON_IsTrue(v) ? AI_BRIDGE_MODEL_LOADED : AI_BRIDGE_MODEL_NOT_LOADED;
    }
    cJSON* state = cJSON_GetObjectItemCaseSensitive(root, "state");
    if (!cJSON_IsString(state))
        state = cJSON_GetObjectItemCaseSensitive(root, "status");
    if (cJSON_IsString(state)) {
        if (contains_ci(state->valuestring, "load") && !contains_ci(state->valuestring, "unload")
            && !contains_ci(state->valuestring, "not"))
            return AI_BRIDGE_MODEL_LOADED;
        if (contains_ci(state->valuestring, "idle") || contains_ci(state->valuestring, "unload")
            || contains_ci(state->valuestring, "none") || contains_ci(state->valuestring, "empty"))
            return AI_BRIDGE_MODEL_NOT_LOADED;
    }
    cJSON* model = cJSON_GetObjectItemCaseSensitive(root, "model");
    if (!model)
        model = cJSON_GetObjectItemCaseSensitive(root, "current_model");
    if (model)
        return cJSON_IsString(model) && model->valuestring[0] ? AI_BRIDGE_MODEL_LOADED : AI_BRIDGE_MODEL_NOT_LOADED;
    return AI_BRIDGE_MODEL_UNKNOWN;
}

static void* fetch_model_state_thread(void* arg)
{
    (void)arg;
    config_t cfg = config_load();
    ai_bridge_model_state_t found = AI_BRIDGE_MODEL_UNKNOWN;
    if (!cfg.error) {
        static const char* const paths[] = { "/ai/runtime", "/ai/status" };
        for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]) && found == AI_BRIDGE_MODEL_UNKNOWN; i++) {
            long status = 0;
            char* body = http_get(&cfg, paths[i], &status);
            if (body && status >= 200 && status < 300) {
                cJSON* root = cJSON_Parse(body);
                found = model_state_from(root);
                cJSON_Delete(root);
            }
            free(body);
        }
    }
    config_free(&cfg);
    pthread_mutex_lock(&g_runtime_lock);
    g_model_state = found;
    g_model_state_checked_at_ms = now_ms();
    g_model_state_fetching = false;
    pthread_mutex_unlock(&g_runtime_lock);
    return NULL;
}

ai_bridge_model_state_t ai_bridge_runtime_state(void)
{
    pthread_mutex_lock(&g_runtime_lock);
    ai_bridge_model_state_t state = g_model_state;
    bool stale = now_ms() - g_model_state_checked_at_ms > 3000;
    bool start = stale && !g_model_state_fetching;
    if (start)
        g_model_state_fetching = true;
    pthread_mutex_unlock(&g_runtime_lock);
    if (start) {
        pthread_t t;
        if (pthread_create(&t, NULL, fetch_model_state_thread, NULL) == 0)
            pthread_detach(t);
        else {
            pthread_mutex_lock(&g_runtime_lock);
            g_model_state_fetching = false;
            pthread_mutex_unlock(&g_runtime_lock);
        }
    }
    return state;
}

static void* post_cancel_thread(void* arg)
{
    (void)arg;
    config_t cfg = config_load();
    if (!cfg.error) {
        CURL* curl = curl_easy_init();
        if (curl) {
            char* url = dup_printf("%s/ai/runtime/cancel", cfg.base_url);
            struct curl_slist* headers = NULL;
            headers = curl_slist_append(headers, "Content-Type: application/json");
            char* auth = cfg.api_key ? dup_printf("Authorization: Bearer %s", cfg.api_key) : NULL;
            if (auth)
                headers = curl_slist_append(headers, auth);
            curl_easy_setopt(curl, CURLOPT_URL, url);
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            curl_easy_perform(curl); // best-effort; nothing to do if this fails
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            free(auth);
            free(url);
        }
    }
    config_free(&cfg);
    return NULL;
}

//! Fire-and-forget POST /v1/ai/runtime/cancel. Stopping the local stream (the atomic flag and
//! curl's abort) leaves the server still generating; TAI serves one generation at a time, so a
//! reply the user stopped would otherwise still block the next request until it finishes.
static void post_runtime_cancel_async(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, post_cancel_thread, NULL) == 0)
        pthread_detach(t);
}

bool ai_bridge_take_usage(int32_t* prompt_tokens, int32_t* completion_tokens)
{
    bool ok = false;
    pthread_mutex_lock(&g_runtime_lock);
    if (g_usage_prompt_tokens >= 0) {
        if (prompt_tokens)
            *prompt_tokens = g_usage_prompt_tokens;
        if (completion_tokens)
            *completion_tokens = g_usage_completion_tokens;
        g_usage_prompt_tokens = -1;
        g_usage_completion_tokens = -1;
        ok = true;
    }
    pthread_mutex_unlock(&g_runtime_lock);
    return ok;
}

static void store_usage(int32_t prompt_tokens, int32_t completion_tokens)
{
    if (prompt_tokens < 0 && completion_tokens < 0)
        return;
    pthread_mutex_lock(&g_runtime_lock);
    g_usage_prompt_tokens = prompt_tokens;
    g_usage_completion_tokens = completion_tokens;
    pthread_mutex_unlock(&g_runtime_lock);
}

//! chars/3.6 for mostly-Latin text, chars/2.5 when the text is mostly Arabic script. Kept in sync
//! by hand with dawn_ai_tokens.c's copy (dawn's own estimator, used for the note-context budget):
//! this one only needs to size the session history against the context window, a much smaller
//! and purely internal use that does not warrant sharing a header across the src/libai split.
static int32_t estimate_tokens_rough(const char* text)
{
    if (!text)
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
            p++;
            continue;
        }
        for (int i = 1; i < len && p[i]; i++)
            cp = (cp << 6) | (p[i] & 0x3F);
        p += len;
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

// #region Sessions and streams

typedef struct {
    char* name;
    ai_bridge_tool_callback_t callback;
    void* user_data;
} tool_t;

typedef struct {
    bool used;
    bool closing; //!< Destroyed by the caller; freed once no stream holds it
    int32_t refs;
    char* instructions;
    cJSON* tools; //!< OpenAI "tools" array, or NULL when the session has none
    tool_t registered[16];
    int32_t registered_count;
    cJSON* history; //!< user / assistant messages, without the system message
    bool tools_refused; //!< The endpoint refused tools once; ask without them from then on
    bool told_tools_refused;
    ai_bridge_progress_callback_t progress; //!< NULL when the caller does not want reports
    void* progress_data;
} session_t;

typedef struct {
    bool used;
    atomic_bool cancel;
    uint8_t session;
    char* prompt;
    double temperature;
    int32_t max_tokens;
    void* context;
    ai_bridge_stream_callback_t callback;
    void* user_data;
    bool inline_calls; //!< Synchronous generation: call back directly on the caller's thread
    strbuf_t collected; //!< Synchronous generation: the whole reply
} stream_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
// Indexed by the one-byte ids the header hands out, so every possible id is in bounds.
static session_t g_sessions[256];
static stream_t g_streams[256];
static bool g_curl_ready;

static void session_release_locked(uint8_t id)
{
    session_t* s = &g_sessions[id];
    if (--s->refs > 0 || !s->closing)
        return;
    free(s->instructions);
    cJSON_Delete(s->tools);
    cJSON_Delete(s->history);
    for (int32_t i = 0; i < s->registered_count; i++)
        free(s->registered[i].name);
    memset(s, 0, sizeof(*s));
}

//! Converts dawn's Claude-style tool list ({name, description, input_schema}) to OpenAI's.
static cJSON* openai_tools_from(const char* tools_json)
{
    cJSON* source = tools_json ? cJSON_Parse(tools_json) : NULL;
    if (!cJSON_IsArray(source) || cJSON_GetArraySize(source) == 0) {
        cJSON_Delete(source);
        return NULL;
    }
    cJSON* tools = cJSON_CreateArray();
    cJSON* item;
    cJSON_ArrayForEach(item, source)
    {
        cJSON* name = cJSON_GetObjectItemCaseSensitive(item, "name");
        if (!cJSON_IsString(name))
            continue;
        cJSON* fn = cJSON_CreateObject();
        cJSON_AddStringToObject(fn, "name", name->valuestring);
        cJSON* desc = cJSON_GetObjectItemCaseSensitive(item, "description");
        if (cJSON_IsString(desc))
            cJSON_AddStringToObject(fn, "description", desc->valuestring);
        cJSON* schema = cJSON_GetObjectItemCaseSensitive(item, "input_schema");
        if (!schema)
            schema = cJSON_GetObjectItemCaseSensitive(item, "parameters");
        cJSON_AddItemToObject(fn, "parameters",
            schema ? cJSON_Duplicate(schema, true) : cJSON_Parse("{\"type\":\"object\",\"properties\":{}}"));
        cJSON* tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "type", "function");
        cJSON_AddItemToObject(tool, "function", fn);
        cJSON_AddItemToArray(tools, tool);
    }
    cJSON_Delete(source);
    return tools;
}

// #endregion

// #region Main-thread queue

typedef enum { EVENT_CHUNK, EVENT_CALL, EVENT_PROGRESS } event_kind_t;

typedef struct event {
    event_kind_t kind;
    struct event* next;
    // EVENT_CHUNK
    ai_bridge_stream_callback_t stream_callback;
    void* context;
    void* user_data;
    char* chunk; //!< NULL marks the end of the reply
    // EVENT_CALL
    stream_t* stream; //!< The stream waiting for the result; a cancelled one gets none
    ai_bridge_tool_callback_t tool_callback;
    char* parameters;
    char* result;
    bool done;
    bool abandoned; //!< The worker stopped waiting; whoever finishes last frees the event
    // EVENT_PROGRESS
    ai_bridge_progress_callback_t progress_callback;
    int32_t phase;
    char* tool;
    int32_t step;
    int32_t steps;
} event_t;

static event_t* g_queue_head;
static event_t* g_queue_tail;

static void enqueue_locked(event_t* e)
{
    e->next = NULL;
    if (g_queue_tail)
        g_queue_tail->next = e;
    else
        g_queue_head = e;
    g_queue_tail = e;
}

static void emit(stream_t* st, const char* chunk)
{
    if (st->inline_calls) {
        if (chunk && strncmp(chunk, "Error:", 6) != 0)
            sb_append(&st->collected, chunk, strlen(chunk));
        else if (chunk) {
            sb_free(&st->collected);
            sb_append(&st->collected, chunk, strlen(chunk));
        }
        return;
    }
    event_t* e = calloc(1, sizeof(*e));
    if (!e)
        return;
    e->kind = EVENT_CHUNK;
    e->stream_callback = st->callback;
    e->context = st->context;
    e->user_data = st->user_data;
    e->chunk = dup_str(chunk);
    pthread_mutex_lock(&g_lock);
    enqueue_locked(e);
    pthread_mutex_unlock(&g_lock);
}

//! Tells the session's progress callback, if it has one, what the stream is doing now. Reports
//! queue behind the chunks already sent, so the caller sees them in order.
static void progress(stream_t* st, ai_bridge_progress_t phase, const char* tool, int32_t step,
    int32_t steps)
{
    if (st->inline_calls)
        return;
    pthread_mutex_lock(&g_lock);
    session_t* s = &g_sessions[st->session];
    if (s->progress) {
        event_t* e = calloc(1, sizeof(*e));
        if (e) {
            e->kind = EVENT_PROGRESS;
            e->progress_callback = s->progress;
            e->context = st->context;
            e->user_data = s->progress_data;
            e->phase = (int32_t)phase;
            e->tool = dup_str(tool);
            e->step = step;
            e->steps = steps;
            enqueue_locked(e);
        }
    }
    pthread_mutex_unlock(&g_lock);
}

void ai_bridge_pump(void)
{
    pthread_mutex_lock(&g_lock);
    event_t* e = g_queue_head;
    g_queue_head = g_queue_tail = NULL;
    pthread_mutex_unlock(&g_lock);

    while (e) {
        event_t* next = e->next;
        if (e->kind == EVENT_CHUNK) {
            e->stream_callback(e->context, e->chunk, e->user_data);
            free(e->chunk);
            free(e);
        } else if (e->kind == EVENT_PROGRESS) {
            e->progress_callback(e->context, e->phase, e->tool, e->step, e->steps, e->user_data);
            free(e->tool);
            free(e);
        } else {
            // A stopped stream's tool never runs: the user asked for no more changes, and the
            // worker may not have noticed the flag yet.
            pthread_mutex_lock(&g_lock);
            bool skip = e->abandoned || atomic_load(&e->stream->cancel);
            pthread_mutex_unlock(&g_lock);
            char* result = skip ? NULL : e->tool_callback(e->parameters, e->user_data);
            pthread_mutex_lock(&g_lock);
            if (e->abandoned) {
                free(result);
                free(e->parameters);
                free(e);
            } else {
                e->result = result;
                e->done = true;
                pthread_cond_broadcast(&g_cond);
            }
            pthread_mutex_unlock(&g_lock);
        }
        e = next;
    }
}

//! Tools that only reach the network or the clock; everything else touches the document.
static bool runs_on_worker(const char* name)
{
    return strcmp(name, "web_search") == 0 || strcmp(name, "get_time") == 0
        || strcmp(name, "past_sessions") == 0;
}

static char* call_tool(stream_t* st, const tool_t* tool, const char* parameters)
{
    if (st->inline_calls || runs_on_worker(tool->name))
        return tool->callback(parameters, tool->user_data);

    event_t* e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    e->kind = EVENT_CALL;
    e->stream = st;
    e->tool_callback = tool->callback;
    e->user_data = tool->user_data;
    e->parameters = dup_str(parameters);

    pthread_mutex_lock(&g_lock);
    enqueue_locked(e);
    while (!e->done && !atomic_load(&st->cancel)) {
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += 100 * 1000 * 1000;
        if (until.tv_nsec >= 1000000000L) {
            until.tv_sec++;
            until.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&g_cond, &g_lock, &until);
    }
    char* result = NULL;
    if (e->done) {
        result = e->result;
        free(e->parameters);
        free(e);
    } else {
        e->abandoned = true; // the pump frees it
    }
    pthread_mutex_unlock(&g_lock);
    return result;
}

// #endregion

// #region One HTTP exchange

typedef struct {
    char* id;
    strbuf_t name;
    strbuf_t arguments;
} tool_call_t;

typedef struct {
    stream_t* stream;
    strbuf_t raw; //!< Everything received, for error bodies and non-streaming replies
    strbuf_t line;
    strbuf_t content;
    tool_call_t calls[MAX_TOOL_CALLS];
    int32_t call_count;
    bool saw_event;
    bool told_writing; //!< Progress: the first reply text has arrived
    char* stream_error;
    int32_t usage_prompt_tokens; //!< -1 when no "usage" object was seen this exchange
    int32_t usage_completion_tokens;
} exchange_t;

static void exchange_free(exchange_t* x)
{
    sb_free(&x->raw);
    sb_free(&x->line);
    sb_free(&x->content);
    for (int32_t i = 0; i < x->call_count; i++) {
        free(x->calls[i].id);
        sb_free(&x->calls[i].name);
        sb_free(&x->calls[i].arguments);
    }
    free(x->stream_error);
}

static char* error_message_from(cJSON* root)
{
    cJSON* err = cJSON_GetObjectItemCaseSensitive(root, "error");
    cJSON* msg = cJSON_IsObject(err) ? cJSON_GetObjectItemCaseSensitive(err, "message") : NULL;
    if (!cJSON_IsString(msg))
        msg = cJSON_GetObjectItemCaseSensitive(root, "message");
    if (!cJSON_IsString(msg) && cJSON_IsString(err))
        msg = err;
    return cJSON_IsString(msg) ? dup_str(msg->valuestring) : NULL;
}

//! Folds one message or delta ({content, tool_calls}) into the exchange.
static void absorb_delta(exchange_t* x, cJSON* delta, bool streaming)
{
    cJSON* content = cJSON_GetObjectItemCaseSensitive(delta, "content");
    if (cJSON_IsString(content) && content->valuestring[0]) {
        sb_append(&x->content, content->valuestring, strlen(content->valuestring));
        if (streaming) {
            if (!x->told_writing) {
                x->told_writing = true;
                progress(x->stream, AI_BRIDGE_PROGRESS_WRITING, NULL, 0, 0);
            }
            emit(x->stream, content->valuestring);
        }
    }

    cJSON* calls = cJSON_GetObjectItemCaseSensitive(delta, "tool_calls");
    cJSON* call;
    int32_t position = 0;
    cJSON_ArrayForEach(call, calls)
    {
        cJSON* index = cJSON_GetObjectItemCaseSensitive(call, "index");
        int32_t i = cJSON_IsNumber(index) ? index->valueint : position;
        position++;
        if (i < 0 || i >= MAX_TOOL_CALLS)
            continue;
        if (i >= x->call_count)
            x->call_count = i + 1;
        tool_call_t* tc = &x->calls[i];
        cJSON* id = cJSON_GetObjectItemCaseSensitive(call, "id");
        if (cJSON_IsString(id) && id->valuestring[0] && !tc->id)
            tc->id = dup_str(id->valuestring);
        cJSON* fn = cJSON_GetObjectItemCaseSensitive(call, "function");
        cJSON* name = cJSON_GetObjectItemCaseSensitive(fn, "name");
        cJSON* args = cJSON_GetObjectItemCaseSensitive(fn, "arguments");
        if (cJSON_IsString(name)) {
            bool named = tc->name.len > 0;
            sb_append(&tc->name, name->valuestring, strlen(name->valuestring));
            // The arguments of an edit are the new text itself, the longest silence of a turn
            // with tools: say what is being written as soon as the call has a name.
            if (streaming && !named && tc->name.len > 0)
                progress(x->stream, AI_BRIDGE_PROGRESS_TOOL_ARGS, tc->name.data, i + 1, x->call_count);
        }
        if (cJSON_IsString(args))
            sb_append(&tc->arguments, args->valuestring, strlen(args->valuestring));
        else if (cJSON_IsObject(args)) {
            char* printed = cJSON_PrintUnformatted(args);
            if (printed)
                sb_append(&tc->arguments, printed, strlen(printed));
            free(printed);
        }
    }
}

static void handle_sse_line(exchange_t* x, char* line)
{
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' '))
        line[--n] = '\0';
    if (strncmp(line, "data:", 5) != 0)
        return;
    char* payload = line + 5;
    while (*payload == ' ')
        payload++;
    x->saw_event = true;
    if (strcmp(payload, "[DONE]") == 0)
        return;
    cJSON* root = cJSON_Parse(payload);
    if (!root)
        return;
    if (cJSON_GetObjectItemCaseSensitive(root, "error") && !x->stream_error)
        x->stream_error = error_message_from(root);
    cJSON* choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    cJSON* choice = cJSON_GetArrayItem(choices, 0);
    cJSON* delta = cJSON_GetObjectItemCaseSensitive(choice, "delta");
    if (cJSON_IsObject(delta))
        absorb_delta(x, delta, true);
    // With "stream_options":{"include_usage":true} the final chunk (choices: []) carries usage.
    cJSON* usage = cJSON_GetObjectItemCaseSensitive(root, "usage");
    if (cJSON_IsObject(usage)) {
        cJSON* pt = cJSON_GetObjectItemCaseSensitive(usage, "prompt_tokens");
        cJSON* ct = cJSON_GetObjectItemCaseSensitive(usage, "completion_tokens");
        if (cJSON_IsNumber(pt))
            x->usage_prompt_tokens = pt->valueint;
        if (cJSON_IsNumber(ct))
            x->usage_completion_tokens = ct->valueint;
    }
    cJSON_Delete(root);
}

static size_t on_body(char* data, size_t size, size_t count, void* userp)
{
    exchange_t* x = userp;
    size_t n = size * count;
    sb_append(&x->raw, data, n);
    for (size_t i = 0; i < n; i++) {
        if (data[i] == '\n') {
            if (x->line.data) {
                handle_sse_line(x, x->line.data);
                x->line.len = 0;
                x->line.data[0] = '\0';
            }
        } else {
            sb_append(&x->line, &data[i], 1);
        }
    }
    return n;
}

static int on_progress(void* userp, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d)
{
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    exchange_t* x = userp;
    return atomic_load(&x->stream->cancel) ? 1 : 0;
}

typedef enum { SEND_OK, SEND_TOOLS_REFUSED, SEND_FAILED, SEND_CANCELLED } send_result_t;

static bool mentions_tools(const char* s)
{
    return s && (strstr(s, "tool") || strstr(s, "Tool") || strstr(s, "function")
                    || strstr(s, "capability_not_supported"));
}

static send_result_t send_request(const config_t* cfg, const char* body, exchange_t* x,
    bool sent_tools, char** error_out)
{
    CURL* curl = curl_easy_init();
    if (!curl) {
        *error_out = dup_str("Error: Couldn't start a connection.");
        return SEND_FAILED;
    }
    char* url = dup_printf("%s/chat/completions", cfg->base_url);
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: text/event-stream");
    char* auth = cfg->api_key ? dup_printf("Authorization: Bearer %s", cfg->api_key) : NULL;
    if (auth)
        headers = curl_slist_append(headers, auth);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, x);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, x);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "dawn");

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (x->line.len > 0)
        handle_sse_line(x, x->line.data);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(auth);
    free(url);

    if (atomic_load(&x->stream->cancel) || rc == CURLE_ABORTED_BY_CALLBACK)
        return SEND_CANCELLED;
    if (rc != CURLE_OK) {
        if (cfg->tai)
            *error_out = dup_str("Error: Can't reach TAI. Turn on AI in Termux Launcher's settings.");
        else
            *error_out = dup_printf("Error: Can't reach %s (%s).", cfg->base_url, curl_easy_strerror(rc));
        return SEND_FAILED;
    }

    cJSON* root = x->saw_event ? NULL : cJSON_Parse(x->raw.data ? x->raw.data : "");
    char* server_message = x->stream_error ? dup_str(x->stream_error) : (root ? error_message_from(root) : NULL);

    if (status == 401 || status == 403) {
        if (cfg->tai)
            *error_out = dup_str("Error: TAI refused the connection. Check the token in Termux Launcher's AI settings.");
        else
            *error_out = dup_str("Error: The server refused the API key in your dawn AI settings.");
        free(server_message);
        cJSON_Delete(root);
        return SEND_FAILED;
    }
    // TAI serves one generation at a time; a second request while one runs gets this. The caller
    // (dawn_ai_queue.c) matches this exact sentinel to retry (chat) or drop the turn (title)
    // instead of showing it as an ordinary error.
    if (status == 409) {
        free(server_message);
        cJSON_Delete(root);
        *error_out = dup_str("Error: generation_active");
        return SEND_FAILED;
    }
    if (status >= 400 || x->stream_error) {
        bool refused = sent_tools && status == 400
            && (mentions_tools(server_message) || mentions_tools(x->raw.data));
        if (!refused)
            *error_out = dup_printf("Error: %s",
                server_message ? server_message : "The AI server couldn't answer.");
        free(server_message);
        cJSON_Delete(root);
        return refused ? SEND_TOOLS_REFUSED : SEND_FAILED;
    }
    free(server_message);

    // A server that ignored "stream": true answers with one ordinary completion.
    if (!x->saw_event && root) {
        cJSON* choice = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "choices"), 0);
        cJSON* message = cJSON_GetObjectItemCaseSensitive(choice, "message");
        if (cJSON_IsObject(message)) {
            absorb_delta(x, message, false);
            if (x->content.len > 0)
                emit(x->stream, x->content.data);
        }
        cJSON* usage = cJSON_GetObjectItemCaseSensitive(root, "usage");
        if (cJSON_IsObject(usage)) {
            cJSON* pt = cJSON_GetObjectItemCaseSensitive(usage, "prompt_tokens");
            cJSON* ct = cJSON_GetObjectItemCaseSensitive(usage, "completion_tokens");
            if (cJSON_IsNumber(pt))
                x->usage_prompt_tokens = pt->valueint;
            if (cJSON_IsNumber(ct))
                x->usage_completion_tokens = ct->valueint;
        }
    }
    cJSON_Delete(root);
    return SEND_OK;
}

// #endregion

// #region Conversation

static const char* NO_TOOLS_NOTICE
    = "This model can't use tools, so it can't search the web. It still reads your note and can "
      "edit it.\n\n";

static const tool_t* find_tool(session_t* s, const char* name)
{
    for (int32_t i = 0; i < s->registered_count; i++)
        if (strcmp(s->registered[i].name, name) == 0)
            return &s->registered[i];
    return NULL;
}

//! The note to attach to the question, read through dawn's own read_document tool so the
//! document is only touched on dawn's thread. Its "context" action gives the title, the note and
//! the selection ready to send; a reader without it gives the selection or the note.
static char* document_context(stream_t* st, session_t* s)
{
    const tool_t* reader = find_tool(s, "read_document");
    if (!reader)
        return NULL;
    char* prepared = call_tool(st, reader, "{\"action\":\"context\"}");
    cJSON* prepared_root = prepared ? cJSON_Parse(prepared) : NULL;
    free(prepared);
    cJSON* prepared_text = cJSON_GetObjectItemCaseSensitive(prepared_root, "text");
    char* ready = cJSON_IsString(prepared_text) && prepared_text->valuestring[0]
        ? dup_str(prepared_text->valuestring) : NULL;
    cJSON_Delete(prepared_root);
    if (ready)
        return ready;
    const char* actions[] = { "{\"action\":\"selection\"}", "{\"action\":\"full\"}" };
    for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); i++) {
        char* result = call_tool(st, reader, actions[i]);
        cJSON* root = result ? cJSON_Parse(result) : NULL;
        free(result);
        cJSON* text = cJSON_GetObjectItemCaseSensitive(root, "text");
        char* out = NULL;
        if (cJSON_IsString(text) && text->valuestring[0]) {
            size_t n = strlen(text->valuestring);
            if (n > DOC_CONTEXT_LIMIT)
                text->valuestring[DOC_CONTEXT_LIMIT] = '\0';
            out = dup_printf("%s%s", i == 0 ? "Selected text:\n" : "The note:\n", text->valuestring);
        }
        cJSON_Delete(root);
        if (out)
            return out;
    }
    return NULL;
}

//! content with the body of every edit block dawn made (<replace_note>…</replace_note> and the
//! like) cut out. The next question brings the note as it now is, so a copy of a rewrite in the
//! history would only spend a small context window twice.
static char* compact_edit_blocks(const char* content)
{
    static const char* const tags[] = { "replace_note", "replace_selection", "insert_at_cursor", "append_to_note" };
    char* out = dup_str(content);
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
            const char* stub = "(made in the note)";
            size_t stub_len = strlen(stub);
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

static cJSON* message_new(const char* role, const char* content)
{
    cJSON* m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", role);
    if (content)
        cJSON_AddStringToObject(m, "content", content);
    else
        cJSON_AddNullToObject(m, "content");
    return m;
}

//! Never send a request whose estimated prompt plus max_tokens exceeds the context window: dawn
//! trims the note context itself before it ever reaches here (dawn_ai_tokens.c), so what is left
//! to trim on this side is the session's own history, oldest first. Called with g_lock held.
static void trim_history_to_budget(session_t* s, const stream_t* st, const char* context)
{
    int32_t window = ai_bridge_context_window();
    int32_t reserve = st->max_tokens > 0 ? st->max_tokens : 768;
    int32_t budget = window - reserve - 256; // headroom for the system message and formatting
    if (budget < 256)
        budget = 256;

    int32_t fixed = estimate_tokens_rough(s->instructions) + estimate_tokens_rough(context)
        + estimate_tokens_rough(st->prompt);
    while (cJSON_GetArraySize(s->history) > 0) {
        int32_t total = fixed;
        cJSON* m;
        cJSON_ArrayForEach(m, s->history)
        {
            cJSON* content = cJSON_GetObjectItemCaseSensitive(m, "content");
            if (cJSON_IsString(content))
                total += estimate_tokens_rough(content->valuestring);
        }
        if (total <= budget)
            break;
        // History is stored as a flat run of user/assistant messages, oldest first: drop the
        // oldest pair together so a lone assistant reply is never left without its question.
        cJSON_DeleteItemFromArray(s->history, 0);
        if (cJSON_GetArraySize(s->history) > 0)
            cJSON_DeleteItemFromArray(s->history, 0);
    }
}

static char* build_body(const config_t* cfg, session_t* s, const stream_t* st, cJSON* turn,
    const char* context, bool with_tools)
{
    cJSON* body = cJSON_CreateObject();
    if (cfg->model)
        cJSON_AddStringToObject(body, "model", cfg->model);
    cJSON_AddBoolToObject(body, "stream", true);
    // Asks for a final usage-only chunk (choices: []) so prompt/completion token counts can
    // calibrate the caller's own char-based estimate. Servers that don't understand the option
    // just ignore it.
    cJSON* stream_options = cJSON_AddObjectToObject(body, "stream_options");
    cJSON_AddBoolToObject(stream_options, "include_usage", true);
    if (st->temperature > 0)
        cJSON_AddNumberToObject(body, "temperature", st->temperature);
    if (st->max_tokens > 0)
        cJSON_AddNumberToObject(body, "max_tokens", st->max_tokens);

    cJSON* messages = cJSON_AddArrayToObject(body, "messages");
    const char* instructions = s->instructions ? s->instructions : "";
    char* system = with_tools
        ? dup_str(instructions)
        : dup_printf("%s\n\nNo tools can be called in this chat. Answer from the note that comes with "
                     "the user's message, and make any change to it with the tagged blocks described above.",
              instructions);
    if (system && system[0])
        cJSON_AddItemToArray(messages, message_new("system", system));
    free(system);

    cJSON* m;
    cJSON_ArrayForEach(m, s->history) cJSON_AddItemToArray(messages, cJSON_Duplicate(m, true));
    int32_t turn_count = cJSON_GetArraySize(turn);
    for (int32_t i = 0; i < turn_count; i++) {
        cJSON* copy = cJSON_Duplicate(cJSON_GetArrayItem(turn, i), true);
        // The note rides on this question only, so history never carries a copy of it per turn.
        if (i == 0 && context) {
            cJSON* content = cJSON_GetObjectItemCaseSensitive(copy, "content");
            char* joined = dup_printf("%s\n\n---\n%s", cJSON_IsString(content) ? content->valuestring : "", context);
            cJSON_ReplaceItemInObject(copy, "content", cJSON_CreateString(joined ? joined : ""));
            free(joined);
        }
        cJSON_AddItemToArray(messages, copy);
    }

    if (with_tools && s->tools)
        cJSON_AddItemToObject(body, "tools", cJSON_Duplicate(s->tools, true));

    char* out = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    return out;
}

//! Every tool reports failure as an object with an "error" key; no result at all is one too.
static bool tool_failed(const char* output)
{
    if (!output)
        return true;
    cJSON* root = cJSON_Parse(output);
    bool failed = cJSON_IsObject(root) && cJSON_GetObjectItemCaseSensitive(root, "error") != NULL;
    cJSON_Delete(root);
    return failed;
}

static cJSON* assistant_tool_message(exchange_t* x)
{
    cJSON* m = message_new("assistant", x->content.len ? x->content.data : NULL);
    cJSON* calls = cJSON_AddArrayToObject(m, "tool_calls");
    for (int32_t i = 0; i < x->call_count; i++) {
        tool_call_t* tc = &x->calls[i];
        if (!tc->name.data)
            continue;
        if (!tc->id)
            tc->id = dup_printf("call_%d", (int)i);
        cJSON* call = cJSON_CreateObject();
        cJSON_AddStringToObject(call, "id", tc->id);
        cJSON_AddStringToObject(call, "type", "function");
        cJSON* fn = cJSON_AddObjectToObject(call, "function");
        cJSON_AddStringToObject(fn, "name", tc->name.data);
        cJSON_AddStringToObject(fn, "arguments", tc->arguments.data ? tc->arguments.data : "{}");
        cJSON_AddItemToArray(calls, call);
    }
    return m;
}

//! Answers one question, including any tool rounds, then records the question and the final
//! answer in the session history. Tool traffic is dropped from the history afterwards: a small
//! on-device context window would otherwise fill with old document reads.
static void run_turn(stream_t* st)
{
    config_t cfg = config_load();
    if (cfg.error) {
        emit(st, cfg.error);
        config_free(&cfg);
        return;
    }

    pthread_mutex_lock(&g_lock);
    session_t* s = &g_sessions[st->session];
    bool use_tools = s->tools && !s->tools_refused;
    pthread_mutex_unlock(&g_lock);

    cJSON* turn = cJSON_CreateArray();
    cJSON_AddItemToArray(turn, message_new("user", st->prompt));
    // The note rides on every question, tools or not. TAI takes the tools away from most
    // on-device models without an error, and a model that was never shown the note cannot call
    // read_document to find it: it would ask what "this" is.
    char* context = document_context(st, s);

    for (int32_t round = 0; round <= MAX_TOOL_ROUNDS && !atomic_load(&st->cancel); round++) {
        pthread_mutex_lock(&g_lock);
        trim_history_to_budget(s, st, context);
        char* body = build_body(&cfg, s, st, turn, context, use_tools);
        pthread_mutex_unlock(&g_lock);

        exchange_t x = { .stream = st, .usage_prompt_tokens = -1, .usage_completion_tokens = -1 };
        char* error = NULL;
        progress(st, AI_BRIDGE_PROGRESS_WAITING, NULL, 0, 0);
        send_result_t result = send_request(&cfg, body ? body : "{}", &x, use_tools, &error);
        free(body);

        if (result == SEND_TOOLS_REFUSED) {
            pthread_mutex_lock(&g_lock);
            s->tools_refused = true;
            bool tell = !s->told_tools_refused;
            s->told_tools_refused = true;
            pthread_mutex_unlock(&g_lock);
            if (tell)
                emit(st, NO_TOOLS_NOTICE);
            use_tools = false;
            free(context);
            context = document_context(st, s);
            // Keep only the question: tool rounds already taken cannot be sent without tools.
            while (cJSON_GetArraySize(turn) > 1)
                cJSON_DeleteItemFromArray(turn, 1);
            exchange_free(&x);
            round = -1;
            continue;
        }
        if (result != SEND_OK) {
            if (result == SEND_FAILED)
                emit(st, error);
            free(error);
            exchange_free(&x);
            break;
        }

        if (x.call_count == 0 || round == MAX_TOOL_ROUNDS) {
            if (x.call_count > 0 && x.content.len == 0) {
                const char* stopped = "I stopped after too many steps. Try asking something more specific.";
                emit(st, stopped);
                sb_append(&x.content, stopped, strlen(stopped));
            }
            pthread_mutex_lock(&g_lock);
            cJSON_AddItemToArray(s->history, cJSON_Duplicate(cJSON_GetArrayItem(turn, 0), true));
            char* kept = compact_edit_blocks(x.content.data ? x.content.data : "");
            cJSON_AddItemToArray(s->history, message_new("assistant", kept ? kept : ""));
            free(kept);
            pthread_mutex_unlock(&g_lock);
            store_usage(x.usage_prompt_tokens, x.usage_completion_tokens);
            exchange_free(&x);
            break;
        }

        cJSON_AddItemToArray(turn, assistant_tool_message(&x));
        if (x.content.len > 0)
            emit(st, "\n\n");
        int32_t steps = 0;
        for (int32_t i = 0; i < x.call_count; i++)
            steps += x.calls[i].name.data != NULL;
        int32_t step = 0;
        for (int32_t i = 0; i < x.call_count && !atomic_load(&st->cancel); i++) {
            tool_call_t* tc = &x.calls[i];
            if (!tc->name.data)
                continue;
            pthread_mutex_lock(&g_lock);
            const tool_t* tool = find_tool(s, tc->name.data);
            pthread_mutex_unlock(&g_lock);
            progress(st, AI_BRIDGE_PROGRESS_TOOL_START, tc->name.data, ++step, steps);
            char* output = tool ? call_tool(st, tool, tc->arguments.data ? tc->arguments.data : "{}")
                                : dup_printf("{\"error\":\"No tool named %s\"}", tc->name.data);
            if (!atomic_load(&st->cancel))
                progress(st, tool_failed(output) ? AI_BRIDGE_PROGRESS_TOOL_FAILED : AI_BRIDGE_PROGRESS_TOOL_DONE,
                    tc->name.data, step, steps);
            cJSON* reply = message_new("tool", output ? output : "{\"error\":\"The tool failed\"}");
            cJSON_AddStringToObject(reply, "tool_call_id", tc->id ? tc->id : "");
            cJSON_AddItemToArray(turn, reply);
            free(output);
        }
        exchange_free(&x);
    }

    free(context);
    cJSON_Delete(turn);
    config_free(&cfg);
}

static void* stream_worker(void* arg)
{
    stream_t* st = arg;
    run_turn(st);
    emit(st, NULL);

    pthread_mutex_lock(&g_lock);
    uint8_t session = st->session;
    free(st->prompt);
    uint8_t id = (uint8_t)(st - g_streams);
    memset(&g_streams[id], 0, sizeof(stream_t));
    session_release_locked(session);
    pthread_mutex_unlock(&g_lock);
    return NULL;
}

// #endregion

// #region ai_bridge.h

bool ai_bridge_init(void)
{
    pthread_mutex_lock(&g_lock);
    if (!g_curl_ready)
        g_curl_ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    bool ready = g_curl_ready;
    pthread_mutex_unlock(&g_lock);
    return ready;
}

// Whether a model is ready is only known by asking it, so the panel is always offered and a
// missing or unloaded model is reported as the answer to the first question.
ai_availability_status_t ai_bridge_check_availability(void) { return AI_BRIDGE_AVAILABLE; }

char* ai_bridge_get_availability_reason(void) { return dup_str("Ready"); }

int32_t ai_bridge_get_supported_languages_count(void) { return 0; }

char* ai_bridge_get_supported_language(int32_t index)
{
    (void)index;
    return NULL;
}

ai_bridge_session_id_t ai_bridge_create_session(const char* instructions, const char* tools_json,
    bool enable_guardrails, bool prewarm)
{
    (void)enable_guardrails;
    (void)prewarm;
    pthread_mutex_lock(&g_lock);
    ai_bridge_session_id_t id = AI_BRIDGE_INVALID_ID;
    for (int32_t i = 1; i <= MAX_SESSIONS; i++) {
        if (!g_sessions[i].used) {
            id = (ai_bridge_session_id_t)i;
            break;
        }
    }
    if (id != AI_BRIDGE_INVALID_ID) {
        session_t* s = &g_sessions[id];
        memset(s, 0, sizeof(*s));
        s->used = true;
        s->refs = 1;
        s->instructions = dup_str(instructions);
        s->tools = openai_tools_from(tools_json);
        s->history = cJSON_CreateArray();
    }
    pthread_mutex_unlock(&g_lock);
    return id;
}

bool ai_bridge_register_tool(ai_bridge_session_id_t session_id, const char* tool_name,
    ai_bridge_tool_callback_t callback, void* user_data)
{
    bool ok = false;
    pthread_mutex_lock(&g_lock);
    session_t* s = &g_sessions[session_id];
    if (session_id != AI_BRIDGE_INVALID_ID && s->used && !s->closing && tool_name && callback
        && s->registered_count < (int32_t)(sizeof(s->registered) / sizeof(s->registered[0]))) {
        tool_t* t = &s->registered[s->registered_count++];
        t->name = dup_str(tool_name);
        t->callback = callback;
        t->user_data = user_data;
        ok = true;
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

void ai_bridge_destroy_session(ai_bridge_session_id_t session_id)
{
    if (session_id == AI_BRIDGE_INVALID_ID)
        return;
    pthread_mutex_lock(&g_lock);
    session_t* s = &g_sessions[session_id];
    if (s->used && !s->closing) {
        for (int32_t i = 1; i <= MAX_STREAMS; i++)
            if (g_streams[i].used && g_streams[i].session == session_id)
                atomic_store(&g_streams[i].cancel, true);
        s->closing = true;
        session_release_locked(session_id);
        pthread_cond_broadcast(&g_cond);
    }
    pthread_mutex_unlock(&g_lock);
}

static stream_t* stream_claim_locked(ai_bridge_session_id_t session_id, ai_bridge_stream_id_t* id_out)
{
    session_t* s = &g_sessions[session_id];
    if (session_id == AI_BRIDGE_INVALID_ID || !s->used || s->closing)
        return NULL;
    for (int32_t i = 1; i <= MAX_STREAMS; i++) {
        if (!g_streams[i].used) {
            stream_t* st = &g_streams[i];
            memset(st, 0, sizeof(*st));
            st->used = true;
            st->session = session_id;
            s->refs++;
            *id_out = (ai_bridge_stream_id_t)i;
            return st;
        }
    }
    return NULL;
}

char* ai_bridge_generate_response(ai_bridge_session_id_t session_id, const char* prompt,
    double temperature, int32_t max_tokens)
{
    ai_bridge_stream_id_t id = AI_BRIDGE_INVALID_ID;
    pthread_mutex_lock(&g_lock);
    stream_t* st = stream_claim_locked(session_id, &id);
    pthread_mutex_unlock(&g_lock);
    if (!st)
        return dup_str("Error: Session not found");

    st->prompt = dup_str(prompt ? prompt : "");
    st->temperature = temperature;
    st->max_tokens = max_tokens;
    st->inline_calls = true;
    run_turn(st);
    char* reply = st->collected.data ? st->collected.data : dup_str("");

    pthread_mutex_lock(&g_lock);
    free(st->prompt);
    memset(st, 0, sizeof(*st));
    session_release_locked(session_id);
    pthread_mutex_unlock(&g_lock);
    return reply;
}

char* ai_bridge_generate_structured_response(ai_bridge_session_id_t session_id, const char* prompt,
    const char* schema_json, double temperature, int32_t max_tokens)
{
    (void)session_id;
    (void)prompt;
    (void)schema_json;
    (void)temperature;
    (void)max_tokens;
    return dup_str("Error: Structured responses are not supported by this bridge");
}

ai_bridge_stream_id_t ai_bridge_generate_response_stream(ai_bridge_session_id_t session_id,
    const char* prompt, double temperature, int32_t max_tokens, void* context,
    ai_bridge_stream_callback_t callback, void* user_data)
{
    if (!callback)
        return AI_BRIDGE_INVALID_ID;
    ai_bridge_stream_id_t id = AI_BRIDGE_INVALID_ID;
    pthread_mutex_lock(&g_lock);
    stream_t* st = stream_claim_locked(session_id, &id);
    if (st) {
        st->prompt = dup_str(prompt ? prompt : "");
        st->temperature = temperature;
        st->max_tokens = max_tokens;
        st->context = context;
        st->callback = callback;
        st->user_data = user_data;
    }
    pthread_mutex_unlock(&g_lock);
    if (!st)
        return AI_BRIDGE_INVALID_ID;

    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    bool started = pthread_create(&thread, &attr, stream_worker, st) == 0;
    pthread_attr_destroy(&attr);
    if (!started) {
        pthread_mutex_lock(&g_lock);
        free(st->prompt);
        memset(st, 0, sizeof(*st));
        session_release_locked(session_id);
        pthread_mutex_unlock(&g_lock);
        return AI_BRIDGE_INVALID_ID;
    }
    return id;
}

ai_bridge_stream_id_t ai_bridge_generate_structured_response_stream(
    ai_bridge_session_id_t session_id, const char* prompt, const char* schema_json,
    double temperature, int32_t max_tokens, void* context, ai_bridge_stream_callback_t callback,
    void* user_data)
{
    (void)session_id;
    (void)prompt;
    (void)schema_json;
    (void)temperature;
    (void)max_tokens;
    (void)context;
    (void)callback;
    (void)user_data;
    return AI_BRIDGE_INVALID_ID;
}

bool ai_bridge_set_progress_callback(ai_bridge_session_id_t session_id,
    ai_bridge_progress_callback_t callback, void* user_data)
{
    bool ok = false;
    pthread_mutex_lock(&g_lock);
    session_t* s = &g_sessions[session_id];
    if (session_id != AI_BRIDGE_INVALID_ID && s->used && !s->closing) {
        s->progress = callback;
        s->progress_data = user_data;
        ok = true;
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

bool ai_bridge_cancel_stream(ai_bridge_stream_id_t stream_id)
{
    bool found = false;
    pthread_mutex_lock(&g_lock);
    if (stream_id != AI_BRIDGE_INVALID_ID && g_streams[stream_id].used) {
        atomic_store(&g_streams[stream_id].cancel, true);
        pthread_cond_broadcast(&g_cond);
        found = true;
    }
    pthread_mutex_unlock(&g_lock);
    if (found)
        post_runtime_cancel_async();
    return found;
}

char* ai_bridge_get_session_history(ai_bridge_session_id_t session_id)
{
    char* out = NULL;
    pthread_mutex_lock(&g_lock);
    session_t* s = &g_sessions[session_id];
    if (session_id != AI_BRIDGE_INVALID_ID && s->used && !s->closing)
        out = cJSON_PrintUnformatted(s->history);
    pthread_mutex_unlock(&g_lock);
    return out;
}

bool ai_bridge_clear_session_history(ai_bridge_session_id_t session_id)
{
    bool ok = false;
    pthread_mutex_lock(&g_lock);
    session_t* s = &g_sessions[session_id];
    if (session_id != AI_BRIDGE_INVALID_ID && s->used && !s->closing) {
        cJSON_Delete(s->history);
        s->history = cJSON_CreateArray();
        ok = true;
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

bool ai_bridge_add_message_to_history(ai_bridge_session_id_t session_id, const char* role,
    const char* content)
{
    bool ok = false;
    pthread_mutex_lock(&g_lock);
    session_t* s = &g_sessions[session_id];
    if (session_id != AI_BRIDGE_INVALID_ID && s->used && !s->closing && role && content) {
        cJSON_AddItemToArray(s->history, message_new(role, content));
        ok = true;
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

void ai_bridge_free_string(char* ptr) { free(ptr); }

// #endregion
