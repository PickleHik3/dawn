// dawn_embed.c - the background indexer and the query side of the meaning index.
//
// Threads. dawn's own thread calls the public functions. One detached worker thread owns all
// indexing: it discovers the embedder, scans the notes, embeds pieces and writes index files. A
// second, short-lived detached thread embeds search queries, so a query is never stuck behind an
// indexing batch; at most one exists, and it takes only the newest query text. Both follow
// libai's pattern: detached, a shared stop flag checked between steps and by curl's progress
// callback, and an "alive" flag that embed_shutdown() waits on for a moment before freeing.
//
// Sharing. g_lock guards the store (the in-memory copy of every note's index), the embedder
// description, the pending live texts and the query slot and cache. The worker is the store's
// only writer, so it reads the store without the lock and takes it only to swap entries in or
// out; dawn's thread holds it while it ranks, which is a few milliseconds at most.

#include "dawn_embed.h"

#include <string.h>

bool embed_hit_matches(const EmbedHit* hit, const char* slice, size_t slice_len)
{
    return hit && slice && slice_len == hit->len && embed_hash(slice, slice_len) == hit->text_hash;
}

#if DAWN_EMBED_LIVE

#include "ai_embed.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

// #region Tuning

#define EMBED_MAX_NOTES 8192 //!< Notes kept in the index; beyond this new ones are skipped
#define EMBED_LIVE_MAX 8 //!< Live texts waiting; the oldest is dropped (the disk scan catches up)
#define EMBED_SETTLE_MS 5000 //!< A live text waits until the note has been still this long
#define EMBED_BATCH 8 //!< Pieces per request: ~3 s at 2.5 pieces/s keeps pausing responsive
#define EMBED_REQUEST_GAP_MS 1000 //!< At least this between indexing requests (limit: 60/min)
#define EMBED_DISCOVER_OK_MS (10 * 60 * 1000) //!< Look at /v1/models again (catches `_revision`)
#define EMBED_DISCOVER_NONE_MS (5 * 60 * 1000) //!< ...and this often while there is no embedder
#define EMBED_DISCOVER_FAIL_MS (60 * 1000) //!< ...and after the endpoint did not answer
#define EMBED_RESCAN_MS (5 * 60 * 1000) //!< Rescan the notes directory for changed files
#define EMBED_IDLE_WAIT_MS 30000 //!< Longest sleep with nothing to do
#define EMBED_BUSY_POLL_MS 3000 //!< How often to re-check a running chat reply
#define EMBED_MAX_RETRIES 6 //!< Retries of one batch before the note waits for the next scan
#define EMBED_QUERY_MAX 512 //!< Query bytes kept (longer queries are cut)
#define EMBED_QUERY_DEBOUNCE_MS 250 //!< A query goes out once typing pauses this long
#define EMBED_QUERY_CACHE 4 //!< Query vectors remembered
#define EMBED_QUERY_RETRY_MS 10000 //!< A failed query may be tried again after this
#define EMBED_RELATED_CACHE 8 //!< embed_related() answers remembered for the current note

// #endregion

// #region State

//! One note in memory: its index plus the mean of its vectors, for whole-note similarity.
typedef struct {
    EmbedIndex idx;
    float* centroid; //!< idx.dims values, unit length; NULL when the note has no pieces
} StoreNote;

typedef struct {
    char* path;
    char* title; //!< NULL: work it out from the body
    char* body;
    size_t len;
    int64_t changed_ms;
} LiveText;

typedef struct {
    uint64_t hash;
    uint64_t epoch; //!< g.epoch when it was embedded; any other epoch makes it unusable
    bool failed;
    int64_t done_ms;
    int64_t used; //!< LRU tick
    int32_t dims;
    float* vec;
} QueryVec;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wake = PTHREAD_COND_INITIALIZER;

static struct {
    bool started;
    atomic_bool stop;
    atomic_bool worker_alive;
    atomic_bool query_alive;
    atomic_bool news; //!< For embed_poll()

    // Set once by embed_start(), read-only afterwards.
    char notes_dir[EMBED_PATH_MAX];
    char cache_dir[EMBED_PATH_MAX];
    char** extra;
    int32_t extra_count;

    // Guarded by g_lock (the worker may read the store without it; see the file comment).
    StoreNote** notes;
    int32_t note_count;
    int32_t note_cap;
    uint64_t generation; //!< Bumped on every store change
    bool have_embedder;
    ai_embedder_t embedder;
    int32_t dims; //!< Vector length in use: embedder.dims, or learned from the first reply
    uint64_t epoch; //!< Bumped whenever the embedder's id, revision or dims change

    LiveText live[EMBED_LIVE_MAX];
    int32_t live_count;
    char* renames[EMBED_LIVE_MAX][2]; //!< embed_note_renamed() pairs (old, new) for the worker
    int32_t rename_count;

    char q_text[EMBED_QUERY_MAX];
    size_t q_len;
    uint64_t q_hash;
    bool q_pending; //!< q_text waits to be sent
    uint64_t q_inflight; //!< Hash of the query being embedded now, 0 for none
    int64_t q_changed_ms;
    QueryVec qcache[EMBED_QUERY_CACHE];
    int64_t q_tick;
} g;

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(int64_t ms)
{
    if (ms <= 0)
        return;
    struct timespec ts = { .tv_sec = (time_t)(ms / 1000), .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

//! Sleep up to ms, waking early for embed_note_changed() or embed_shutdown().
static void worker_wait(int64_t ms)
{
    if (ms <= 0 || atomic_load(&g.stop))
        return;
    if (ms > EMBED_IDLE_WAIT_MS)
        ms = EMBED_IDLE_WAIT_MS;
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += (time_t)(ms / 1000);
    until.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&g_lock);
    if (!atomic_load(&g.stop))
        pthread_cond_timedwait(&g_wake, &g_lock, &until);
    pthread_mutex_unlock(&g_lock);
}

static char* dup_n(const char* s, size_t n)
{
    char* out = malloc(n + 1);
    if (!out)
        return NULL;
    if (n > 0)
        memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

//! "dir/name" into out; false when it does not fit.
static bool join_path(char* out, size_t out_size, const char* dir, const char* name)
{
    int n = snprintf(out, out_size, "%s/%s", dir, name);
    return n > 0 && (size_t)n < out_size;
}

static void mkdir_p(const char* path)
{
    char buf[EMBED_PATH_MAX];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(buf))
        return;
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (buf[i] != '/')
            continue;
        buf[i] = '\0';
        mkdir(buf, 0700);
        buf[i] = '/';
    }
    mkdir(buf, 0700);
}

// #endregion

// #region Store

static void store_note_free(StoreNote* n)
{
    if (!n)
        return;
    embed_index_free(&n->idx);
    free(n->centroid);
    free(n);
}

