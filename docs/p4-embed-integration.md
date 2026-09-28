# P4 "meaning": wiring the embedding index into dawn

The index is self-contained: `src/dawn_embed.{c,h}` (worker thread, store, query API),
`src/dawn_embed_index.{c,h}` (pure: chunker, index file, ranking) and `libai/ai_embed.{c,h}`
(HTTP). CMake already picks up `src/dawn_embed*.c` through the `src/dawn*.c` glob and adds
`libai/ai_embed.c` to `libai_openai`. On builds without the OpenAI-compatible libai (web, Windows,
macOS, `USE_LIBAI=OFF`) every `embed_*` call is a no-op stub, so the hooks below need no `#if`.

Rule for every UI hook: **show nothing unless `embed_ready()` is true** and the call returned
hits. Until an embedder exists, nothing changes and nothing mentions it.

Line numbers are for `tl` at `658732f`.

## 1. Start and stop (dawn.c)

`#include "dawn_embed.h"` next to the other `dawn_*.h` includes (dawn.c:28-30).

**`dawn_engine_init()`** (dawn.c:5711): `hist_load()` fills `app.history` at 5742. Just before
the final `return true;` (5765), after the `#if HAS_LIBAI` block:

```c
    {
        // Every note in the notes directory, plus notes opened from elsewhere (the history).
        const char* extra[256];
        int32_t n = 0;
        for (int32_t i = 0; i < app.hist_count && n < 256; i++)
            extra[n++] = app.history[i].path;
        embed_start(history_dir(), extra, n);
    }
```

**`dawn_engine_shutdown()`** (dawn.c:5768): first thing, before the save at 5773:

```c
    embed_shutdown();
```

(Anything pending is simply lost; the note was saved, and the next session's scan re-embeds only
what changed.)

## 2. Tell the indexer when the open note changes (dawn_file.c)

**`save_session()`** (dawn_file.c:170): right after `app.dirty = false;` (211), inside the
`if (app.dirty)` block. `#include "dawn_embed.h"` at the top.

```c
        char* body = gap_to_str(&app.text);
        if (body) {
            embed_note_changed(app.session_path, fm_get_string(app.frontmatter, "title"), body,
                gap_len(&app.text));
            free(body);
        }
```

Autosave runs every 5 s while typing and each call replaces the last. The worker waits until the
note has been still for 5 s, then re-embeds only the pieces whose text changed.

## 3. Frame loop (dawn.c)

**`dawn_frame()`** (dawn.c:5817), next to `ai_pump();` (5839):

```c
    if (embed_poll())
        app.embed_news = true; // or any flag your cached results key on
```

`render()` runs every frame, so no redraw request is needed. The flag is there so the search and
chat hooks below re-rank only when something changed, not on every frame. Add
`bool embed_news;` to `App` (dawn_types.h), or keep a `static bool` in dawn.c.

## 4. Ctrl+S: exact matches, then a dim "by meaning" group

The exact search stays as it is (`search_find()` at dawn.c:3592, `render_search()` at
dawn_render.c:958). Add a second list under the exact results:

```c
// dawn.c or dawn_render.c
static EmbedHit g_meaning[5];
static int32_t g_meaning_count;
static char g_meaning_query[SEARCH_MAX_QUERY];

static void refresh_meaning(const SearchState* s)
{
    if (!embed_ready() || s->query_len < 3 || !app.session_path) {
        g_meaning_count = 0;
        return;
    }
    if (!app.embed_news && strcmp(g_meaning_query, s->query) == 0)
        return; // unchanged query, unchanged index
    app.embed_news = false;
    snprintf(g_meaning_query, sizeof(g_meaning_query), "%s", s->query);
    EmbedSearchOpts opts = { .only_path = app.session_path, .min_score = EMBED_SEARCH_MIN_SCORE };
    int32_t n = 0;
    if (embed_search(s->query, (size_t)s->query_len, &opts, g_meaning, 5, &n) != EMBED_READY)
        n = 0; // PENDING: embed_poll() fires when it lands and this runs again
    // Drop pieces the user has edited since they were indexed, and pieces that an exact match
    // already covers.
    int32_t kept = 0;
    size_t doc_len = gap_len(&app.text);
    for (int32_t i = 0; i < n; i++) {
        const EmbedHit* h = &g_meaning[i];
        if ((size_t)h->start + h->len > doc_len)
            continue;
        char* slice = gap_substr(&app.text, h->start, h->start + h->len);
        bool ok = slice && embed_hit_matches(h, slice, h->len);
        free(slice);
        for (int32_t k = 0; ok && k < s->count; k++)
            if (s->results[k].pos >= h->start && s->results[k].pos < (size_t)h->start + h->len)
                ok = false;
        if (ok)
            g_meaning[kept++] = *h;
    }
    g_meaning_count = kept;
}
```

