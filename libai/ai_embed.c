/**
 * @file ai_embed.c
 * @brief ai_embed.h over TAI's /v1/embeddings, /v1/tokenize, /v1/models and /v1/ai/runtime.
 *
 * Plain blocking libcurl calls, one easy handle per call, like ai_bridge_openai.c's http_get():
 * the endpoint and key are read again for every call (ai_openai_endpoint()), so turning TAI on or
 * editing ai.json takes effect on the next one. Replies are capped at RESPONSE_MAX bytes and every
 * field read from them is type- and range-checked.
 */

#include "ai_embed.h"

#include "cJSON.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define RESPONSE_MAX (16u << 20) //!< 64 inputs x 768 floats as JSON text is ~1 MB; plenty of room
#define MAX_DIMS 4096
#define PREFERRED_DIMS 256
#define DEFAULT_BATCH 16

// #region HTTP

typedef struct {
    char* data;
    size_t len;
    size_t cap;
    bool too_big;
} resp_t;

static void resp_free(resp_t* r)
{
    free(r->data);
    memset(r, 0, sizeof(*r));
}

static size_t on_body(char* data, size_t size, size_t count, void* userp)
{
    resp_t* r = userp;
    size_t n = size * count;
    if (n > RESPONSE_MAX - r->len) {
        r->too_big = true;
        return 0; // aborts the transfer
    }
    if (r->len + n + 1 > r->cap) {
        size_t cap = r->cap ? r->cap : 4096;
        while (cap < r->len + n + 1)
            cap *= 2;
        char* grown = realloc(r->data, cap);
        if (!grown)
            return 0;
        r->data = grown;
        r->cap = cap;
    }
    memcpy(r->data + r->len, data, n);
    r->len += n;
    r->data[r->len] = '\0';
    return n;
}

//! Picks Retry-After (in whole seconds, the only form TAI sends) out of the response headers.
static size_t on_header(char* buf, size_t size, size_t count, void* userp)
{
    int32_t* retry_after_s = userp;
    size_t n = size * count;
    static const char name[] = "Retry-After:";
    const size_t name_len = sizeof(name) - 1;
    if (n > name_len && strncasecmp(buf, name, name_len) == 0) {
        char digits[16];
        size_t k = 0;
        for (size_t i = name_len; i < n && k < sizeof(digits) - 1; i++) {
            if (buf[i] >= '0' && buf[i] <= '9')
                digits[k++] = buf[i];
            else if (k > 0)
                break;
        }
        digits[k] = '\0';
        if (k > 0)
            *retry_after_s = atoi(digits);
    }
    return n;
}

static int on_progress(void* userp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    const atomic_bool* cancel = userp;
    return cancel && atomic_load(cancel) ? 1 : 0;
}

//! One call to the endpoint: GET when body is NULL, else POST of JSON. Maps the outcome onto
//! ai_embed_status_t; *resp gets the body whatever the status (caller frees).
static ai_embed_status_t http_call(const char* path, const char* body, long timeout_s,
    const atomic_bool* cancel, resp_t* resp, int32_t* retry_after_ms)
{
    memset(resp, 0, sizeof(*resp));
    if (retry_after_ms)
        *retry_after_ms = 0;
    if (cancel && atomic_load(cancel))
        return AI_EMBED_CANCELLED;

    char* base = NULL;
    char* key = NULL;
    if (!ai_openai_endpoint(&base, &key))
        return AI_EMBED_NONE;

    CURL* curl = curl_easy_init();
    if (!curl) {
        free(base);
        free(key);
        return AI_EMBED_ERROR;
    }

    size_t url_len = strlen(base) + strlen(path) + 1;
    char* url = malloc(url_len);
    char* auth = NULL;
    if (key) {
        size_t auth_len = strlen(key) + 32;
        auth = malloc(auth_len);
        if (auth)
            snprintf(auth, auth_len, "Authorization: Bearer %s", key);
    }
    if (!url || (key && !auth)) {
        free(url);
        free(auth);
        free(base);
        free(key);
        curl_easy_cleanup(curl);
        return AI_EMBED_ERROR;
    }
    snprintf(url, url_len, "%s%s", base, path);

    struct curl_slist* headers = NULL;
    if (body)
        headers = curl_slist_append(headers, "Content-Type: application/json");
    if (auth)
        headers = curl_slist_append(headers, auth);

    int32_t retry_after_s = 0;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    if (headers)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (body)
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, resp);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &retry_after_s);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, (void*)cancel);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "dawn");

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(auth);
    free(url);
    free(base);
    free(key);

    if (retry_after_s < 1)
        retry_after_s = 0;
    if (retry_after_s > 600)
        retry_after_s = 600;

    if (cancel && atomic_load(cancel))
        return AI_EMBED_CANCELLED;
    if (rc != CURLE_OK) {
        if (resp->too_big)
            return AI_EMBED_ERROR;
        if (retry_after_ms)
            *retry_after_ms = 30 * 1000;
        return AI_EMBED_RETRY;
    }
    if (status >= 200 && status < 300)
        return AI_EMBED_OK;
    if (status == 404 || status == 501)
        return AI_EMBED_NONE;
    if (status == 429 || status == 503) {
        if (retry_after_ms)
            *retry_after_ms = (retry_after_s ? retry_after_s : (status == 429 ? 10 : 20)) * 1000;
        return AI_EMBED_RETRY;
    }
    if (status == 400 || status == 413)
        return AI_EMBED_REJECTED;
    return AI_EMBED_ERROR;
}