//! Wrap an index (taken over) with its centroid.
static StoreNote* store_note_new(EmbedIndex* idx)
{
    StoreNote* n = calloc(1, sizeof(StoreNote));
    if (!n) {
        embed_index_free(idx);
        return NULL;
    }
    n->idx = *idx;
    memset(idx, 0, sizeof(*idx));
    if (n->idx.count > 0 && n->idx.dims > 0) {
        n->centroid = calloc((size_t)n->idx.dims, sizeof(float));
        if (n->centroid) {
            for (int32_t i = 0; i < n->idx.count; i++) {
                const float* v = n->idx.vectors + (size_t)i * (size_t)n->idx.dims;
                for (int32_t d = 0; d < n->idx.dims; d++)
                    n->centroid[d] += v[d];
            }
            embed_normalize(n->centroid, n->idx.dims);
        }
    }
    return n;
}

static int32_t store_find(const char* path)
{
    for (int32_t i = 0; i < g.note_count; i++)
        if (strcmp(g.notes[i]->idx.path, path) == 0)
            return i;
    return -1;
}

//! Put a note in the store, replacing the one with its path. Worker thread only.
static void store_put(StoreNote* n)
{
    if (!n)
        return;
    StoreNote* old = NULL;
    pthread_mutex_lock(&g_lock);
    int32_t i = store_find(n->idx.path);
    if (i >= 0) {
        old = g.notes[i];
        g.notes[i] = n;
    } else if (g.note_count < EMBED_MAX_NOTES) {
        if (g.note_count == g.note_cap) {
            int32_t cap = g.note_cap ? g.note_cap * 2 : 64;
            StoreNote** grown = realloc(g.notes, sizeof(StoreNote*) * (size_t)cap);
            if (grown) {
                g.notes = grown;
                g.note_cap = cap;
            }
        }
        if (g.note_count < g.note_cap)
            g.notes[g.note_count++] = n;
        else
            old = n;
    } else {
        old = n;
    }
    g.generation++;
    pthread_mutex_unlock(&g_lock);
    store_note_free(old);
    atomic_store(&g.news, true);
}

//! Drop a note from the store and its file from the cache. Worker thread only.
static void store_remove(const char* path)
{
    StoreNote* old = NULL;
    pthread_mutex_lock(&g_lock);
    int32_t i = store_find(path);
    if (i >= 0) {
        old = g.notes[i];
        g.notes[i] = g.notes[--g.note_count];
        g.generation++;
    }
    pthread_mutex_unlock(&g_lock);
    if (old) {
        char name[32], file[EMBED_PATH_MAX];
        embed_index_file_name(old->idx.path, name, sizeof(name));
        if (join_path(file, sizeof(file), g.cache_dir, name))
            remove(file);
        store_note_free(old);
        atomic_store(&g.news, true);
    }
}

//! Carry renamed notes over (embed_note_renamed()): a note keeps its pieces under its new path and
//! its cache file is written again under the new name, so nothing is embedded twice and the old
//! path is never offered as a hit. Worker thread only.
static void apply_renames(void)
{
    char* pairs[EMBED_LIVE_MAX][2];
    pthread_mutex_lock(&g_lock);
    int32_t count = g.rename_count;
    memcpy(pairs, g.renames, sizeof(pairs[0]) * (size_t)count);
    g.rename_count = 0;
    pthread_mutex_unlock(&g_lock);
    for (int32_t r = 0; r < count; r++) {
        const char* from = pairs[r][0];
        const char* to = pairs[r][1];
        pthread_mutex_lock(&g_lock);
        int32_t i = store_find(from);
        bool moved = i >= 0 && store_find(to) < 0;
        if (moved) {
            snprintf(g.notes[i]->idx.path, sizeof(g.notes[i]->idx.path), "%s", to);
            g.generation++;
        }
        pthread_mutex_unlock(&g_lock);
        if (moved) {
            // Only the worker changes the store, so g.notes[i] stays put without the lock.
            char name[32], file[EMBED_PATH_MAX];
            embed_index_file_name(to, name, sizeof(name));
            if (join_path(file, sizeof(file), g.cache_dir, name))
                embed_index_write(file, &g.notes[i]->idx);
            embed_index_file_name(from, name, sizeof(name));
            if (join_path(file, sizeof(file), g.cache_dir, name))
                remove(file);
            atomic_store(&g.news, true);
        } else {
            store_remove(from); // nothing to carry, or the new path is indexed already
        }
        free(pairs[r][0]);
        free(pairs[r][1]);
    }
}

//! Whether an index was made by the embedder in use now. dims 0 means "not learned yet": then
//! only the id and revision are compared.
static bool index_is_current(const EmbedIndex* idx, const ai_embedder_t* emb, int32_t dims)
{
    return strcmp(idx->model, emb->id) == 0 && strcmp(idx->revision, emb->revision) == 0
        && (dims == 0 || idx->dims == dims);
}

//! Read every index file in the cache into the store; a file that fails to read (truncated,
//! corrupt, another version, misnamed) is deleted and its note rebuilt later.
static void load_cache(void)
{
    DIR* dir = opendir(g.cache_dir);
    if (!dir)
        return;
    struct dirent* e;
    while ((e = readdir(dir)) != NULL && !atomic_load(&g.stop)) {
        size_t n = strlen(e->d_name);
        char file[EMBED_PATH_MAX];
        if (!join_path(file, sizeof(file), g.cache_dir, e->d_name))
            continue;
        if (n > 4 && strcmp(e->d_name + n - 4, ".tmp") == 0) {
            remove(file); // a write that never finished
            continue;
        }
        if (n != 20 || strcmp(e->d_name + 16, ".idx") != 0)
            continue;
        EmbedIndex idx;
        char expect[32];
        if (!embed_index_read(file, &idx)) {
            remove(file);
            continue;
        }
        embed_index_file_name(idx.path, expect, sizeof(expect));
        if (strcmp(expect, e->d_name) != 0) {
            embed_index_free(&idx);
            remove(file);
            continue;
        }
        store_put(store_note_new(&idx));
    }
    closedir(dir);
}

// #endregion

// #region Embedder

//! The vector length an embedder's replies will have, when /v1/models says: the Matryoshka size
//! asked for, else `_endpoint_dimensions`; 0 when it is learned from the first reply.
static int32_t embedder_dims(const ai_embedder_t* emb) { return emb->dims > 0 ? emb->dims : emb->native_dims; }

//! Record the embedder found by discovery; a change of id, revision or dims starts a new epoch
//! (every note is then stale and query vectors are thrown away). Returns whether it changed.
static bool set_embedder(const ai_embedder_t* emb)
{
    pthread_mutex_lock(&g_lock);
    bool changed = !g.have_embedder || strcmp(g.embedder.id, emb->id) != 0
        || strcmp(g.embedder.revision, emb->revision) != 0 || g.embedder.dims != emb->dims
        || g.embedder.native_dims != emb->native_dims;
    g.have_embedder = true;
    if (changed) {
        g.embedder = *emb;
        g.dims = embedder_dims(emb);
        g.epoch++;
    } else {
        g.embedder.max_batch = emb->max_batch;
        g.embedder.context_window = emb->context_window;
    }
    pthread_mutex_unlock(&g_lock);
    if (changed)
        atomic_store(&g.news, true);
    return changed;
}

