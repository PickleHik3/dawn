# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A fork of [andrewmd5/dawn](https://github.com/andrewmd5/dawn), a C23 terminal markdown drafter, adapted for
Termux Launcher on Android. Branch `tl` is the fork and the default; `origin/main` tracks upstream. The fork's
changes ride on top of upstream as ordinary commits (clipboard over OSC 52, an OpenAI-compatible AI bridge
to the launcher's TAI, touch, Material You theming, notices, voice, the meaning index).

The shipping binary is cross-compiled for aarch64 by `../tlstore/recipes/cross/build-dawn.sh`, which pins a
commit of this repo and greps the stripped binary for marker strings (`]52;c;?`, `/chat/completions`,
`replace_selection`, `<append_to_note>`, no `xclip`). Removing or renaming any of those breaks the store build.
When a change here should ship, bump `DAWN_COMMIT` in that script.

## Build and test

CMake 3.16+, a C23 compiler (GCC 13+/Clang 16+), libcurl. Submodules must be checked out
(`git submodule update --init --recursive`). The root `Makefile` wraps cmake:

```sh
make                 # RelWithDebInfo into build/
make with-ai         # Release with -DUSE_LIBAI=ON: the chat, read-aloud and meaning index are compiled in
make debug           # Debug; add -DENABLE_ASAN=ON -DENABLE_UBSAN=ON by calling cmake directly
make web             # Emscripten build into build-web/ (requires emcmake)
```

Direct form, which is what CI and the store recipe use:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DUSE_LIBAI=ON
cmake --build build -j
```

Warnings are errors (`-Wall -Wextra -Wpedantic -Werror`). `USE_LIBAI` is OFF by default; most fork code
(`dawn_chat`, `dawn_session`, `dawn_speak`, `dawn_embed`, `search.c`) only compiles into the binary with it ON,
so build with it ON before claiming a change compiles. Source files are globbed (`src/dawn*.c`,
`src/highlight/lang/*.c`), so re-run cmake after adding a file.

Tests are two plain executables, no framework, no ctest:

```sh
./build/test-block                                 # block + inline parsing; exits non-zero on failure
./build/test-block tests/commonmark_spec.json -v   # CommonMark spec pass rate (about 92%); a report, exit is always 0
./build/test-embed                                 # chunker, index file, cosine top-k (pure half only); exits non-zero on failure
```

`test-block` links `dawn_block`, `dawn_gap`, `dawn_md`, `dawn_wrap` alone and stubs the theme/image/tex
getters at the top of `tests/test_block.c`. When `dawn_md.c` starts calling a new palette getter, add a stub
there or the test stops linking. `test-embed` links only `dawn_embed_index.c`; keep that file free of
globals, backend calls, threads and network so it stays that way.

Format with `.clang-format` (WebKit base, 4 spaces, `char* p`, no column limit, function brace on its own line).

To drive the built binary like a user without a phone, run it in a tmux pty under a scratch HOME
with `tests/ux/mock_tai.py` standing in for TAI (point the scratch `~/.launcherctl/endpoint` at it);
`tests/ux/ux.sh` sends keys, raw escape sequences (dictation marks, theme reports) and captures the
screen. The header of `ux.sh` has the recipe; `tests/ux/phone/README.md` has the adb equivalent for
the real device. Known typing quirk inherited from upstream: `-` at a line start auto-inserts a
space, so a typed `- [ ]` becomes `-  [ ]` and is not a checkbox. The open findings from the
2026-10-06 run are in `docs/fixes-2026-10-06.md`.

## Architecture

### Engine, backend vtable, frontends

```
main_term.c / main_web.c          frontends: pick a backend, parse args, run the loop
        │  dawn_app.h             engine API: dawn_engine_init, dawn_frame, dawn_load_document, ...
        ▼
dawn.c (7k lines) + dawn_*.c      engine: editing, parse, layout, render, panels, AI, voice
        │  DAWN_BACKEND(app)->fn  src/dawn_backend.h: struct DawnBackend of function pointers
        ▼
dawn_backend_posix.c | dawn_backend_win32.c | dawn_backend_web.c
```

The platform layer is the `DawnBackend` vtable in `src/dawn_backend.h` (display, input, clipboard,
filesystem, time, kitty images, `take_dictation`). Each backend is one const instance. The engine never
includes a backend header; it calls through `DAWN_BACKEND(app)` and tests `DawnCap` bits with
`dawn_ctx_has`. Key codes (`DawnKey`) sit above the Unicode range; fork keys include the theme reports,
mouse release/drag and `DAWN_KEY_DICTATION`.

`main_term.c` runs `while (dawn_frame()) { input_ready(...); poll_jobs(); }`. `dawn_frame()` in `dawn.c`
does, in order: quit/resize check, timer and 5-second autosave, `ai_pump()` + `ai_tick()`, `voice_tick()`,
`embed_poll()`, `handle_input()`, `render()`.

### State

One global `App app` (`src/dawn_types.h`) holds the gap buffer, cursor/selection, scroll, mode stack,
theme, history, frontmatter, chat fields, undo stack and dirty flags. Subsystems with heavy state hang off
it as opaque `void*` handles (`block_cache`, `hl_ctx`, `toc_state`, `search_state`). Only dawn's thread
touches `app`; AI, speak and embed workers hand results back through queues, atomics and generation
counters, and are never joined.

### Render pipeline

Gap buffer → `block_cache_parse()` (`dawn_block.c`, using the `md_check_*` matchers in `dawn_md.c`) →
`Block` array with byte offsets, wrapped virtual rows (`dawn_wrap.c`) and `InlineRun`s → `render_writing()`
→ `render_block()` per block → `dawn_theme.h` primitives → backend. The cache re-parses only when text
length, wrap width or height changes. Headings use kitty text sizing (`write_scaled`), images and the
scroll pill use kitty graphics. `dawn_render.c` draws every non-writing screen and modal overlay.

### Modes, keys, panels

`AppMode` is an enum in `dawn_types.h` with `MODE_PUSH`/`MODE_POP`. `handle_input()` in `dawn.c` reads a
key, runs `notice_ack`, the theme-report keys and `voice_handle_key`, then switches on `app.mode`. Ctrl
keys are raw integers in nested `switch` statements (`case 12: // Ctrl+L`); there is no binding table.
A new Ctrl key goes in `handle_writing` (and `handle_ai_input` if it should work with the chat focused),
guarded by `CAN_EDIT()`/`CAN_MODIFY()`, and into `render_help()` in `dawn_render.c`. A new modal needs an
`AppMode` value, a `render()` case (usually `render_writing()` then the overlay) and a `handle_input()`
case. The AI panel is not a mode: it is `app.ai_open`/`ai_focused`, drawn as a side panel or, at phone
width, a bottom sheet.

### AI: libai and TAI

`libai/` is upstream 6over3/libai (Apple FoundationModels, macOS only) plus the fork's
`ai_bridge_openai.c`, which implements the same `ai_bridge.h` over OpenAI-compatible Chat Completions with
libcurl and pthreads. On non-Apple hosts with `USE_LIBAI`, CMake builds `libai_openai` from `ai.c`,
`ai_bridge_openai.c`, `ai_speak.c`, `ai_embed.c` and defines `DAWN_HAS_SPEAK`. `DAWN_EMBED_LIVE`
(`dawn_embed.h`) is true only there; elsewhere every `embed_*` call is a no-op stub, so UI hooks need no
`#if`.

Endpoint resolution (`config_load` in `ai_bridge_openai.c`), re-read on every request, no env vars:
`<XDG_CONFIG_HOME>/dawn/ai.json` with `{"provider":"tai"}` (the default when absent) or
`{"provider":"openai","base_url":...,"api_key":...,"model":...}`. In TAI mode the base URL is the first
line of `~/.launcherctl/endpoint` plus `/v1` and the bearer token is `~/.launcherctl/token`, both written
by Termux Launcher. The model chosen in the chat header's picker lives in `state.json` beside `ai.json`;
a model named in `ai.json` wins. Endpoints used: `/v1/chat/completions`, `/v1/models`,
`/v1/ai/runtime`, `/v1/ai/runtime/cancel`, `/v1/ai/runtime/keep-warm`, `/v1/ai/speak`,
`/v1/ai/speak/stop`, `/v1/embeddings`, `/v1/tokenize`.

Tools are registered in `dawn_chat.c` (read_document, web_search, get_time, past_sessions, replace_note,
replace_selection, insert_at_cursor, append_to_note). TAI drops tools for most on-device models, so the
note rides on every question (`dawn_ai_tokens.c` snapshot) and a plain reply can edit through tagged
blocks like `<append_to_note>`. `dawn_session.c` keeps one long-lived conversation per note so TAI's KV
cache is reused. `dawn_ai_queue.c` runs one job at a time in two lanes (USER, QUIET); quiet jobs
(titles, snapshots) must never edit the note. AI edits are single undo steps.

### Voice and meaning index (fork)

`dawn_voice.c` is the facade `dawn.c` calls. Behind it, `dawn_speak.c` reads aloud a sentence at a time
over `/v1/ai/speak` (Ctrl+Q; any other key stops it) and `dawn_dictate.c` applies dictation phrases
arriving as OSC 7727 marks, parsed in `dawn_backend_posix.c`. `DAWN_REDUCED_MOTION` disables the glow.

`dawn_embed_index.c` is the pure half (markdown-aware chunker, `DAWNEMBD` index file, cosine top-k);
`dawn_embed.c` is the worker that discovers an embedder via `/v1/models`, indexes every note in the notes
directory plus history paths, and writes one `<hash>.idx` per note under `$XDG_CACHE_HOME/dawn/embed`.
UI rule: show nothing unless `embed_ready()` returned true and the query returned hits.

### Launcher integration points

- Material You palette from `~/.termux/material-colors-{dark,light}.properties` (`dawn_theme.c`), with
  light/dark switches arriving as CSI ?997 reports after `?2031h`.
- Clipboard is OSC 52 under `__ANDROID__`; the xclip/xsel path must stay compiled out there.
- OSC 8 hyperlinks are gated on `DAWN_CAP_HYPERLINKS`.
- Notes live in `$XDG_DATA_HOME/dawn` (fallback `~/.dawn`); settings in `<config>/dawn/settings.json`.

### Syntax highlighting

`src/highlight/` is a C port of speed-highlight-js on PCRE2. A language is one `lang/<x>.c` with a static
rule table and a `hl_lang_<x>()` accessor; declare it in `highlight.h` and register it in
`hl_ctx_new_with_defaults()` in `highlight.c`.

## Conventions

- Module prefix equals file name (`gap_*`, `block_*`, `embed_*`, `voice_*`, `notice_*`). Types are
  PascalCase, enum values UPPER_SNAKE, fixed-width integers everywhere (`int32_t main(...)`).
- Each file opens with `// file.c - purpose`; doc comments are `//!`; regions use `// #region`.
  Fork files carry a prose explanation at the top; keep it current when behaviour changes.
- Errors are bool returns or NULL; the engine never aborts. User-visible failures go through
  `notice_post` (an error notice is sticky until a key or tap).
- A function returning `char*` hands ownership to the caller; libai strings are freed with
  `ai_free_string`.
- `DAWN_ENUM(type)` (`dawn_support.h`) gives typed enums on C23 compilers; MSVC builds as C17 via
  `dawn_compat.c`.
- Commit messages follow `type(scope): what changed`, written as a sentence
  (`fix(dawn): a long notice is cut with an ellipsis instead of never showing`).

`docs/p2-voice-integration.md` and `docs/p4-embed-integration.md` describe how voice and the meaning index
were hooked into `dawn.c`. Both are merged; their line numbers refer to an older commit.
