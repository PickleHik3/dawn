/**
 * @file ai_speak.c
 * @brief ai_speak.h: sentence-at-a-time read-aloud through POST /v1/ai/speak.
 *
 * Threading follows ai_bridge_openai.c: a detached worker per run, and a generation number in
 * place of a join. ai_speak_stop() (or a new start) moves g_active_gen on; the worker checks it
 * between sentences and from curl's progress callback, and once it no longer matches, the worker
 * stops writing the shared status and frees its own job on the way out. So nothing ever waits on
 * the network from dawn's thread.
 *
 * Request: {"input": "<sentence>"} as JSON (the launcher also takes plain text, but JSON keeps
 * quotes and newlines exact). Answer: {ok, audioSeconds, firstSoundMs, played, stopped, ...} on
 * success; {error: {code, message}} or {ok:false, error, message} with a 4xx/5xx on failure.
 */

#include "ai_speak.h"

#include "ai_bridge.h"
#include "cJSON.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SPEAK_MAX_SENTENCES 8192
#define SPEAK_MAX_BODY (256u * 1024u) //!< The answer is a small JSON object; anything bigger is cut

// #region Shared state

typedef struct {
    char** texts;
    size_t* lens;
    int32_t count;
    uint32_t gen;
} speak_job_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_uint g_active_gen; //!< The run allowed to go on; 0 = none
static uint32_t g_next_gen = 1; //!< dawn's thread only

static struct {
    uint32_t gen; //!< Whose status this is
    ai_speak_phase_t phase;
    int32_t index;
    int64_t sent_ms; //!< Monotonic time the sentence at index went out
    double sec_per_byte;
    int32_t lead_in_ms;
    char error[128];
} g_st = { .lead_in_ms = -1 };

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void job_free(speak_job_t* job)
{
    if (!job)
        return;
    for (int32_t i = 0; i < job->count; i++)
        free(job->texts[i]);
    free(job->texts);
    free(job->lens);
    free(job);
}

//! Copy a reason into the status, bounded, lower-casing the first letter to match notices.
static void set_error_locked(const char* msg)
{
    if (!msg || !msg[0])
        msg = "read aloud failed";
    size_t n = strlen(msg);
    if (n >= sizeof(g_st.error))
        n = sizeof(g_st.error) - 1;
    memcpy(g_st.error, msg, n);
    g_st.error[n] = '\0';
    if (g_st.error[0] >= 'A' && g_st.error[0] <= 'Z')
        g_st.error[0] = (char)(g_st.error[0] - 'A' + 'a');
}

// #endregion

// #region HTTP

typedef struct {
    char* data;
    size_t len;
    size_t cap;
} body_t;

static size_t write_body(char* data, size_t size, size_t count, void* userp)
{
    body_t* b = userp;
    size_t n = size * count;
    if (b->len + n + 1 > SPEAK_MAX_BODY)
        n = b->len + 1 < SPEAK_MAX_BODY ? SPEAK_MAX_BODY - b->len - 1 : 0;
    if (n > 0) {
        if (b->len + n + 1 > b->cap) {
            size_t cap = b->cap ? b->cap : 1024;
            while (b->len + n + 1 > cap)
                cap *= 2;
            char* grown = realloc(b->data, cap);
            if (!grown)
                return 0; // aborts the transfer
            b->data = grown;
            b->cap = cap;
        }
        memcpy(b->data + b->len, data, n);
        b->len += n;
        b->data[b->len] = '\0';
    }
    return size * count; // extra bytes past the cap are dropped, not an error
}

//! curl's progress callback: nonzero aborts, once this run is no longer the active one.
static int abort_if_stale(void* userp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    uint32_t gen = (uint32_t)(uintptr_t)userp;
    return atomic_load(&g_active_gen) != gen ? 1 : 0;
}

//! POST body (JSON) to base_url + path. Returns the answer (malloc'd, may be NULL) and the HTTP
//! status in *status (0 when there was no answer at all). gen = 0 means "not abortable".
static char* post_json(const char* base_url, const char* api_key, const char* path, const char* body,
    long timeout_s, uint32_t gen, long* status)
{
    *status = 0;
    CURL* curl = curl_easy_init();
    if (!curl)
        return NULL;

    size_t url_len = strlen(base_url) + strlen(path) + 1;
    char* url = malloc(url_len);
    char* auth = NULL;
    struct curl_slist* headers = NULL;
    body_t answer = { 0 };
    if (!url)
        goto done;
    snprintf(url, url_len, "%s%s", base_url, path);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (api_key && api_key[0]) {
        size_t auth_len = strlen(api_key) + 32;
        auth = malloc(auth_len);
        if (auth) {
            snprintf(auth, auth_len, "Authorization: Bearer %s", api_key);
            headers = curl_slist_append(headers, auth);
        }
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &answer);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "dawn");
    if (gen) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, abort_if_stale);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, (void*)(uintptr_t)gen);
    }
    if (curl_easy_perform(curl) == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);