static void clear_embedder(void)
{
    pthread_mutex_lock(&g_lock);
    bool had = g.have_embedder;
    g.have_embedder = false;
    pthread_mutex_unlock(&g_lock);
    if (had)
        atomic_store(&g.news, true);
}

// #endregion

// #region Scan

typedef struct {
    char* path;
    int64_t mtime;
    uint64_t size;
} ScanEntry;

typedef struct {
    ScanEntry* items;
    int32_t count;
    int32_t cap;
    int32_t next; //!< Next item to process
} ScanList;

static void scan_free(ScanList* s)
{
    for (int32_t i = 0; i < s->count; i++)
        free(s->items[i].path);
    free(s->items);
    memset(s, 0, sizeof(*s));
}

static bool scan_has(const ScanList* s, const char* path)
{
    for (int32_t i = 0; i < s->count; i++)
        if (strcmp(s->items[i].path, path) == 0)
            return true;
    return false;
}

//! Add a regular file of indexable size.
static void scan_add(ScanList* s, const char* path)
{
    if (s->count >= EMBED_MAX_NOTES || strlen(path) >= EMBED_PATH_MAX)
        return;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || (uint64_t)st.st_size > EMBED_NOTE_MAX)
        return;
    if (s->count == s->cap) {
        int32_t cap = s->cap ? s->cap * 2 : 64;
        ScanEntry* grown = realloc(s->items, sizeof(ScanEntry) * (size_t)cap);
        if (!grown)
            return;
        s->items = grown;
        s->cap = cap;
    }
    char* copy = dup_n(path, strlen(path));
    if (!copy)
        return;
    s->items[s->count++] = (ScanEntry) { copy, (int64_t)st.st_mtime, (uint64_t)st.st_size };
}

static int cmp_newest_first(const void* a, const void* b)
{
    const ScanEntry* x = a;
    const ScanEntry* y = b;
    return (x->mtime < y->mtime) - (x->mtime > y->mtime);
}

//! List the notes whose index is missing or stale (newest first), and forget notes whose file is
//! gone (deleted, or renamed by a live title).
static void scan_build(ScanList* out, const ai_embedder_t* emb, int32_t dims)
{
    scan_free(out);
    ScanList all = { 0 };
    DIR* dir = opendir(g.notes_dir);
    if (dir) {
        struct dirent* e;
        while ((e = readdir(dir)) != NULL) {
            size_t n = strlen(e->d_name);
            char path[EMBED_PATH_MAX];
            if (e->d_name[0] == '.' || n < 4 || strcmp(e->d_name + n - 3, ".md") != 0)
                continue;
            if (join_path(path, sizeof(path), g.notes_dir, e->d_name))
                scan_add(&all, path);
        }
        closedir(dir);
    }
    for (int32_t i = 0; i < g.extra_count; i++)
        if (!scan_has(&all, g.extra[i]))
            scan_add(&all, g.extra[i]);

    // Notes whose file is gone. Collected first: store_remove() reorders the store.
    char** gone = NULL;
    int32_t gone_count = 0;
    for (int32_t i = 0; i < g.note_count; i++) {
        struct stat st;
        if (stat(g.notes[i]->idx.path, &st) == 0)
            continue;
        char** grown = realloc(gone, sizeof(char*) * (size_t)(gone_count + 1));
        if (!grown)
            break;
        gone = grown;
        gone[gone_count] = dup_n(g.notes[i]->idx.path, strlen(g.notes[i]->idx.path));
        if (gone[gone_count])
            gone_count++;
    }
    for (int32_t i = 0; i < gone_count; i++) {
        store_remove(gone[i]);
        free(gone[i]);
    }
    free(gone);

    for (int32_t i = 0; i < all.count; i++) {
        ScanEntry* se = &all.items[i];
        int32_t at = store_find(se->path);
        if (at >= 0) {
            const EmbedIndex* idx = &g.notes[at]->idx;
            if (index_is_current(idx, emb, dims) && idx->mtime == se->mtime && idx->size == se->size) {
                free(se->path);
                continue;
            }
        }
        if (out->count == out->cap) {
            int32_t cap = out->cap ? out->cap * 2 : 64;
            ScanEntry* grown = realloc(out->items, sizeof(ScanEntry) * (size_t)cap);
            if (!grown) {
                free(se->path);
                continue;
            }
            out->items = grown;
            out->cap = cap;
        }
        out->items[out->count++] = *se;
    }
    free(all.items);
    if (out->count > 1)
        qsort(out->items, (size_t)out->count, sizeof(ScanEntry), cmp_newest_first);
}

// #endregion

// #region Indexing one note

typedef enum { JOB_DONE, JOB_FAILED, JOB_NO_EMBEDDER, JOB_CANCELLED } JobResult;

//! What the worker carries from note to note.
typedef struct {
    ai_embedder_t emb;
    int32_t dims; //!< 0 until the first reply when the model lists no Matryoshka sizes
    float token_scale; //!< The model's tokens over the estimate
    bool calibrated; //!< /v1/tokenize was tried for this embedder
    int64_t last_request_ms;
    int64_t busy_checked_ms;
    bool busy;
} Worker;

//! Block while a chat reply is generating (checked at most every EMBED_BUSY_POLL_MS). False when
//! asked to stop.
static bool wait_until_idle(Worker* w)
{
    for (;;) {
        if (atomic_load(&g.stop))
            return false;
        int64_t now = now_ms();
        if (now - w->busy_checked_ms >= EMBED_BUSY_POLL_MS || w->busy_checked_ms == 0) {
            bool active = false;
            ai_embed_status_t st = ai_embed_generation_active(&active, &g.stop);
            if (st == AI_EMBED_CANCELLED)
                return false;
            w->busy = st == AI_EMBED_OK && active;
            w->busy_checked_ms = now_ms();
        }
        if (!w->busy)
            return true;
        worker_wait(EMBED_BUSY_POLL_MS);
    }
}

//! One-off: the model's own token count against the estimate, on a sample of real text, so pieces
//! come out near their target. A 501 (not a LiteRT embedder) leaves the estimate alone; the
//! tokens reported with each batch refine it either way.
static void calibrate(Worker* w, const char* body, size_t len)
{
    if (w->calibrated || len < 200)
        return;
    w->calibrated = true;
    size_t n = len < 2000 ? len : 2000;
    while (n > 0 && n < len && ((unsigned char)body[n] & 0xC0) == 0x80)
        n--;
    int32_t est = embed_estimate_tokens(body, n);
    int32_t tokens = 0;
    if (est > 0 && ai_embed_tokenize(w->emb.id, body, n, &tokens, &g.stop) == AI_EMBED_OK && tokens > 0) {
        float s = (float)tokens / (float)est;
        w->token_scale = s < 0.5f ? 0.5f : s > 3.0f ? 3.0f : s;
    }
}

