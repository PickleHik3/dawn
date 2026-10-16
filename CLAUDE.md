# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

**Read `docs/HANDOFF.md` first.** It is the rolling list of open work left by earlier sessions; update it
before you end yours (move finished items out, add what you left open).

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

Tests are seven plain executables, no framework, no ctest:

```sh
./build/test-block                                 # block + inline parsing; exits non-zero on failure
./build/test-block tests/commonmark_spec.json -v   # CommonMark spec pass rate (about 92%); a report, exit is always 0
./build/test-embed                                 # chunker, index file, cosine top-k (pure half only); exits non-zero on failure
./build/test-store                                 # dawn_fsio, the history CRDT, dawn_notepath naming; exits non-zero on failure
./build/test-hit                                   # hit regions and the hint-line parser (dawn_hit); exits non-zero on failure
./build/test-format                                # formatting commands on the gap buffer (dawn_format); exits non-zero on failure
./build/test-commands                              # the command table, ui scale, tab row and ribbon layout at every scale, menu lines (dawn_commands); exits non-zero on failure
./build/test-tips                                  # the one-shot tips' seen set and its "tips_seen" JSON (dawn_tips); exits non-zero on failure
```

`test-block` links `dawn_block`, `dawn_gap`, `dawn_md`, `dawn_wrap` alone and stubs the theme/image/tex
getters at the top of `tests/test_block.c`. When `dawn_md.c` starts calling a new palette getter, add a stub
there or the test stops linking. `test-embed` links only `dawn_embed_index.c`; keep that file free of
globals, backend calls, threads and network so it stays that way. `test-store` (not on Windows) links
`dawn_fsio.c`, `dawn_crdt.c`, `dawn_notepath.c` and cJSON with stubs; keep those three pure too.
`test-hit`, `test-format` (with `dawn_gap.c`), `test-commands` and `test-tips` (with cJSON) each link
their module alone.

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
guarded by `CAN_EDIT()`/`CAN_MODIFY()`, and into the command table below; help lists it from there. A new modal needs an
`AppMode` value, a `render()` case (usually `render_writing()` then the overlay) and a `handle_input()`
case. Touch: anything tappable registers a hit region while it renders (`hit_add` in `src/dawn_hit.c`,
cleared at the top of `render()`), and `handle_input` turns a tap on it into the key it stands for before
the mode's switch, so a tap and its key share one path; a tap on no region reaches the mode as
`DAWN_KEY_MOUSE_CLICK`.