//! The stable error code of an error reply (`error.code`, or a top-level `code`), or "".
static const char* error_code(cJSON* root)
{
    cJSON* err = cJSON_GetObjectItemCaseSensitive(root, "error");
    cJSON* code = cJSON_IsObject(err) ? cJSON_GetObjectItemCaseSensitive(err, "code") : NULL;
    if (!cJSON_IsString(code))
        code = cJSON_GetObjectItemCaseSensitive(root, "code");
    return cJSON_IsString(code) && code->valuestring ? code->valuestring : "";
}

//! A NUL-terminated copy of n bytes, for cJSON, which only takes C strings.
static char* dup_bytes(const char* s, size_t n)
{
    char* out = malloc(n + 1);
    if (!out)
        return NULL;
    if (n > 0)
        memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

static bool contains_ci(const char* haystack, const char* needle)
{
    size_t hn = strlen(haystack), nn = strlen(needle);
    for (size_t i = 0; i + nn <= hn; i++)
        if (strncasecmp(haystack + i, needle, nn) == 0)
            return true;
    return false;
}

// #endregion

// #region Discovery and runtime

static bool has_embedding_capability(cJSON* entry)
{
    cJSON* caps = cJSON_GetObjectItemCaseSensitive(entry, "_capabilities");
    cJSON* cap;
    cJSON_ArrayForEach(cap, caps)
    {
        if (cJSON_IsString(cap) && cap->valuestring && strcmp(cap->valuestring, "text_embeddings") == 0)
            return true;
    }
    return false;
}

//! 256 when the model lists it among its Matryoshka sizes, else the listed size nearest to it;
//! 0 (leave `dimensions` out) when it lists none.
static int32_t pick_dims(cJSON* entry)
{
    cJSON* list = cJSON_GetObjectItemCaseSensitive(entry, "_endpoint_matryoshka_dims");
    int32_t best = 0;
    cJSON* d;
    cJSON_ArrayForEach(d, list)
    {
        if (!cJSON_IsNumber(d) || d->valuedouble < 1 || d->valuedouble > MAX_DIMS)
            continue;
        int32_t v = (int32_t)d->valuedouble;
        if (best == 0 || abs(v - PREFERRED_DIMS) < abs(best - PREFERRED_DIMS))
            best = v;
    }
    return best;
}

ai_embed_status_t ai_embed_find_embedder(ai_embedder_t* out, const atomic_bool* cancel)
{
    memset(out, 0, sizeof(*out));
    resp_t resp;
    ai_embed_status_t st = http_call("/models", NULL, 8L, cancel, &resp, NULL);
    if (st != AI_EMBED_OK) {
        resp_free(&resp);
        return st;
    }
    cJSON* root = resp.data ? cJSON_Parse(resp.data) : NULL;
    resp_free(&resp);
    if (!root)
        return AI_EMBED_ERROR;

    // EmbeddingGemma 2 beats EmbeddingGemma beats any other embedder; the first of a rank wins.
    cJSON* list = cJSON_IsArray(root) ? root : cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON* chosen = NULL;
    int32_t chosen_rank = -1;
    cJSON* entry;
    cJSON_ArrayForEach(entry, list)
    {
        cJSON* id = cJSON_GetObjectItemCaseSensitive(entry, "id");
        if (!cJSON_IsString(id) || !id->valuestring || !id->valuestring[0]
            || strlen(id->valuestring) >= sizeof(out->id) || !has_embedding_capability(entry))
            continue;
        int32_t rank = contains_ci(id->valuestring, "embeddinggemma-2") ? 2
            : contains_ci(id->valuestring, "embeddinggemma")            ? 1
                                                                        : 0;
        if (rank > chosen_rank) {
            chosen = entry;
            chosen_rank = rank;
        }
        if (rank == 2)
            break;
    }
    if (!chosen) {
        cJSON_Delete(root);
        return AI_EMBED_NONE;
    }

    snprintf(out->id, sizeof(out->id), "%s", cJSON_GetObjectItemCaseSensitive(chosen, "id")->valuestring);
    cJSON* rev = cJSON_GetObjectItemCaseSensitive(chosen, "_revision");
    if (cJSON_IsString(rev) && rev->valuestring && strlen(rev->valuestring) < sizeof(out->revision))
        snprintf(out->revision, sizeof(out->revision), "%s", rev->valuestring);
    out->dims = pick_dims(chosen);
    cJSON* native = cJSON_GetObjectItemCaseSensitive(chosen, "_endpoint_dimensions");
    if (cJSON_IsNumber(native) && native->valuedouble >= 1 && native->valuedouble <= MAX_DIMS)
        out->native_dims = (int32_t)native->valuedouble;
    cJSON* batch = cJSON_GetObjectItemCaseSensitive(chosen, "_endpoint_max_batch");
    out->max_batch = DEFAULT_BATCH;
    if (cJSON_IsNumber(batch) && batch->valuedouble >= 1)
        out->max_batch = batch->valuedouble > AI_EMBED_BATCH_MAX ? AI_EMBED_BATCH_MAX : (int32_t)batch->valuedouble;
    cJSON* window = cJSON_GetObjectItemCaseSensitive(chosen, "_endpoint_context_window");
    if (cJSON_IsNumber(window) && window->valuedouble >= 1 && window->valuedouble < 1e7)
        out->context_window = (int32_t)window->valuedouble;
    cJSON_Delete(root);
    return AI_EMBED_OK;
}

ai_embed_status_t ai_embed_generation_active(bool* active, const atomic_bool* cancel)
{
    *active = false;
    resp_t resp;
    ai_embed_status_t st = http_call("/ai/runtime", NULL, 5L, cancel, &resp, NULL);
    if (st == AI_EMBED_OK) {
        cJSON* root = resp.data ? cJSON_Parse(resp.data) : NULL;
        cJSON* runtime = cJSON_GetObjectItemCaseSensitive(root, "runtime");
        cJSON* flag = cJSON_GetObjectItemCaseSensitive(cJSON_IsObject(runtime) ? runtime : root, "activeGeneration");
        *active = cJSON_IsTrue(flag);
        if (!root)
            st = AI_EMBED_ERROR;
        cJSON_Delete(root);
    }
    resp_free(&resp);
    return st;
}

ai_embed_status_t ai_embed_tokenize(const char* model, const char* text, size_t len, int32_t* tokens,
    const atomic_bool* cancel)
{
    *tokens = 0;
    char* input = dup_bytes(text, len);
    cJSON* req = cJSON_CreateObject();
    if (!input || !req) {
        free(input);
        cJSON_Delete(req);
        return AI_EMBED_ERROR;
    }
    cJSON_AddStringToObject(req, "model", model);
    cJSON_AddStringToObject(req, "input", input);
    free(input);
    char* body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body)
        return AI_EMBED_ERROR;

    resp_t resp;
    ai_embed_status_t st = http_call("/tokenize", body, 15L, cancel, &resp, NULL);
    free(body);
    if (st == AI_EMBED_OK) {
        cJSON* root = resp.data ? cJSON_Parse(resp.data) : NULL;
        cJSON* n = cJSON_GetObjectItemCaseSensitive(root, "tokens");
        if (cJSON_IsNumber(n) && n->valuedouble >= 0 && n->valuedouble < 1e9)
            *tokens = (int32_t)n->valuedouble;
        else
            st = AI_EMBED_ERROR;
        cJSON_Delete(root);
    }
    resp_free(&resp);
    return st;
}