Call `refresh_meaning(search)` after `search_find()` in the `MODE_SEARCH` case of `render()`
(dawn.c:3591-3595). In `render_search()`, under the exact list, only when
`g_meaning_count > 0`: a dim `by meaning` label, then one dim row per hit (its `heading`, or the
first line of the piece via `gap_substr`). Selecting one (the `'\r'` case of the `MODE_SEARCH`
key handler, dawn.c:5653) sets `app.cursor = hit.start;`, as an exact result does with `r->pos`.

To also search other notes, run a second query with
`{ .exclude_path = app.session_path, .one_per_note = true, .min_score = EMBED_SEARCH_MIN_SCORE }`
and open the hit with `load_file_for_editing(hit.path)` then `app.cursor = hit.start`. That query
reuses the cached query vector, so it costs no extra request.

## 5. "also in: <title>" under the chat (dawn.c)

In **`render_ai_panel()`** (dawn.c:2468), on the row between the last message and the input:

```c
    EmbedHit rel[1];
    if (embed_ready() && app.session_path
        && embed_related(app.session_path, EMBED_RELATED_MIN_SCORE, rel, 1) == 1) {
        char line[EMBED_TITLE_MAX + 16];
        snprintf(line, sizeof(line), "also in: %s", rel[0].note_title);
        // draw `line` dim, truncated to content_width
    }
```

`embed_related()` is cached until the index changes, so calling it every frame is fine.
`rel[0].path` is the note to open if the line is tapped. `rel[0].start`/`len` point at that
note's closest piece. `EMBED_RELATED_MIN_SCORE` (0.60) is a first guess for whole-note cosine and
should be tuned on the phone.

## 6. Relevance-picked snapshots (dawn_chat.c / dawn_ai_tokens.c)

**Where:** `note_context()` (dawn_chat.c:361) builds the snapshot with `ai_note_snapshot()` (call
at 381). It runs from `ai_send()` (908) at 950, after `g_turn_original_prompt` is set, so the
question is available there.

**Gather the ranges** (dawn_chat.c, in `note_context()`, before the `ai_note_snapshot()` call):

```c
    EmbedHit rel[8];
    int32_t rel_n = 0;
    if (app.session_path && g_turn_original_prompt
        && embed_relevant(app.session_path, g_turn_original_prompt, strlen(g_turn_original_prompt),
               rel, 8, &rel_n) != EMBED_READY)
        rel_n = 0; // not embedded yet: this turn falls back to outline + neighbours
    int32_t kept = 0;
    size_t doc_len = gap_len(&app.text);
    for (int32_t i = 0; i < rel_n; i++) {
        if ((size_t)rel[i].start + rel[i].len > doc_len)
            continue;
        char* slice = gap_substr(&app.text, rel[i].start, rel[i].start + rel[i].len);
        if (slice && embed_hit_matches(&rel[i], slice, rel[i].len))
            rel[kept++] = rel[i]; // still the text that was indexed
        free(slice);
    }
    rel_n = kept; // best first
```

**Use them in the builder:** `ai_note_snapshot()` (dawn_ai_tokens.c:149) fills its budget in this
order: selection, section, outline, neighbours. Add a step between the outline and the
neighbours. It takes the ranges in the given order and adds each one that is not already inside
the section (or the selection), until the budget runs out. The smallest change is two new
parameters, `const EmbedHit* relevant, int32_t relevant_count`. `NULL, 0` keeps today's
behaviour. Each range goes through the builder's existing `slice_within_budget(gb, start, end,
&budget, &cut)`, followed by a line such as `(relevant passage under "<heading>")`. Set
`info->has_relevant` (new field) so the explainer can say "You see the outline, the section "X"
and passages relevant to the question".

**Warm the query while the user types** (optional, but it makes the first turn hit): in the chat
input key handler, after any edit to `app.ai_input` (dawn.c:4747-4800):

```c
    EmbedHit warm[1];
    int32_t warm_n;
    if (app.session_path)
        embed_relevant(app.session_path, app.ai_input, (size_t)app.ai_input_len, warm, 1, &warm_n);
```

The query is sent once typing pauses for 250 ms, only the newest text goes out, and the vector is
cached. When Enter reaches `note_context()`, the same text is usually `EMBED_READY` already.

## Notes

- Byte ranges are offsets into the note body: the text without frontmatter, with LF line endings.
  That is what `app.text` holds once a note is loaded (`load_content()`, dawn_file.c:372). If a
  note's frontmatter fails to parse as YAML, dawn keeps it in the text while the index skips it,
  so the ranges shift. `embed_hit_matches()` catches this, and such hits are dropped.
- Index files: `$XDG_CACHE_HOME/dawn/embed/<16 hex of path hash>.idx`. They are safe to delete.
  A file that is corrupt, truncated or from another version is removed and rebuilt. A change of
  model, `_revision` or dimensions rebuilds every note.
- Pacing: batches of up to 8 pieces, at least 1 s apart. Indexing pauses while
  `runtime.activeGeneration` is true and waits out Retry-After on 429/503. When no embedder is
  listed, the worker checks `/v1/models` again every 5 minutes and does nothing else.