`src/dawn_commands.c` is the table of the writing screen's commands (label, group, the key
`handle_writing` takes, the shortcut as printed, what each needs, the ribbon button's caption, its
text glyph and its Nerd Font icon). Where `chrome_place()` in `dawn.c` says so (`app.launcher_term` or
`app.cols < AI_PANEL_MIN_COLS`, not in preview or print; a wide desktop terminal gets none), a tab row
runs across the top as a word processor has it (`render_tabs`: `File  Formatting  AI`, AI only with
`HAS_LIBAI`, on a tinted band, the open tab on its ribbon's surface; the word count, the timer and
"not saved" at its right end when there is room, `tabs_status`). A tap on a tab is a `DAWN_KEY_TAB_*`
key: its ribbon drops open under the row (`ribbon_toggle`, row by row over `RIBBON_DROP_MS`, at once
with `DAWN_REDUCED_MOTION`), a tap on the open tab or a typed esc folds it away, and a chat sheet and a
ribbon never share the screen. A ribbon (`render_ribbon`) is sections (`ribbon_buttons`, the
`SECTIONS` table): File = details, pause, close | undo, redo, cut, copy, paste | find, outline, focus,
light or dark (`DAWN_KEY_CMD_THEME`); Formatting = `CMD_GROUP_FORMAT` (one `DAWN_KEY_FMT_*` per
`FormatOp`, one `format_apply` and one undo step; a style `format_state` says is on draws pressed),
then help; AI = chat and the model button (glyph: the model's short name when it fits, else a chip;
caption: loaded, loading or not loaded; a tap opens the chat's model picker, `DAWN_KEY_AI_MODEL`),
then undo, title (`DAWN_KEY_AI_TITLE`), auto format (`DAWN_KEY_AI_FORMAT`; both are `ai_action` in
`dawn_chat.c`, USER-lane chat turns whose edit is a tagged block) and read aloud. Every command is on
a ribbon or in the full menu (`ribbon_tab_of`, `commands_in_menu`, tested). Each button is a box with
its glyph centred over its caption, about three rows tall at scale 2, all of a ribbon's buttons one
width (its widest; captions at most 7 columns, every glyph budgeted 2) so they form a grid.
`ribbon_layout` (`ribbon_pack`) lets a section go on after the one before it on the same row, behind
a divider, wrapping what does not fit to the left edge, or start a row of its own under a thin rule,
its buttons spread evenly; it takes the way with the fewest rows, then the fewest split sections
(File at six a row: 3 | 3, then 2 | 4); then each row is spread to the ribbon's right edge
(`ribbon_spread`: its buttons widened evenly, the cells left over to its gaps), rows of one section
in a run kept one grid as the fullest of them; above scale 1 two rows of buttons are a blank row apart
(`ribbon_row_gap`, counted in the layout; a stacked section's rule is drawn in it); a ribbon pages
with a "more" button (`DAWN_KEY_RIBBON_MORE`) rather than leave the note fewer than
`RIBBON_MIN_NOTE_ROWS`. The text cursor stays hidden while a ribbon drops (`Layout.ribbon_dropping`).
Icons are Nerd Font code points, used only when `app.nerd_font` (on in the launcher when a file under
`~/.termux/fonts/` names "nerd", `nerd_font_installed`; settings.json's `"nerd_font"` wins); otherwise
each button has a text glyph. Above scale 1 an icon goes out as one OSC 66 block of the icon and a
space, `s=<scale>:w=2` (`print_scaled_block`, `FACE_BLOCK`), 2×scale columns: the launcher draws a
Nerd icon full size only so, and sized alone it comes out half height. It is drawn at 3/4 of the
block (`:n=3:d=4:v=2`, `RIBBON_ICON_NUM`/`DEN`) so it keeps a margin in its box, with `h=2`
(centred in the block), or `h=0` with the block shifted a cell where the box is an odd count of
cells wider than the block, so the 3-cell icon still centres. Ctrl+] (and "all commands", the "menu" button in Formatting's help
section) opens the full grouped command menu (`MODE_MENU`, `menu_build`, drawn by `render_menu` in
`dawn_render.c`), every command but the format group's. Help (`MODE_HELP`, Ctrl+O) reads the same
table: its shortcut pages list every command by group with its key and its ribbon ("File ribbon", or
"menu" for one on none), from `help_rows`/`help_paginate`, after a
hand-written "move and select" group; a new command goes in that table and nowhere else. Help's pages
are `HelpPage` (`dawn_types.h`: shortcuts, Markdown cheatsheet, activity, meaning index), each over as
many screens as the screen's height needs (`app.help_screen`, `render_help_screens`); tab and <- turn
through them via `render_help_neighbour`. Running a command from the menu presses its key (through
`pending_key`), so it behaves as the shortcut does; a key from a ribbon button or the menu is
`key_is_command` (so "close" closes the note while a typed esc only folds the ribbon). Commands with no
chord are the virtual keys `DAWN_KEY_CMD_*` and `DAWN_KEY_AI_*` in `dawn_backend.h`. Size:
`Layout.ui_scale` scales the tab row, the ribbons and the command menu. Where the terminal has kitty
text sizing (`DAWN_CAP_TEXT_SIZING`) it comes from the pane's pixels (`ui_scale_now`,
`commands_ui_scale`: a chrome row is about 1/26 of the pane's width (`UI_SCALE_DIVISOR`), clamped to
1..4, 1 without a pixel size), lowered until the three tabs fit (`tabs_scale`), so the chrome keeps its
size on screen when a pinch changes the font while the note follows the font; without text sizing
(tmux, xterm) it is 1. A button's caption is at half the scale (`ribbon_caption_scale`). Under the tab
row there is no status line: the bottom row is the notice line (`render_notice_line`: a notice or a
one-shot tip while one is up, else the status panel's corner); a desktop terminal keeps
`render_status_bar`. Layout: tab row, ribbon, a blank row, the text, the notice line; all scaled places,
in screen cells, come from the pure functions in `dawn_commands.c` (tested in `test-commands`), and tap
regions are as tall as what they cover. The AI panel is not a mode: it is `app.ai_open`/`ai_focused`, drawn as a side
panel or, at phone width, a bottom sheet.

Zone rule under the tab row: nothing is ever drawn over the note. The writing screen is the chrome
(the tab row, then the open ribbon or a popup's band), the note, and one bottom row always kept for
the notice line (notices, tips, the status panel's corner; with a chat sheet it is the row between
the note and the sheet), so a notice coming or going moves nothing. A popup over the note
(`popup_over_note`: the menu, search, contents, the conflict dialog, the frontmatter and image
editors) takes a band under the tab row in the ribbon's place (`Layout.popup_rows` from
`render_popup_rows`, drawn through `render_popup_band`), and the note re-wraps under it; only where
it does not fit is it centred as on a desktop. The chat sheet takes its rows from the note. The
chrome and the note are each a frame part (`frame_part_begin`/`end` in the backend, `PART_CHROME`,
`PART_NOTE` in `dawn.c`): each paints only its own rows, the chrome first, and a part unchanged since
it was last sent is left out of the frame, so a fading notice repaints only its line. Anything new
drawn on the writing screen goes in one of the zones; drawing over another zone's rows from outside
its part leaves stale cells when that part is skipped.

First run: until settings.json holds `"onboarded": true`, `dawn_engine_init` opens on `MODE_TOUR` (cards
in `TOUR_CARDS`, drawn by `render_tour` in `dawn_render.c`, keys in `tour_key` in `dawn.c`; enter and the
arrows or a swipe turn cards, esc or enter on the last one sets `onboarded` and goes to the start screen).
"welcome tour" in the help group of the command table (`DAWN_KEY_CMD_TOUR`) pushes it again, and then it
pops back. Two one-shot tips, the first selection and the first save, go through `tip_show` in
`dawn.c` as a `NOTICE_TIP`, which `render_status_bar` (or, under the tab row, `render_notice_line`) draws alone across the whole line; a tip
counts as given only once drawn, never off the writing screen. `src/dawn_tips.c` is the pure set of
tips given (`TipId`, `tips_seen`, `tips_mark`), saved by name as `"tips_seen"` in settings.json.

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
- Notes live in `$XDG_DATA_HOME/dawn` (fallback `~/.dawn`); settings in `<config>/dawn/settings.json`
  (`theme`, `timer_mins`, `nerd_font`, `meaning_index`, `onboarded`, `tips_seen`; a save keeps
  keys it does not know, and tip names in `tips_seen` it does not know).
- Storage: every write goes through `dawn_fsio` (temp file, fsync, rename, then a directory fsync).
  `save_session` in `dawn_file.c` compares the file with what dawn last saw and never overwrites
  a change made elsewhere: it writes a `.conflict-<time>.md` copy and opens `MODE_CONFLICT`.
  Files that fail to parse are set aside as `*.corrupt-<time>`, never overwritten.

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