// #endregion

// #region Embeddings

//! Decode standard base64 of little-endian float32 values. Returns a malloc'd array and its
//! length in *n, or NULL when the text is not whole float32 values in valid base64.
static float* decode_base64_floats(const char* s, int32_t* n)
{
    *n = 0;
    size_t len = strlen(s);
    while (len > 0 && s[len - 1] == '=')
        len--;
    size_t bytes = len / 4 * 3 + (len % 4 == 3 ? 2 : len % 4 == 2 ? 1 : 0);
    if (len % 4 == 1 || bytes == 0 || bytes % 4 != 0 || bytes / 4 > MAX_DIMS)
        return NULL;
    uint8_t* raw = malloc(bytes);
    if (!raw)
        return NULL;
    uint32_t acc = 0;
    int32_t bits = 0;
    size_t w = 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        int32_t v = c >= 'A' && c <= 'Z' ? c - 'A'
            : c >= 'a' && c <= 'z'       ? c - 'a' + 26
            : c >= '0' && c <= '9'       ? c - '0' + 52
            : c == '+'                   ? 62
            : c == '/'                   ? 63
                                         : -1;
        if (v < 0) {
            free(raw);
            return NULL;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (w < bytes)
                raw[w++] = (uint8_t)(acc >> bits);
        }
    }
    int32_t count = (int32_t)(bytes / 4);
    float* out = w == bytes ? malloc(sizeof(float) * (size_t)count) : NULL;
    if (out) {
        for (int32_t i = 0; i < count; i++) {
            const uint8_t* b = raw + (size_t)i * 4;
            uint32_t u = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
            memcpy(&out[i], &u, 4);
        }
        *n = count;
    }
    free(raw);
    return out;
}