static int cmp_hash_slot(const void* a, const void* b)
{
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return (x > y) - (x < y);
}

//! Embed one batch of pieces with pacing, pausing and retries. vectors/dims are filled on success
//! (the caller frees *res).
static JobResult embed_batch(Worker* w, const char* const* texts, const size_t* lens, int32_t count,
    const char* title, ai_embed_result_t* res)
{
    int32_t retries = 0;
    for (;;) {
        if (!wait_until_idle(w))
            return JOB_CANCELLED;
        int64_t gap = w->last_request_ms + EMBED_REQUEST_GAP_MS - now_ms();
        if (gap > 0)
            worker_wait(gap);
        if (atomic_load(&g.stop))
            return JOB_CANCELLED;

        ai_embed_request_t req = {
            .model = w->emb.id,
            .inputs = texts,
            .inputs_len = lens,
            .count = count,
            .dims = w->emb.dims,
            .query = false,
            .title = title,
        };
        int32_t retry_ms = 0;
        ai_embed_status_t st = ai_embed_vectors(&req, res, &retry_ms, &g.stop);
        w->last_request_ms = now_ms();
        switch (st) {
        case AI_EMBED_OK:
            if (res->dims < 1 || res->dims > EMBED_MAX_DIMS || (w->dims > 0 && res->dims != w->dims)) {
                ai_embed_result_free(res);
                return JOB_NO_EMBEDDER; // the model changed under us: look again
            }
            return JOB_DONE;
        case AI_EMBED_CANCELLED:
            return JOB_CANCELLED;
        case AI_EMBED_NONE:
            return JOB_NO_EMBEDDER;
        case AI_EMBED_REJECTED:
            return JOB_FAILED;
        case AI_EMBED_RETRY:
        case AI_EMBED_ERROR:
            break;
        }
        if (++retries > EMBED_MAX_RETRIES)
            return JOB_FAILED;
        worker_wait(retry_ms > 0 ? retry_ms : 10000);
    }
}

//! Read a note file: its title and its body (frontmatter removed, LF endings), malloc'd.
static char* read_note(const char* path, char* title, size_t title_size, size_t* body_len)
{
    *body_len = 0;
    FILE* f = fopen(path, "rb");
    if (!f)
        return NULL;
    char* raw = NULL;
    size_t n = 0;
    if (fseek(f, 0, SEEK_END) == 0) {
        long size = ftell(f);
        if (size > 0 && (unsigned long)size <= EMBED_NOTE_MAX && fseek(f, 0, SEEK_SET) == 0) {
            raw = malloc((size_t)size + 1);
            n = raw ? fread(raw, 1, (size_t)size, f) : 0;
        }
    }
    fclose(f);
    if (!raw || n == 0) {
        free(raw);
        return NULL;
    }
    embed_note_title(raw, n, path, title, title_size);
    size_t off = embed_body_offset(raw, n);
    size_t len = n - off;
    memmove(raw, raw + off, len);
    len = embed_normalize_newlines(raw, len);
    raw[len] = '\0';
    *body_len = len;
    return raw;
}