done:
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(auth);
    free(url);
    if (*status == 0) {
        free(answer.data);
        return NULL;
    }
    return answer.data;
}

//! The launcher's error text from either failure shape, or NULL.
static const char* error_message(cJSON* root)
{
    cJSON* err = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsObject(err)) {
        cJSON* msg = cJSON_GetObjectItemCaseSensitive(err, "message");
        if (cJSON_IsString(msg) && msg->valuestring && msg->valuestring[0])
            return msg->valuestring;
        cJSON* code = cJSON_GetObjectItemCaseSensitive(err, "code");
        if (cJSON_IsString(code) && code->valuestring)
            return code->valuestring;
    }
    cJSON* msg = cJSON_GetObjectItemCaseSensitive(root, "message");
    if (cJSON_IsString(msg) && msg->valuestring && msg->valuestring[0])
        return msg->valuestring;
    if (cJSON_IsString(err) && err->valuestring)
        return err->valuestring;
    return NULL;
}

// #endregion

// #region Worker

static void* speak_thread(void* arg)
{
    speak_job_t* job = arg;
    char* base_url = NULL;
    char* api_key = NULL;
    char* cfg_error = NULL;
    ai_speak_phase_t end = AI_SPEAK_DONE;
    char reason[128] = "";

    if (!ai_bridge_endpoint(&base_url, &api_key, &cfg_error)) {
        end = AI_SPEAK_FAILED;
        snprintf(reason, sizeof(reason), "%s", cfg_error ? cfg_error : "can't find the launcher");
        // "Error: Can't find TAI. ..." reads better on the status line without the prefix
        if (strncmp(reason, "Error: ", 7) == 0)
            memmove(reason, reason + 7, strlen(reason + 7) + 1);
        goto finish;
    }

    for (int32_t i = 0; i < job->count; i++) {
        if (atomic_load(&g_active_gen) != job->gen) {
            end = AI_SPEAK_STOPPED;
            break;
        }

        pthread_mutex_lock(&g_lock);
        if (g_st.gen == job->gen) {
            g_st.index = i;
            g_st.sent_ms = mono_ms();
        }
        pthread_mutex_unlock(&g_lock);

        cJSON* req = cJSON_CreateObject();
        char* text = job->texts[i];
        char* body = NULL;
        if (req && cJSON_AddStringToObject(req, "input", text))
            body = cJSON_PrintUnformatted(req);
        cJSON_Delete(req);
        if (!body) {
            end = AI_SPEAK_FAILED;
            snprintf(reason, sizeof(reason), "out of memory");
            break;
        }

        // The call answers once the sentence has been heard: synthesis plus playback, with the
        // launcher's own deadline (60 s + 200 ms per character) as the outer bound.
        long timeout_s = 90L + (long)(job->lens[i] / 4);
        long status = 0;
        char* answer = post_json(base_url, api_key, "/ai/speak", body, timeout_s, job->gen, &status);
        free(body);

        if (atomic_load(&g_active_gen) != job->gen) {
            free(answer);
            end = AI_SPEAK_STOPPED;
            break;
        }
        if (status == 0) {
            free(answer);
            end = AI_SPEAK_FAILED;
            snprintf(reason, sizeof(reason), "can't reach the launcher");
            break;
        }

        cJSON* root = answer ? cJSON_Parse(answer) : NULL;
        free(answer);
        if (status >= 400 || !cJSON_IsObject(root)
            || (cJSON_GetObjectItemCaseSensitive(root, "error")
                && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ok")))) {
            const char* msg = cJSON_IsObject(root) ? error_message(root) : NULL;
            snprintf(reason, sizeof(reason), "%s", msg ? msg : "read aloud failed");
            cJSON_Delete(root);
            end = AI_SPEAK_FAILED;
            break;
        }

        cJSON* secs = cJSON_GetObjectItemCaseSensitive(root, "audioSeconds");
        cJSON* first = cJSON_GetObjectItemCaseSensitive(root, "firstSoundMs");
        bool stopped = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "stopped"));
        pthread_mutex_lock(&g_lock);
        if (g_st.gen == job->gen) {
            if (cJSON_IsNumber(secs) && secs->valuedouble > 0.0 && job->lens[i] > 0)
                g_st.sec_per_byte = secs->valuedouble / (double)job->lens[i];
            if (cJSON_IsNumber(first) && first->valuedouble >= 0.0 && first->valuedouble < 60000.0)
                g_st.lead_in_ms = (int32_t)first->valuedouble;
        }
        pthread_mutex_unlock(&g_lock);
        cJSON_Delete(root);

        // Something else on the phone (the launcher's own stop, another app) cut it short.
        if (stopped) {
            end = AI_SPEAK_STOPPED;
            break;
        }
    }

