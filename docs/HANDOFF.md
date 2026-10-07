# Handoff

A rolling note for the next agent. Read it before starting; before ending a session, update it:
move finished items out, add what you left open, and date your entry. Newest session first. Keep
it short: a line per item, with file:line or the command that shows it.

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
1. **Phone check.**
   - Does closing a Termux session send SIGHUP to dawn, or SIGKILL? If it is SIGKILL, up to 5 s of
     typing is still lost and the only fix is a shorter autosave or saving on every pause.
     Recipe: `tests/ux/phone/README.md`.
   - Also check: taps on the conflict dialog rows, and auto-reload when Syncthing changes a note.
2. **Ship it.** After the phone check, bump `DAWN_COMMIT` in `../tlstore/recipes/cross/build-dawn.sh`.
   The marker strings were checked on a host build: `/chat/completions`, `replace_selection` and
   `<append_to_note>` are present. `]52;c;?` (and the absence of `xclip`) only hold in an
   `__ANDROID__` build, so the cross build has to confirm those.
3. **Branches.** Another session works in the main checkout on `embed-gemma2`, which already
   contains `35fa80c`. Merging it into `tl` should be clean; check that `dawn_embed*.c` merges
   cleanly, since both lines of work touched it.
4. **Windows and web have not been compiled since these changes** (no mingw or emcc here).
   `win32_write_file` (`src/dawn_backend_win32.c`, about line 1421) still writes in place with
   `fopen("wb")` and an unchecked fclose, so it is not atomic. A proper fix is a temp file,
   `FlushFileBuffers`, then `MoveFileExW(MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)`.
5. **Crash leftovers are never swept.** A crash in the middle of a save leaves hidden
   `.<name>.tmp-<random>` files beside the note, and an interrupted image download leaves the same
   in the image cache. Consider sweeping ones older than a day at startup.
6. **No restore UI for `versions/`.** The user chose snapshots without a picker. Copies are found
   by path only: the AI-edit notice, or the folder itself.
7. Ctrl+Z after "reload theirs" restores the text but not the frontmatter, because the undo stack
   holds text only.
8. Snapshots are silent: notices have no activity-list-only level.
9. Two dawns on one note: there is no lock or swap-file warning. The conflict check catches the
   clash at the first save after it happens.
10. `fsio_move_no_replace` falls back to lstat+rename where hard links are not allowed (Android
    shared storage), which leaves a tiny race. The rename-based save also breaks hard links, as in
    most editors.
11. There is no automated test of the save/conflict flow; it was checked by hand in tmux. A
    scripted `tests/ux` check would guard it. Recipe used: start dawn on a scratch note under a
    scratch HOME and XDG dirs, type, rewrite the file from outside, wait 6 s, capture the screen.
    For reload, write the file while the note is clean and wait 3 s. For SIGHUP, type, then
    `kill -HUP` and read the file.
12. Older open findings are in `docs/fixes-2026-10-06.md`.