//! Bring one note's index up to date. body is taken over (NULL: read the file). Pieces whose text
//! and heading are unchanged keep their vectors; only the rest are embedded.
static JobResult index_note(Worker* w, const char* path, char* body, size_t len, const char* live_title,
    int64_t mtime, uint64_t size)
{
    char title[EMBED_TITLE_MAX];
    if (!body) {
        body = read_note(path, title, sizeof(title), &len);
        if (!body)
            return JOB_FAILED;
    } else if (live_title && live_title[0]) {
        snprintf(title, sizeof(title), "%s", live_title);
    } else {
        embed_note_title(body, len, path, title, sizeof(title));
    }
    if (len > EMBED_NOTE_MAX || strlen(path) >= EMBED_PATH_MAX) {
        free(body);
        return JOB_FAILED;
    }

    uint64_t body_hash = embed_hash(body, len);
    int32_t at = store_find(path);
    const EmbedIndex* old = at >= 0 ? &g.notes[at]->idx : NULL;
    bool reusable = old && index_is_current(old, &w->emb, w->dims) && old->dims > 0;

    EmbedIndex idx = { 0 };
    snprintf(idx.model, sizeof(idx.model), "%s", w->emb.id);
    snprintf(idx.revision, sizeof(idx.revision), "%s", w->emb.revision);
    snprintf(idx.path, sizeof(idx.path), "%s", path);
    snprintf(idx.title, sizeof(idx.title), "%s", title);
    idx.body_hash = body_hash;
    idx.body_len = (uint32_t)len;
    idx.mtime = mtime;
    idx.size = size;

    // Same text, same model: only the file's metadata (or the title) moved. No request needed.
    if (reusable && old->body_hash == body_hash && old->body_len == len) {
        free(body);
        idx.dims = old->dims;
        idx.count = old->count;
        if (idx.count > 0) {
            idx.chunks = malloc(sizeof(EmbedChunk) * (size_t)idx.count);
            idx.vectors = malloc(sizeof(float) * (size_t)idx.count * (size_t)idx.dims);
            if (!idx.chunks || !idx.vectors) {
                embed_index_free(&idx);
                return JOB_FAILED;
            }
            memcpy(idx.chunks, old->chunks, sizeof(EmbedChunk) * (size_t)idx.count);
            memcpy(idx.vectors, old->vectors, sizeof(float) * (size_t)idx.count * (size_t)idx.dims);
        }
        goto store;
    }

    calibrate(w, body, len);
    int32_t cap = (int32_t)(len / 4 + 8 < EMBED_MAX_PIECES ? len / 4 + 8 : EMBED_MAX_PIECES);
    idx.chunks = malloc(sizeof(EmbedChunk) * (size_t)cap);
    if (!idx.chunks) {
        free(body);
        return JOB_FAILED;
    }
    idx.count = embed_chunk(body, len, w->token_scale, idx.chunks, cap);
    idx.dims = w->dims;

    JobResult result = JOB_DONE;
    bool* have = idx.count > 0 ? calloc((size_t)idx.count, sizeof(bool)) : NULL;
    if (idx.count > 0 && !have) {
        result = JOB_FAILED;
        goto fail;
    }

    // Carry over vectors of unchanged pieces: a sorted (hash, piece) table of the old index.
    if (reusable && old->count > 0 && idx.count > 0) {
        idx.vectors = malloc(sizeof(float) * (size_t)idx.count * (size_t)old->dims);
        uint64_t* table = malloc(sizeof(uint64_t) * 2 * (size_t)old->count);
        if (idx.vectors && table) {
            idx.dims = old->dims;
            for (int32_t i = 0; i < old->count; i++) {
                table[2 * i] = old->chunks[i].text_hash;
                table[2 * i + 1] = (uint64_t)i;
            }
            qsort(table, (size_t)old->count, sizeof(uint64_t) * 2, cmp_hash_slot);
            for (int32_t i = 0; i < idx.count; i++) {
                uint64_t key = idx.chunks[i].text_hash;
                uint64_t* hit = bsearch(&key, table, (size_t)old->count, sizeof(uint64_t) * 2, cmp_hash_slot);
                if (!hit)
                    continue;
                while (hit > table && hit[-2] == key)
                    hit -= 2; // the first of equal hashes
                for (; hit < table + 2 * (size_t)old->count && hit[0] == key; hit += 2) {
                    const EmbedChunk* oc = &old->chunks[hit[1]];
                    if (oc->len == idx.chunks[i].len && strcmp(oc->heading, idx.chunks[i].heading) == 0) {
                        memcpy(idx.vectors + (size_t)i * (size_t)idx.dims,
                            old->vectors + (size_t)hit[1] * (size_t)idx.dims, sizeof(float) * (size_t)idx.dims);
                        have[i] = true;
                        break;
                    }
                }
            }
        }
        free(table);
    }

    // Embed the rest, in batches of consecutive pieces that share one heading (a request carries
    // one `title`). Pieces above the first heading go under the note's title, unless it is dawn's
    // placeholder, which would only add noise ("none" is what the model was trained with).
    const char* note_heading = strcmp(title, "Untitled") == 0 ? "" : title;
    for (int32_t i = 0; i < idx.count && result == JOB_DONE;) {
        if (have[i]) {
            i++;
            continue;
        }
        const char* heading = idx.chunks[i].heading[0] ? idx.chunks[i].heading : note_heading;
        const char* texts[EMBED_BATCH];
        size_t lens[EMBED_BATCH];
        int32_t slots[EMBED_BATCH];
        int32_t limit = w->emb.max_batch < EMBED_BATCH ? w->emb.max_batch : EMBED_BATCH;
        if (limit < 1)
            limit = 1;
        int32_t n = 0;
        int32_t est = 0;
        int32_t j = i;
        for (; j < idx.count && n < limit; j++) {
            if (have[j])
                continue;
            const char* h = idx.chunks[j].heading[0] ? idx.chunks[j].heading : note_heading;
            if (strcmp(h, heading) != 0)
                break;
            texts[n] = body + idx.chunks[j].start;
            lens[n] = idx.chunks[j].len;
            slots[n] = j;
            est += embed_estimate_tokens(texts[n], lens[n]);
            n++;
        }

        ai_embed_result_t res;
        result = embed_batch(w, texts, lens, n, heading[0] ? heading : NULL, &res);
        if (result != JOB_DONE)
            break;
        if (w->dims == 0)
            w->dims = res.dims;
        if (idx.dims == 0 || !idx.vectors) {
            idx.dims = res.dims;
            free(idx.vectors);
            idx.vectors = malloc(sizeof(float) * (size_t)idx.count * (size_t)idx.dims);
            if (!idx.vectors) {
                ai_embed_result_free(&res);
                result = JOB_FAILED;
                break;
            }
        }
        if (res.dims != idx.dims) {
            ai_embed_result_free(&res);
            result = JOB_NO_EMBEDDER;
            break;
        }
        for (int32_t k = 0; k < n; k++) {
            float* dst = idx.vectors + (size_t)slots[k] * (size_t)idx.dims;
            memcpy(dst, res.vectors + (size_t)k * (size_t)idx.dims, sizeof(float) * (size_t)idx.dims);
            embed_normalize(dst, idx.dims);
            have[slots[k]] = true;
        }
        // The model's count includes the prefix; close enough to steer the next split.
        if (res.tokens > 0 && est > 0) {
            float s = (float)res.tokens / (float)est;
            s = s < 0.5f ? 0.5f : s > 3.0f ? 3.0f : s;
            w->token_scale = w->token_scale * 0.8f + s * 0.2f;
        }
        ai_embed_result_free(&res);
        i = j;
    }
    if (result != JOB_DONE)
        goto fail;
    if (idx.dims == 0) {
        // An empty note before the first reply ever: nothing to say about it yet.
        result = JOB_FAILED;
        goto fail;
    }
    free(have);
    free(body);

store: {
    char name[32], file[EMBED_PATH_MAX];
    embed_index_file_name(path, name, sizeof(name));
    if (join_path(file, sizeof(file), g.cache_dir, name))
        embed_index_write(file, &idx); // a failed write only costs a rebuild next session
    pthread_mutex_lock(&g_lock);
    if (g.dims == 0 && w->dims > 0)
        g.dims = w->dims;
    pthread_mutex_unlock(&g_lock);
    store_put(store_note_new(&idx));
    return JOB_DONE;
}

fail:
    free(have);
    free(body);
    embed_index_free(&idx);
    return result;
}

// #endregion

// #region Worker

//! Take the oldest live text that has settled. *next_ms gets the wait until the next one does
//! (or EMBED_IDLE_WAIT_MS).
static bool take_live(LiveText* out, int64_t now, int64_t* next_ms)
{
    *next_ms = EMBED_IDLE_WAIT_MS;
    bool found = false;
    pthread_mutex_lock(&g_lock);
    int32_t best = -1;
    for (int32_t i = 0; i < g.live_count; i++) {
        int64_t wait = g.live[i].changed_ms + EMBED_SETTLE_MS - now;
        if (wait <= 0) {
            if (best < 0 || g.live[i].changed_ms < g.live[best].changed_ms)
                best = i;
        } else if (wait < *next_ms) {
            *next_ms = wait;
        }
    }
    if (best >= 0) {
        *out = g.live[best];
        for (int32_t i = best; i + 1 < g.live_count; i++)
            g.live[i] = g.live[i + 1];
        g.live_count--;
        found = true;
    }
    pthread_mutex_unlock(&g_lock);
    return found;
}