//! One `embedding` value, base64 or a float array, as a malloc'd array.
static float* decode_embedding(cJSON* e, int32_t* n)
{
    *n = 0;
    if (cJSON_IsString(e) && e->valuestring)
        return decode_base64_floats(e->valuestring, n);
    if (!cJSON_IsArray(e))
        return NULL;
    int32_t count = cJSON_GetArraySize(e);
    if (count < 1 || count > MAX_DIMS)
        return NULL;
    float* out = malloc(sizeof(float) * (size_t)count);
    if (!out)
        return NULL;
    int32_t i = 0;
    cJSON* v;
    cJSON_ArrayForEach(v, e)
    {
        if (!cJSON_IsNumber(v) || i >= count) {
            free(out);
            return NULL;
        }
        out[i++] = (float)v->valuedouble;
    }
    *n = i;
    return out;
}

static char* build_request(const ai_embed_request_t* req, bool base64)
{
    cJSON* root = cJSON_CreateObject();
    cJSON* input = cJSON_CreateArray();
    if (!root || !input) {
        cJSON_Delete(root);
        cJSON_Delete(input);
        return NULL;
    }
    cJSON_AddStringToObject(root, "model", req->model);
    cJSON_AddItemToObject(root, "input", input);
    for (int32_t i = 0; i < req->count; i++) {
        char* text = dup_bytes(req->inputs[i], req->inputs_len[i]);
        cJSON* item = text ? cJSON_CreateString(text) : NULL;
        free(text);
        if (!item) {
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_AddItemToArray(input, item);
    }
    if (req->dims > 0)
        cJSON_AddNumberToObject(root, "dimensions", req->dims);
    cJSON_AddStringToObject(root, "input_type", req->query ? "query" : "document");
    if (!req->query && req->title && req->title[0])
        cJSON_AddStringToObject(root, "title", req->title);
    if (base64)
        cJSON_AddStringToObject(root, "encoding_format", "base64");
    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return body;
}

//! Fill *out from a 200 reply; false when it does not hold exactly one well-formed vector per
//! input, all of one length.
static bool parse_vectors(const char* body, const ai_embed_request_t* req, ai_embed_result_t* out)
{
    cJSON* root = body ? cJSON_Parse(body) : NULL;
    cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    bool ok = cJSON_IsArray(data) && cJSON_GetArraySize(data) == req->count;
    bool seen[AI_EMBED_BATCH_MAX] = { 0 };
    out->tokens = 0;

    int32_t pos = 0;
    cJSON* item;
    cJSON_ArrayForEach(item, data)
    {
        if (!ok)
            break;
        cJSON* index = cJSON_GetObjectItemCaseSensitive(item, "index");
        int32_t at = cJSON_IsNumber(index) ? (int32_t)index->valuedouble : pos;
        pos++;
        if (at < 0 || at >= req->count || seen[at]) {
            ok = false;
            break;
        }
        seen[at] = true;
        int32_t n;
        float* v = decode_embedding(cJSON_GetObjectItemCaseSensitive(item, "embedding"), &n);
        if (!v || (req->dims > 0 && n != req->dims) || (out->dims > 0 && n != out->dims)) {
            free(v);
            ok = false;
            break;
        }
        if (!out->vectors) {
            out->dims = n;
            out->vectors = malloc(sizeof(float) * (size_t)n * (size_t)req->count);
            if (!out->vectors) {
                free(v);
                ok = false;
                break;
            }
        }
        memcpy(out->vectors + (size_t)at * (size_t)n, v, sizeof(float) * (size_t)n);
        free(v);

        cJSON* tokens = cJSON_GetObjectItemCaseSensitive(item, "tokens");
        if (cJSON_IsNumber(tokens) && tokens->valuedouble >= 0 && out->tokens >= 0)
            out->tokens += (int32_t)(tokens->valuedouble < 1e6 ? tokens->valuedouble : 1e6);
        else
            out->tokens = -1;
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "truncated")))
            out->truncated++;
    }
    cJSON_Delete(root);
    if (ok)
        out->count = req->count;
    return ok;
}

