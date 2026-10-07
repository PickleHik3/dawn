# Handoff

A rolling note for the next agent. Read it before starting; before ending a session, update it:
move finished items out, add what you left open, and date your entry. Newest session first. Keep
it short: a line per item, with file:line or the command that shows it.

## 2026-10-07 · meaning index UI, EmbeddingGemma 2, first device pass

On `embed-gemma2` (27 commits over `tl`, not merged into `tl`; it already contains all of `tl`):
- UI: Ctrl+S "by meaning" covers every note, rows are tappable, the count includes them and the
  index's status sits under the list; the chat's "also in:" is tappable; a permanent indexer
  failure posts one sticky notice; help has a third page "meaning index" with `m` (switch, saved as
  `meaning_index` in settings.json) and `r` twice (rebuild); `src/dawn_status.c` is a panel that
  slides out bottom-right (welcome screen and notes) with model loading, live title, answer,
  read-aloud, mic and indexing x/y.
- Thresholds follow the embedder (`embed_floors()` in `src/dawn_embed_index.c`): measured on the
  phone at 256 dims, EmbeddingGemma 2 scores unrelated text 0.55-0.65 (v1: 0.07-0.25), so v2 uses
  search 0.70 / related 0.80, v1 and others 0.35 / 0.60, plus a 0.10 cut below the best hit.
- `dawn_engine_shutdown()` saves before stopping the indexer: the launcher's ShellTerminator sends
  SIGHUP and SIGKILL 150 ms later, and `embed_shutdown()` may wait a second.
- A long Ctrl+S query no longer runs into the match count.
- Launcher: `embed-gemma2` (LiteRT-LM 0.18.0 `EmbeddingEngine` runtime, catalogue, tier policy) is
  merged into the local `dev` checkout as `adec12b8f`, not pushed.

Checked on the phone (A065, cross build of this branch, launcher `embed-gemma2`):
- SIGHUP save inside the 150 ms window, conflict dialog taps, auto-reload after an outside rename,
  help page 2 at phone width; cross-build markers `]52;c;?`, no `xclip`, AI strings, RUNPATH.
- The 440M `.litertlm` loads (about 6 s cold), honours `dimensions` 256, normalises, and the file
  carries no task-prefix template, so the launcher's prefixes are not doubled.

Second device pass, done (launcher `dev` c8e453df4 = adec12b8f + the Android 16 display fix):
- 440M indexes 20 notes in about a minute; Ctrl+S hits the right note for tomatoes, sourdough,
  kubernetes, "books I want to read" (a real note) and "settings screen cleanup", and returns
  nothing for an unrelated query. Status panel bottom-right, help page 3 switch (saved) and `r r`
  rebuild all work.
- Fixed here (b8fe373): TAI's /v1/models takes ~12-16 s on the phone, past the indexer's 8 s
  discovery timeout, so indexing never started; now 30 s. A meaning row whose H1 equals the note
  title no longer prints the name twice.

Open:
1. TAI is slow: /v1/models ~12-16 s and, under memory pressure, /v1/ai/runtime ~15 s. Launcher
   side, unconfirmed by profiling: `TaiModelSpec.java:517` re-parses each Gemma `.litertlm`
   (multi-GB) through `ModelInfo.from` 6-10 times per request with no cache. The chat bridge's
   /models and /ai/runtime timeouts (`libai/ai_bridge_openai.c:378`, `:762`, 8 s) fail the same way.
   Rebuild ran at ~2 notes/min while TAI was in that state.
2. Ship: merge `embed-gemma2` into `tl` (ask first), then bump `DAWN_COMMIT` in
   `../tlstore/recipes/cross/build-dawn.sh`; push launcher `dev` when its owner agrees.
3. A permanent refusal (e.g. `capability_not_supported` from a launcher without the `.litertlm`
   route) stops indexing until the next discovery; dawn could fall back to the next embedder.
4. Launcher flakes, order-dependent, pass alone: `TerminalIOPreferencesDataStoreLazyModeTest`,
   `IconPackChoicesLiveApplyTest`.
5. Two sessions installing to the same phone overwrite each other's launcher builds; check
   `adb shell dumpsys package com.termux | grep lastUpdateTime` before trusting a device result.
6. clang-format is not installed; this branch was formatted by hand.

## 2026-10-07 · storage robustness (local data store)

Done and merged on `tl` (merges `35fa80c` and `87472b4`, plus the index-sync commit after them):
- `src/dawn_fsio.{h,c}`: pure storage layer. It handles atomic writes (a unique O_EXCL temp file,
  fsync, rename, then a directory fsync) and follows symlinks. It also has checked reads,
  mkdir -p at 0700, no-replace moves, setting damaged files aside, and hashing.
  `tests/test_store.c` covers it, plus the history CRDT and `dawn_notepath`.
- SIGHUP (closing the Termux session) quits cleanly and saves. The posix backend notices POLLHUP.
- Note-level code is in `src/dawn_file.c`:
  - Every save compares the file with the hash dawn last saw. A change from elsewhere writes the
    editor's text to `<stem>.conflict-<time>.md`, pauses saving and opens `MODE_CONFLICT`
    (r reload theirs / o overwrite with mine / k keep editing).
  - A note with no unsaved edits auto-reloads every 2 s (`note_watch`).
  - Per-session snapshots go into `versions/`, then one every 30 minutes; 50 are kept per note.
- `.sessions`, `settings.json` and `state.json` that fail to parse are set aside as
  `*.corrupt-<time>`; every failed write posts one notice. Files holding NUL bytes are refused.
- Meaning-index files are now synced too (file and directory). Their temp names are unique per
  process, and only temp files older than 10 minutes are swept.

Open, roughly in priority order:
1. Phone check, cross build and the branch merge: done 2026-10-07 (see the entry above).
2. Ship: see the entry above.
3. **Windows and web have not been compiled since these changes** (no mingw or emcc here).
   `win32_write_file` (`src/dawn_backend_win32.c`, about line 1421) still writes in place with
   `fopen("wb")` and an unchecked fclose, so it is not atomic. A proper fix is a temp file,
   `FlushFileBuffers`, then `MoveFileExW(MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)`.
4. **Crash leftovers are never swept.** A crash in the middle of a save leaves hidden
   `.<name>.tmp-<random>` files beside the note, and an interrupted image download leaves the same
   in the image cache. Consider sweeping ones older than a day at startup.
5. **No restore UI for `versions/`.** The user chose snapshots without a picker. Copies are found
   by path only: the AI-edit notice, or the folder itself.
6. Ctrl+Z after "reload theirs" restores the text but not the frontmatter, because the undo stack
   holds text only.
7. Snapshots are silent: notices have no activity-list-only level.
8. Two dawns on one note: there is no lock or swap-file warning. The conflict check catches the
   clash at the first save after it happens.
9. `fsio_move_no_replace` falls back to lstat+rename where hard links are not allowed (Android
    shared storage), which leaves a tiny race. The rename-based save also breaks hard links, as in
    most editors.
10. There is no automated test of the save/conflict flow; it was checked by hand in tmux. A
    scripted `tests/ux` check would guard it. Recipe used: start dawn on a scratch note under a
    scratch HOME and XDG dirs, type, rewrite the file from outside, wait 6 s, capture the screen.
    For reload, write the file while the note is clean and wait 3 s. For SIGHUP, type, then
    `kill -HUP` and read the file.
11. Older open findings are in `docs/fixes-2026-10-06.md`.