static void* worker_main(void* arg)
{
    (void)arg;
    mkdir_p(g.cache_dir);
    load_cache();

    Worker w = { .token_scale = 1.0f };
    bool have = false;
    int64_t next_discover = 0, next_scan = 0;
    ScanList scan = { 0 };

    while (!atomic_load(&g.stop)) {
        apply_renames();
        int64_t now = now_ms();
        if (now >= next_discover) {
            ai_embedder_t found;
            ai_embed_status_t st = ai_embed_find_embedder(&found, &g.stop);
            if (st == AI_EMBED_CANCELLED)
                break;
            if (st == AI_EMBED_OK) {
                if (set_embedder(&found) || !have) {
                    w.emb = found;
                    w.dims = embedder_dims(&found);
                    w.calibrated = false;
                    w.token_scale = 1.0f;
                    next_scan = 0;
                }
                w.emb.max_batch = found.max_batch;
                have = true;
                next_discover = now + EMBED_DISCOVER_OK_MS;
            } else if (st == AI_EMBED_NONE) {
                clear_embedder();
                have = false;
                next_discover = now + EMBED_DISCOVER_NONE_MS;
            } else {
                next_discover = now + EMBED_DISCOVER_FAIL_MS;
            }
        }
        if (!have) {
            worker_wait(next_discover - now);
            continue;
        }
        if (now >= next_scan) {
            scan_build(&scan, &w.emb, w.dims);
            next_scan = now + EMBED_RESCAN_MS;
        }

        JobResult r;
        LiveText live;
        int64_t live_wait;
        if (take_live(&live, now, &live_wait)) {
            // Live text has no file time: a later scan re-reads the file and, finding the same
            // text, only records its time.
            r = index_note(&w, live.path, live.body, live.len, live.title, 0, 0);
            free(live.path);
            free(live.title);
        } else if (scan.next < scan.count) {
            ScanEntry* se = &scan.items[scan.next++];
            r = index_note(&w, se->path, NULL, 0, NULL, se->mtime, se->size);
        } else {
            int64_t wait = live_wait;
            if (next_scan - now < wait)
                wait = next_scan - now;
            if (next_discover - now < wait)
                wait = next_discover - now;
            worker_wait(wait);
            continue;
        }

        if (r == JOB_CANCELLED)
            break;
        if (r == JOB_NO_EMBEDDER) {
            clear_embedder();
            have = false;
            next_discover = now_ms() + 5000; // look again soon: it may have been replaced
        }
    }

    scan_free(&scan);
    atomic_store(&g.worker_alive, false);
    return NULL;
}

// #endregion

// #region Query thread

static QueryVec* qcache_find(uint64_t hash)
{
    for (int32_t i = 0; i < EMBED_QUERY_CACHE; i++)
        if (g.qcache[i].hash == hash && g.qcache[i].epoch == g.epoch && (g.qcache[i].vec || g.qcache[i].failed))
            return &g.qcache[i];
    return NULL;
}

//! Store a query's outcome (vec taken over; NULL when it failed). Caller holds g_lock.
static void qcache_put(uint64_t hash, uint64_t epoch, float* vec, int32_t dims)
{
    QueryVec* slot = &g.qcache[0];
    for (int32_t i = 0; i < EMBED_QUERY_CACHE; i++) {
        if (g.qcache[i].hash == hash) {
            slot = &g.qcache[i];
            break;
        }
        if (g.qcache[i].used < slot->used)
            slot = &g.qcache[i];
    }
    free(slot->vec);
    slot->hash = hash;
    slot->epoch = epoch;
    slot->vec = vec;
    slot->dims = dims;
    slot->failed = vec == NULL;
    slot->done_ms = now_ms();
    slot->used = ++g.q_tick;
}

static void* query_main(void* arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_lock);
        if (atomic_load(&g.stop) || !g.q_pending) {
            g.q_inflight = 0;
            atomic_store(&g.query_alive, false);
            pthread_mutex_unlock(&g_lock);
            return NULL;
        }
        int64_t wait = g.q_changed_ms + EMBED_QUERY_DEBOUNCE_MS - now_ms();
        if (wait > 0) {
            pthread_mutex_unlock(&g_lock);
            sleep_ms(wait < 50 ? wait : 50);
            continue;
        }
        char text[EMBED_QUERY_MAX];
        size_t len = g.q_len;
        memcpy(text, g.q_text, len);
        uint64_t hash = g.q_hash;
        g.q_pending = false;
        g.q_inflight = hash;
        bool have = g.have_embedder;
        ai_embedder_t emb = g.embedder;
        int32_t dims = g.dims;
        uint64_t epoch = g.epoch;
        pthread_mutex_unlock(&g_lock);

        float* vec = NULL;
        int32_t got = 0;
        if (have) {
            const char* inputs[1] = { text };
            size_t lens[1] = { len };
            ai_embed_request_t req = {
                .model = emb.id, .inputs = inputs, .inputs_len = lens, .count = 1, .dims = emb.dims, .query = true
            };
            ai_embed_result_t res;
            int32_t retry_ms;
            if (ai_embed_vectors(&req, &res, &retry_ms, &g.stop) == AI_EMBED_OK
                && (dims == 0 || res.dims == dims)) {
                vec = res.vectors;
                got = res.dims;
                res.vectors = NULL;
                embed_normalize(vec, got);
            }
            ai_embed_result_free(&res);
        }

        pthread_mutex_lock(&g_lock);
        qcache_put(hash, epoch, vec, got);
        g.q_inflight = 0;
        pthread_mutex_unlock(&g_lock);
        atomic_store(&g.news, true);
    }
}

// #endregion

// #region Public API

void embed_start(const char* notes_dir, const char* const* extra_paths, int32_t extra_count)
{
    if (g.started || !notes_dir || strlen(notes_dir) >= sizeof(g.notes_dir))
        return;

    const char* xdg = getenv("XDG_CACHE_HOME");
    const char* home = getenv("HOME");
    int n;
    if (xdg && xdg[0] == '/')
        n = snprintf(g.cache_dir, sizeof(g.cache_dir), "%s/dawn/embed", xdg);
    else if (home && home[0])
        n = snprintf(g.cache_dir, sizeof(g.cache_dir), "%s/.cache/dawn/embed", home);
    else
        return;
    if (n <= 0 || (size_t)n >= sizeof(g.cache_dir) - 24) // room for "/<16 hex>.idx.tmp"
        return;
    snprintf(g.notes_dir, sizeof(g.notes_dir), "%s", notes_dir);

    if (extra_paths && extra_count > 0) {
        if (extra_count > EMBED_MAX_NOTES)
            extra_count = EMBED_MAX_NOTES;
        g.extra = calloc((size_t)extra_count, sizeof(char*));
        for (int32_t i = 0; g.extra && i < extra_count; i++) {
            const char* p = extra_paths[i];
            if (p && p[0] && strlen(p) < EMBED_PATH_MAX && (g.extra[g.extra_count] = dup_n(p, strlen(p))))
                g.extra_count++;
        }
    }

    atomic_store(&g.stop, false);
    atomic_store(&g.worker_alive, true);
    pthread_t t;
    if (pthread_create(&t, NULL, worker_main, NULL) != 0) {
        atomic_store(&g.worker_alive, false);
        return;
    }
    pthread_detach(t);
    g.started = true;
}