//! Set once the server refuses base64; from then on vectors come as float arrays.
static atomic_bool g_float_only;

ai_embed_status_t ai_embed_vectors(const ai_embed_request_t* req, ai_embed_result_t* out,
    int32_t* retry_after_ms, const atomic_bool* cancel)
{
    memset(out, 0, sizeof(*out));
    if (retry_after_ms)
        *retry_after_ms = 0;
    if (!req || !req->model || !req->inputs || !req->inputs_len || req->count < 1 || req->count > AI_EMBED_BATCH_MAX
        || req->dims < 0 || req->dims > MAX_DIMS)
        return AI_EMBED_REJECTED;

    for (int32_t attempt = 0; attempt < 2; attempt++) {
        bool base64 = !atomic_load(&g_float_only);
        char* body = build_request(req, base64);
        if (!body)
            return AI_EMBED_ERROR;
        resp_t resp;
        // A batch waits behind a live chat reply at background priority; give it time.
        ai_embed_status_t st = http_call("/embeddings", body, 120L, cancel, &resp, retry_after_ms);
        free(body);

        if (st == AI_EMBED_REJECTED && base64) {
            cJSON* root = resp.data ? cJSON_Parse(resp.data) : NULL;
            bool no_base64 = strcmp(error_code(root), "unsupported_encoding_format") == 0;
            cJSON_Delete(root);
            if (no_base64) {
                atomic_store(&g_float_only, true);
                resp_free(&resp);
                continue;
            }
        }
        if (st == AI_EMBED_OK && !parse_vectors(resp.data, req, out)) {
            ai_embed_result_free(out);
            st = AI_EMBED_ERROR;
        }
        resp_free(&resp);
        return st;
    }
    return AI_EMBED_REJECTED;
}

void ai_embed_result_free(ai_embed_result_t* r)
{
    if (!r)
        return;
    free(r->vectors);
    memset(r, 0, sizeof(*r));
}

// #endregion