finish:
    pthread_mutex_lock(&g_lock);
    if (g_st.gen == job->gen) {
        g_st.phase = end;
        if (end == AI_SPEAK_FAILED)
            set_error_locked(reason);
    }
    pthread_mutex_unlock(&g_lock);
    // A run nobody replaced ends here; leave the next start free to take generation 0 as "none".
    unsigned expected = job->gen;
    atomic_compare_exchange_strong(&g_active_gen, &expected, 0u);

    free(base_url);
    free(api_key);
    free(cfg_error);
    job_free(job);
    return NULL;
}

static void* stop_thread(void* arg)
{
    (void)arg;
    char* base_url = NULL;
    char* api_key = NULL;
    if (ai_bridge_endpoint(&base_url, &api_key, NULL)) {
        long status = 0;
        free(post_json(base_url, api_key, "/ai/speak/stop", "{}", 5L, 0, &status)); // best effort
    }
    free(base_url);
    free(api_key);
    return NULL;
}

// #endregion

// #region ai_speak.h

bool ai_speak_start(const char* const* texts, const size_t* lens, int32_t count)
{
    if (!texts || !lens || count <= 0)
        return false;
    if (count > SPEAK_MAX_SENTENCES)
        count = SPEAK_MAX_SENTENCES;
    if (atomic_load(&g_active_gen) != 0)
        ai_speak_stop();
    if (!ai_bridge_init()) // curl_global_init, once, on dawn's thread
        return false;

    speak_job_t* job = calloc(1, sizeof(*job));
    if (!job)
        return false;
    job->texts = calloc((size_t)count, sizeof(char*));
    job->lens = calloc((size_t)count, sizeof(size_t));
    if (!job->texts || !job->lens) {
        job_free(job);
        return false;
    }
    for (int32_t i = 0; i < count; i++) {
        job->texts[i] = malloc(lens[i] + 1);
        if (!job->texts[i]) {
            job->count = i;
            job_free(job);
            return false;
        }
        memcpy(job->texts[i], texts[i], lens[i]);
        job->texts[i][lens[i]] = '\0';
        job->lens[i] = lens[i];
    }
    job->count = count;

    uint32_t gen = g_next_gen++;
    if (g_next_gen == 0)
        g_next_gen = 1;
    job->gen = gen;

    pthread_mutex_lock(&g_lock);
    g_st.gen = gen;
    g_st.phase = AI_SPEAK_RUNNING;
    g_st.index = 0;
    g_st.sent_ms = mono_ms();
    g_st.error[0] = '\0';
    // sec_per_byte and lead_in_ms carry over from the last run: the voice hasn't changed
    pthread_mutex_unlock(&g_lock);
    atomic_store(&g_active_gen, gen);

    pthread_t t;
    if (pthread_create(&t, NULL, speak_thread, job) != 0) {
        atomic_store(&g_active_gen, 0u);
        pthread_mutex_lock(&g_lock);
        g_st.phase = AI_SPEAK_IDLE;
        pthread_mutex_unlock(&g_lock);
        job_free(job);
        return false;
    }
    pthread_detach(t);
    return true;
}

void ai_speak_stop(void)
{
    atomic_store(&g_active_gen, 0u);
    pthread_mutex_lock(&g_lock);
    bool was_running = g_st.phase == AI_SPEAK_RUNNING;
    if (was_running)
        g_st.phase = AI_SPEAK_STOPPED;
    g_st.gen = 0; // the old worker's late writes no longer land
    pthread_mutex_unlock(&g_lock);
    if (was_running) {
        pthread_t t;
        if (pthread_create(&t, NULL, stop_thread, NULL) == 0)
            pthread_detach(t);
    }
}

void ai_speak_status(ai_speak_status_t* out)
{
    if (!out)
        return;
    pthread_mutex_lock(&g_lock);
    out->phase = g_st.phase;
    out->index = g_st.index;
    out->elapsed_ms = g_st.phase == AI_SPEAK_RUNNING ? mono_ms() - g_st.sent_ms : 0;
    out->sec_per_byte = g_st.sec_per_byte;
    out->lead_in_ms = g_st.lead_in_ms;
    memcpy(out->error, g_st.error, sizeof(out->error));
    pthread_mutex_unlock(&g_lock);
}

void ai_speak_ack(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_st.phase != AI_SPEAK_RUNNING)
        g_st.phase = AI_SPEAK_IDLE;
    pthread_mutex_unlock(&g_lock);
}

// #endregion