void embed_shutdown(void)
{
    if (!g.started)
        return;
    pthread_mutex_lock(&g_lock);
    atomic_store(&g.stop, true);
    pthread_cond_broadcast(&g_wake);
    pthread_mutex_unlock(&g_lock);

    for (int32_t i = 0; i < 100 && (atomic_load(&g.worker_alive) || atomic_load(&g.query_alive)); i++)
        sleep_ms(10);
    g.started = false;
    if (atomic_load(&g.worker_alive) || atomic_load(&g.query_alive))
        return; // still unwinding a network call: leave everything to the process's exit

    for (int32_t i = 0; i < g.note_count; i++)
        store_note_free(g.notes[i]);
    free(g.notes);
    g.notes = NULL;
    g.note_count = g.note_cap = 0;
    for (int32_t i = 0; i < g.live_count; i++) {
        free(g.live[i].path);
        free(g.live[i].title);
        free(g.live[i].body);
    }
    g.live_count = 0;
    for (int32_t i = 0; i < g.rename_count; i++) {
        free(g.renames[i][0]);
        free(g.renames[i][1]);
    }
    g.rename_count = 0;
    for (int32_t i = 0; i < EMBED_QUERY_CACHE; i++) {
        free(g.qcache[i].vec);
        memset(&g.qcache[i], 0, sizeof(g.qcache[i]));
    }
    for (int32_t i = 0; i < g.extra_count; i++)
        free(g.extra[i]);
    free(g.extra);
    g.extra = NULL;
    g.extra_count = 0;
    g.have_embedder = false;
}

void embed_note_changed(const char* path, const char* title, const char* body, size_t len)
{
    if (!g.started || !path || !path[0] || strlen(path) >= EMBED_PATH_MAX || !body || len > EMBED_NOTE_MAX)
        return;
    LiveText item = {
        .path = dup_n(path, strlen(path)),
        .title = title && title[0] ? dup_n(title, strlen(title)) : NULL,
        .body = dup_n(body, len),
        .len = len,
        .changed_ms = now_ms(),
    };
    if (!item.path || !item.body || (title && title[0] && !item.title)) {
        free(item.path);
        free(item.title);
        free(item.body);
        return;
    }

    LiveText dropped = { 0 };
    pthread_mutex_lock(&g_lock);
    int32_t at = -1;
    for (int32_t i = 0; i < g.live_count; i++)
        if (strcmp(g.live[i].path, path) == 0)
            at = i;
    if (at >= 0) {
        dropped = g.live[at];
        g.live[at] = item;
    } else {
        if (g.live_count == EMBED_LIVE_MAX) {
            dropped = g.live[0];
            for (int32_t i = 1; i < g.live_count; i++)
                g.live[i - 1] = g.live[i];
            g.live_count--;
        }
        g.live[g.live_count++] = item;
    }
    pthread_cond_broadcast(&g_wake);
    pthread_mutex_unlock(&g_lock);
    free(dropped.path);
    free(dropped.title);
    free(dropped.body);
}

void embed_note_renamed(const char* old_path, const char* new_path)
{
    if (!g.started || !old_path || !new_path || !new_path[0] || strlen(new_path) >= EMBED_PATH_MAX
        || strcmp(old_path, new_path) == 0)
        return;
    char* from = dup_n(old_path, strlen(old_path));
    char* to = dup_n(new_path, strlen(new_path));
    pthread_mutex_lock(&g_lock);
    // Text still waiting under the old name goes in under the new one.
    for (int32_t i = 0; i < g.live_count; i++) {
        char* renamed = strcmp(g.live[i].path, old_path) == 0 ? dup_n(new_path, strlen(new_path)) : NULL;
        if (renamed) {
            free(g.live[i].path);
            g.live[i].path = renamed;
        }
    }
    if (from && to && g.rename_count < EMBED_LIVE_MAX) {
        g.renames[g.rename_count][0] = from;
        g.renames[g.rename_count][1] = to;
        g.rename_count++;
        from = to = NULL;
    }
    pthread_cond_broadcast(&g_wake);
    pthread_mutex_unlock(&g_lock);
    free(from); // queue full: the next rescan drops the old path instead
    free(to);
}

bool embed_ready(void)
{
    if (!g.started)
        return false;
    pthread_mutex_lock(&g_lock);
    bool ready = g.have_embedder && g.note_count > 0;
    pthread_mutex_unlock(&g_lock);
    return ready;
}

bool embed_poll(void) { return g.started && atomic_exchange(&g.news, false); }

//! Whether a stored note can be compared with vectors of the current embedder. Caller holds g_lock.
static bool note_comparable(const StoreNote* n, int32_t dims)
{
    return n->idx.dims == dims && strcmp(n->idx.model, g.embedder.id) == 0
        && strcmp(n->idx.revision, g.embedder.revision) == 0;
}

static void fill_hit(EmbedHit* hit, const StoreNote* n, int32_t chunk, float score)
{
    memset(hit, 0, sizeof(*hit));
    snprintf(hit->path, sizeof(hit->path), "%s", n->idx.path);
    snprintf(hit->note_title, sizeof(hit->note_title), "%s", n->idx.title);
    if (chunk >= 0 && chunk < n->idx.count) {
        const EmbedChunk* c = &n->idx.chunks[chunk];
        snprintf(hit->heading, sizeof(hit->heading), "%s", c->heading);
        hit->start = c->start;
        hit->len = c->len;
        hit->text_hash = c->text_hash;
    }
    hit->score = score;
}

//! Rank every comparable piece against q. Caller holds g_lock.
static int32_t rank_locked(const float* q, int32_t dims, const EmbedSearchOpts* opts, EmbedHit* hits, int32_t max)
{
    EmbedScored top[EMBED_HITS_MAX];
    int32_t n = 0;
    float min_score = opts ? opts->min_score : -2.0f;
    for (int32_t i = 0; i < g.note_count; i++) {
        const StoreNote* note = g.notes[i];
        if (!note_comparable(note, dims))
            continue;
        if (opts && opts->only_path && strcmp(note->idx.path, opts->only_path) != 0)
            continue;
        if (opts && opts->exclude_path && strcmp(note->idx.path, opts->exclude_path) == 0)
            continue;
        EmbedScored best = { i, -1, -2.0f };
        for (int32_t c = 0; c < note->idx.count; c++) {
            float s = embed_dot(q, note->idx.vectors + (size_t)c * (size_t)dims, dims);
            if (s < min_score)
                continue;
            EmbedScored cand = { i, c, s };
            if (opts && opts->one_per_note) {
                if (s > best.score)
                    best = cand;
            } else {
                embed_topk_push(top, &n, max, cand);
            }
        }
        if (opts && opts->one_per_note && best.chunk >= 0)
            embed_topk_push(top, &n, max, best);
    }
    for (int32_t i = 0; i < n; i++)
        fill_hit(&hits[i], g.notes[top[i].note], top[i].chunk, top[i].score);
    return n;
}

