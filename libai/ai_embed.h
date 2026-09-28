/**
 * @file ai_embed.h
 * @brief Blocking calls to an OpenAI-compatible embeddings endpoint (Termux Launcher's TAI).
 *
 * Every function here blocks on the network and is meant for a worker thread (dawn_embed.c's
 * indexer and its query thread), never dawn's own. They reach the same endpoint, with the same
 * key, as the chat (ai_openai_endpoint(), in ai_bridge_openai.c), and hold no state between
 * calls. A cancel flag, when given, is checked by curl's progress callback, so a call in flight
 * stops within about a second of it being set.
 *
 * The contract is TAI's (dawn-embedding-response.md): /v1/models entries whose `_capabilities`
 * holds "text_embeddings", POST /v1/embeddings with `input_type`, `title`, `dimensions` and base64
 * vectors, POST /v1/tokenize, and `runtime.activeGeneration` in GET /v1/ai/runtime.
 */

#ifndef AI_EMBED_H
#define AI_EMBED_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AI_EMBED_ID_MAX 128 //!< Model id, bytes including the NUL
#define AI_EMBED_REVISION_MAX 64 //!< `_revision`, bytes including the NUL
#define AI_EMBED_BATCH_MAX 64 //!< Most inputs one request may carry

//! How a call ended.
typedef enum {
    AI_EMBED_OK,
    AI_EMBED_NONE, //!< No embedder: none listed, 404 model_not_found or 501; look again later
    AI_EMBED_RETRY, //!< 429, 503 or no connection: try again after *retry_after_ms
    AI_EMBED_REJECTED, //!< 400 or 413: this request will never succeed as it is
    AI_EMBED_CANCELLED, //!< The cancel flag was set
    AI_EMBED_ERROR, //!< Anything else (a 5xx, a reply that does not parse)
} ai_embed_status_t;

//! The embedder dawn uses, as /v1/models describes it.
typedef struct {
    char id[AI_EMBED_ID_MAX];
    char revision[AI_EMBED_REVISION_MAX]; //!< "" when the entry has none
    int32_t dims; //!< What to ask for: 256 when the model lists it, 0 to leave `dimensions` out
    int32_t max_batch; //!< `_endpoint_max_batch`, clamped to 1..AI_EMBED_BATCH_MAX (16 if absent)
    int32_t context_window; //!< `_endpoint_context_window`, 0 if absent
} ai_embedder_t;

//! One embeddings request.
typedef struct {
    const char* model; //!< The embedder's id (always sent)
    const char* const* inputs; //!< count texts, each inputs_len[i] bytes (need not be NUL-terminated)
    const size_t* inputs_len;
    int32_t count; //!< 1..AI_EMBED_BATCH_MAX
    int32_t dims; //!< `dimensions`, or 0 to leave it out
    bool query; //!< input_type "query" (true) or "document" (false)
    const char* title; //!< Documents only: the heading the inputs sit under, NULL for none
} ai_embed_request_t;

//! One embeddings reply.
typedef struct {
    float* vectors; //!< count * dims values in request order (malloc'd; free with ai_embed_result_free)
    int32_t dims; //!< Values per vector
    int32_t count;
    int32_t tokens; //!< Sum of `data[i].tokens` (the model's own count, prefix included), -1 if absent
    int32_t truncated; //!< How many inputs the server cut to fit its window
} ai_embed_result_t;

//! The endpoint the chat talks to: *base_url ends in /v1, *api_key is NULL when none is set. Both
//! are malloc'd; caller frees. False when no endpoint is configured (TAI off, bad ai.json).
//! Implemented in ai_bridge_openai.c, which owns the configuration.
bool ai_openai_endpoint(char** base_url, char** api_key);

//! Find the embedder in GET /v1/models: the first entry whose `_capabilities` include
//! "text_embeddings", preferring an EmbeddingGemma. AI_EMBED_NONE when there is none.
ai_embed_status_t ai_embed_find_embedder(ai_embedder_t* out, const atomic_bool* cancel);

//! Whether a chat reply is being generated now (`runtime.activeGeneration` in GET /v1/ai/runtime).
//! A reply without the field counts as not generating.
ai_embed_status_t ai_embed_generation_active(bool* active, const atomic_bool* cancel);

//! The model's own token count for text (POST /v1/tokenize), without prefix or BOS/EOS.
//! AI_EMBED_NONE when the endpoint does not tokenize (501).
ai_embed_status_t ai_embed_tokenize(const char* model, const char* text, size_t len, int32_t* tokens,
    const atomic_bool* cancel);

//! Embed a batch (POST /v1/embeddings, base64 vectors, falling back to float arrays). On
//! AI_EMBED_OK *out holds count vectors in request order, each of one length (dims when asked).
//! On AI_EMBED_RETRY *retry_after_ms says how long to wait (Retry-After, or a default).
ai_embed_status_t ai_embed_vectors(const ai_embed_request_t* req, ai_embed_result_t* out,
    int32_t* retry_after_ms, const atomic_bool* cancel);

//! Free a reply's vectors and zero it.
void ai_embed_result_free(ai_embed_result_t* r);

#endif // AI_EMBED_H