EmbedState embed_search(const char* query, size_t len, const EmbedSearchOpts* opts, EmbedHit* hits,
    int32_t max, int32_t* count)
{
    if (count)
        *count = 0;
    if (!g.started || !query || !hits || !count || max <= 0)
        return EMBED_UNAVAILABLE;
    if (max > EMBED_HITS_MAX)
        max = EMBED_HITS_MAX;
    while (len > 0 && (query[0] == ' ' || query[0] == '\t' || query[0] == '\n')) {
        query++;
        len--;
    }
    while (len > 0 && (query[len - 1] == ' ' || query[len - 1] == '\t' || query[len - 1] == '\n'))
        len--;
    if (len == 0)
        return EMBED_UNAVAILABLE;
    if (len > EMBED_QUERY_MAX - 1) {
        len = EMBED_QUERY_MAX - 1;
        while (len > 0 && ((unsigned char)query[len] & 0xC0) == 0x80)
            len--;
    }
    uint64_t hash = embed_hash(query, len);
    if (hash == 0)
        hash = 1; // 0 means "none" in q_inflight

    EmbedState state = EMBED_PENDING;
    bool spawn = false;
    pthread_mutex_lock(&g_lock);
    if (!g.have_embedder || g.note_count == 0) {
        state = EMBED_UNAVAILABLE;
    } else {
        QueryVec* qv = qcache_find(hash);
        if (qv && qv->failed && now_ms() - qv->done_ms < EMBED_QUERY_RETRY_MS) {
            state = EMBED_UNAVAILABLE;
        } else if (qv && qv->vec) {
            qv->used = ++g.q_tick;
            *count = rank_locked(qv->vec, qv->dims, opts, hits, max);
            state = EMBED_READY;
        } else if (g.q_inflight != hash && (!g.q_pending || g.q_hash != hash)) {
            memcpy(g.q_text, query, len);
            g.q_len = len;
            g.q_hash = hash;
            g.q_pending = true;
            g.q_changed_ms = now_ms();
        }
        if (state == EMBED_PENDING && g.q_pending && !atomic_load(&g.query_alive)) {
            atomic_store(&g.query_alive, true);
            spawn = true;
        }
    }
    pthread_mutex_unlock(&g_lock);

    if (spawn) {
        pthread_t t;
        if (pthread_create(&t, NULL, query_main, NULL) == 0) {
            pthread_detach(t);
        } else {
            pthread_mutex_lock(&g_lock);
            atomic_store(&g.query_alive, false);
            g.q_pending = false;
            pthread_mutex_unlock(&g_lock);
            state = EMBED_UNAVAILABLE;
        }
    }
    return state;
}

EmbedState embed_relevant(const char* path, const char* query, size_t len, EmbedHit* hits, int32_t max,
    int32_t* count)
{
    if (count)
        *count = 0;
    if (!path)
        return EMBED_UNAVAILABLE;
    EmbedSearchOpts opts = { .only_path = path, .min_score = -2.0f };
    return embed_search(query, len, &opts, hits, max, count);
}

//! embed_related()'s last answer (dawn's thread only).
static struct {
    uint64_t path_hash;
    uint64_t generation;
    uint64_t epoch;
    float min_score;
    int32_t max;
    int32_t count;
    EmbedHit hits[EMBED_RELATED_CACHE];
    bool valid;
} g_related;

int32_t embed_related(const char* path, float min_score, EmbedHit* out, int32_t max)
{
    if (!g.started || !path || !out || max <= 0)
        return 0;
    if (max > EMBED_RELATED_CACHE)
        max = EMBED_RELATED_CACHE;
    uint64_t path_hash = embed_hash(path, strlen(path));

    pthread_mutex_lock(&g_lock);
    if (g_related.valid && g_related.path_hash == path_hash && g_related.generation == g.generation
        && g_related.epoch == g.epoch && g_related.min_score == min_score && g_related.max == max) {
        int32_t n = g_related.count;
        memcpy(out, g_related.hits, sizeof(EmbedHit) * (size_t)n);
        pthread_mutex_unlock(&g_lock);
        return n;
    }

    int32_t n = 0;
    int32_t self = g.have_embedder ? store_find(path) : -1;
    const StoreNote* me = self >= 0 ? g.notes[self] : NULL;
    if (me && me->centroid && note_comparable(me, me->idx.dims)) {
        int32_t dims = me->idx.dims;
        EmbedScored top[EMBED_RELATED_CACHE];
        for (int32_t i = 0; i < g.note_count; i++) {
            const StoreNote* other = g.notes[i];
            if (i == self || !other->centroid || !note_comparable(other, dims))
                continue;
            float s = embed_dot(me->centroid, other->centroid, dims);
            if (s >= min_score)
                embed_topk_push(top, &n, max, (EmbedScored) { i, -1, s });
        }
        for (int32_t k = 0; k < n; k++) {
            // Name the other note's piece nearest this note as a whole.
            const StoreNote* other = g.notes[top[k].note];
            int32_t best = -1;
            float best_s = -2.0f;
            for (int32_t c = 0; c < other->idx.count; c++) {
                float s = embed_dot(me->centroid, other->idx.vectors + (size_t)c * (size_t)dims, dims);
                if (s > best_s) {
                    best_s = s;
                    best = c;
                }
            }
            fill_hit(&out[k], other, best, top[k].score);
        }
    }

    g_related.valid = true;
    g_related.path_hash = path_hash;
    g_related.generation = g.generation;
    g_related.epoch = g.epoch;
    g_related.min_score = min_score;
    g_related.max = max;
    g_related.count = n;
    memcpy(g_related.hits, out, sizeof(EmbedHit) * (size_t)n);
    pthread_mutex_unlock(&g_lock);
    return n;
}

// #endregion

#else // !DAWN_EMBED_LIVE

// No embeddings client on this build: every call is the "no embedder" answer.

void embed_start(const char* notes_dir, const char* const* extra_paths, int32_t extra_count)
{
    (void)notes_dir;
    (void)extra_paths;
    (void)extra_count;
}

void embed_shutdown(void) { }

void embed_note_changed(const char* path, const char* title, const char* body, size_t len)
{
    (void)path;
    (void)title;
    (void)body;
    (void)len;
}

void embed_note_renamed(const char* old_path, const char* new_path)
{
    (void)old_path;
    (void)new_path;
}

bool embed_ready(void) { return false; }

bool embed_poll(void) { return false; }

EmbedState embed_search(const char* query, size_t len, const EmbedSearchOpts* opts, EmbedHit* hits,
    int32_t max, int32_t* count)
{
    (void)query;
    (void)len;
    (void)opts;
    (void)hits;
    (void)max;
    if (count)
        *count = 0;
    return EMBED_UNAVAILABLE;
}

EmbedState embed_relevant(const char* path, const char* query, size_t len, EmbedHit* hits, int32_t max,
    int32_t* count)
{
    (void)path;
    return embed_search(query, len, NULL, hits, max, count);
}

int32_t embed_related(const char* path, float min_score, EmbedHit* out, int32_t max)
{
    (void)path;
    (void)min_score;
    (void)out;
    (void)max;
    return 0;
}

#endif // DAWN_EMBED_LIVE
