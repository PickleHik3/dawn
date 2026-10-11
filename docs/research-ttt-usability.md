# What dawn can borrow from ttt to be easier for new users

Research note, 2026-10-11. Sources: ttt at commit `b5905d2` (MIT, github.com/eugenioenko/ttt), read
but not built or run, and this worktree at `f611283`. ttt paths are relative to the ttt repo root;
dawn paths are relative to this repo. Line numbers are from those two commits.

## Summary

- **ttt is not a touch or phone editor.** Its code and docs never mention touch, phones, Termux or
  Android. It has no width breakpoints, and its own audit records that it breaks at 50 columns and
  below (`audit/2026-07-12-ux-bug-audit.md:460-498`). Its good "layout, scaling and touch"
  impression comes from three things:
  1. Every feature can be reached by clicking something visible: the menubar, `⋮` buttons, the
     welcome rows and the status-bar segments.
  2. One command registry feeds the menus, the command palette and the help, so the shortcut
     shown is always the live one.
  3. Some pieces degrade gracefully: tab overflow, the welcome layout and the dividers.
- **Dawn already handles touch at a lower level than ttt.** It has double and triple tap, drag
  selection, swipes routed per pane, checkbox taps and a bottom-sheet chat. What it lacks is
  everything in the first two bullets above:
  - The welcome screen cannot be tapped.
  - Help is a fixed 26-row box. It is taller than the phone's 24-row pane, and nothing on the
    writing screen tells you how to open it.
  - There is no formatting command at all; formatting means typing Markdown.
  - There is no list of features you can browse.
- **The biggest win is one action table** (ttt's `Command` registry) feeding three things: a
  command palette (`MODE_PALETTE`), a tappable formatting bar and generated help.
- **Then add the onramps:** a tappable welcome screen, a sample/tour note, one-time hints and
  slash commands. Then the phone-width layout fixes.
- **Known quirk, now fixed:** the `- [ ]` → `-  [ ]` quirk that CLAUDE.md still lists was fixed in
  `0dd46a0`. See `src/dawn.c:375-381` and `docs/fixes-2026-10-06.md:134`. CLAUDE.md is stale on
  this point.

## 1. What ttt actually does (verified in source)

### 1.1 Layout and scaling

- **The layout is a fixed widget tree:** menubar, then sidebar|editor split, then status bar
  (`internal/app/widgets.go:354`). The bottom panel sits under the editor (`:268-272`).
  `Root` only hands the full rect down (`internal/ui/root.go:69`).
- **No breakpoints.** The menubar never collapses. Items are laid out left to right and drawing
  simply stops at the edge (`internal/ui/menubar_widget.go:65-87`).
  - It can only be hidden by a command or setting (`internal/app/menubar.go:12-26`).
  - When it is hidden, a status message tells you how to bring it back (`:30-38`).
- **Tab strips overflow Chrome-style.** The active tab is always kept visible, and the rest go
  behind a 3-column `»` button that opens a list of the hidden tabs
  (`internal/widgets/tabs.go:123-145, 226-236`; `internal/app/callbacks.go:590-600`). This is
  ttt's only real responsive pattern.
- **The editor tab bar has a `⋮` "more" button** (`internal/ui/more_button.go:21-25`). Its
  `◀ ▶` scroll zones are 3 columns wide (`internal/ui/tabbar_widget.go:166-178`).
- **The welcome page is the one adaptive layout** (`internal/app/welcome_view.go:68-91`).
  - It tries three title sizes (block art, smaller art, plain text).
  - It drops spacing before it drops content, and shows the hint row only if 2 spare rows remain.
- **Dividers:**
  - A 2-cell grab zone, where a press with no movement is replayed to the editor as a click
    (`internal/ui/split_panel.go:228-301, 373-383`).
  - Dragging a pane to zero collapses it, and dragging from the edge reopens it
    (`internal/app/app.go:229-238, 310-312`; `internal/ui/content_split.go:351-355`).
  - A one-key fullscreen for the terminal pane (`internal/app/commands_view.go:20-31`).
- **Failures at small sizes** (ttt's own audit, BUG-036..040, still open per `audit/critical.md:138-140`):
  - The status bar is invisible at 50 columns or fewer.
  - The sidebar can crush the editor to 1 column.
  - Menus clip instead of scrolling.
  - The status bar drops its whole right-hand group when it doesn't fit
    (`internal/ui/statusbar_widget.go:86-104`).

### 1.2 Mouse (there is no touch-specific code)

- **Modes:** tcell `EnableMouse()` (`internal/term/tcell_screen.go:39`) turns on ?1000h, ?1002h,
  ?1003h (any-motion, for hover) and ?1006h SGR. Checked in tcell v3.4.1 `tscreen.go:1069-1135`.
  Bracketed paste is on too. OSC 22 sets the pointer shape over dividers
  (`tcell_screen.go:164-178`).
- **Pointer capture:** a widget that returns `EventCaptured` gets every event until release
  (`internal/ui/root.go:279-317`). Right-click is routed at the root to context menus
  (`root.go:302-307`; `internal/app/menus.go:312-362`).
- **Double/triple click:** 500 ms window, same cell (`internal/ui/widget.go:21`;
  `internal/ui/editor_widget_mouse.go:98-121`).
- **Wheel:** the wheel over the tab bar switches tabs (`tabbar_widget.go:349-356`).
- **Scrollbars:** draggable, and a click on the track centres the thumb
  (`internal/ui/scrollbar.go:53-88`).
- **Single-press activation:**
  - Palette rows (`internal/ui/selectdialog_widget.go:346-367`) and autocomplete rows run on press.
  - Welcome rows run on press (`welcome_view.go:310-325`).
  - Status-bar segments with an `OnClick` run on press (`statusbar_widget.go:153-194`): the
    indent and EOL pickers and the language → output panel.
- **Touch pitfalls, found in the code (not run):**
  - Context-menu items fire on press, not on release (`internal/ui/contextmenu_widget.go:272-289`).
  - The tab `✕` is one cell and needs the release on exactly the pressed cell
    (`tabbar_widget.go:97-102, 374`).
  - The Explorer's file operations need a right-click (`internal/app/plugins_panel.go:65-66` is
    the only tree with a per-row `⋮`).
  - No long-press anywhere.

### 1.3 Discoverability

- **Menubar:** File / Edit / Selection / View / Options / Help (`internal/app/widgets.go:277-284`;
  `internal/app/menus.go:16-80`).
  - Menus open on a mouse press, and hovering another label switches menus (`menus.go:289-307`).
  - Each item shows its live shortcut: `resolveShortcuts` copies it from the registry
    (`menus.go:192-206`).
  - Options is generated as checkbox toggles (`internal/app/commands_options.go:327+`).
- **Command registry:** `Command{ID, Title, Shortcut, Keywords, Handler, Hidden}`
  (`internal/command/registry.go:5-12`).
  - The category is just a title prefix such as "View:" or "Git:".
  - Shortcuts are re-applied from `config/keybindings.json` (VS Code format, chords)
    (`internal/app/commands.go:30-60`).
- **Palette:**
  - Prefix modes: `>` commands, `:` go to line, `?` help, anything else files
    (`internal/ui/selectdialog_widget.go:468-487`). It opens pre-filled with `>` (`:91`).
  - Each row shows its shortcut, right-aligned and dimmed (`:250-282`).
  - Fuzzy scoring favours substrings, word starts and consecutive runs
    (`internal/ui/fuzzy.go:9-59`).
  - Width is 60% of the screen, clamped to 40..60 columns and to the screen (`:26-27, 109-114`).
- **`?` help topics:** "a deliberately curated new-user onramp" (`internal/ui/palette_help.go:35-80`).
  Six hand-written topics each pull in commands by ID prefix, so the shortcuts listed stay live.
- **Per-panel help** dialogs (`internal/app/commands_help.go:9-70`).
- **A shortcut editor** with record mode (`internal/ui/keybindings_widget.go`).
- **A key tester** that shows what a key decodes to (`internal/ui/keytester_widget.go`).
- **Welcome page** (`internal/app/welcome.go:22-42`):
  - Six actions, each showing its shortcut, then favourites and recent folders.
  - All rows are clickable, plus a hint row `↑↓ select   enter open`.
- **Settings form:** tabs, checkboxes, selects and inputs, applied with Apply/Cancel instead of
  editing JSON (`internal/app/settings_view.go:62-240, 347-349`). Its fixed 30+34-column rows
  (`:16-17`) would not fit a phone.

### 1.4 Formatting

- **ttt has no Markdown formatting affordances.**
  - No bold/heading commands and no toolbar.
  - Toggle-comment falls back to `//` for Markdown (`internal/ui/editor_widget_lines.go:335-349`).
  - Preview is a community plugin (`community-plugins.json:73-80`).
  - The built-in Markdown renderer is used only for hovers and plugin READMEs
    (`internal/markdown/markdown.go:112-122`).
- **Everything dawn needs for formatting is therefore dawn's own design**, not a ttt borrowing.

## 2. Dawn today

### 2.1 How a new user learns what exists

- **Welcome screen** (`src/dawn_render.c:138-236`):
  - Logo, tagline, and a 2×3 grid of key hints: enter write, h history, t timer, d theme,
    q quit, ? help.
  - The handler takes keys only (`src/dawn.c:5509-5537`). **None of it is tappable.**
  - On a phone the user must summon the keyboard just to start a note.
- **Help:**
  - It is `Ctrl+O` inside a note (`src/dawn.c:4778`, `case 15`) but `?` on the welcome screen.
    Nothing in the writing screen says so.
  - The status line's right-hand hints are only `^P · ^/ · esc` (`src/dawn.c:3217-3240`).
  - The help page is a hand-written list in `render_help()` (`src/dawn_render.c:528-654`).
    - It is fixed at `height = 26`, and `render_popup_box()` clamps the top but not the height
      (`src/dawn_render.c:79-86`).
    - The reference phone pane is 53×24 (`tests/ux/phone/README.md:26`), so the box runs off the
      screen even before the keyboard opens.
  - It omits several keys:
    - `^B` (cursor syntax, `src/dawn.c:4772`) and `^N` (footnote, `:4775`).
    - `^K`/`^U` (kill line) and `^A` (select all).
    - `^Q` (read aloud, handled in `dawn_voice`).
    - Tables, math, emoji shortcodes and footnote syntax.
  - It says "opt+arrows", which is a macOS term.
  - Help has three pages (`app.help_page`), and the third (meaning index) is the only one with tap
    targets (`src/dawn_render.c:516-526`).
- **README is out of step with the code** (`README.md:230-244`):
  - It lists `Ctrl+H` for help. Ctrl+H is 8, which is backspace (`src/dawn.c:5074`).
  - It swaps `Ctrl+G`/`Ctrl+E`: in the code 7 is the frontmatter/title editor and 5 is block/image
    edit.
- **There is no example note, tour, first-run state or contextual hint.** Notices (`notice_post`)
  and the status panel (`src/dawn_status.c`) report background work only.

### 2.2 How formatting happens

- **It is typed Markdown, with a few helpers:**
  - Auto-space after a list marker (`src/dawn.c:372-`).
  - List, task and quote continuation on Enter (`:5188-5246`).
  - Tab/Shift+Tab indent (`:5152-5187`).
  - Structure-aware backspace (`smart_backspace`, `:822`).
  - A newline after a typed rule or image (`check_auto_newline`, `:833`).
  - Footnote creation on `]`.
  - Emoji shortcodes and smart dashes.
- **There is no command that makes text bold, makes a heading, toggles a list or inserts a link,
  table or code block**, by key or by tap. The only structural tap is ticking a task box
  (`try_toggle_task_box`, `src/dawn.c:4419-4462`).
- **Live rendering hides the syntax**, which makes this harder for beginners. Markers show only
  when the cursor is inside the element (`CURSOR_IN`, `src/dawn.c:103`; `^B` toggles this). A
  user who did not type the `**` has no on-screen way to learn it.

### 2.3 Touch today

- **Mouse modes:** ?1000h ?1002h ?1006h (`src/dawn_term_common.h:29`); no any-motion.
- **Decoding** (`src/dawn_backend_posix.c:1395-1430`):
  - Button 0 press/release become `DAWN_KEY_MOUSE_CLICK`/`RELEASE`.
  - Button 32 (drag) becomes `DAWN_KEY_MOUSE_DRAG`.
  - Buttons 64/65 become scroll.
  - Every other button is dropped, so a right-click menu would never fire on Termux.
- **In a note:**
  - Tap places the cursor.
  - Double/triple tap selects a word/paragraph (350 ms, 1 cell; `src/dawn.c:4209-4210`).
  - Press-drag extends the selection.
  - A swipe scrolls the pane it started over, and swiping the sheet down closes it
    (`:4611-4675`).
  - Tapping a checkbox toggles it.
  - Taps on the chat's model name and "also in:" are handled.
  - The code says plainly that there is "no floating bar" for a selection (`src/dawn.c:4203-4207`).
- **Taps reach only these modes:** writing (`src/dawn.c:5650-5664`), help page 3 (`:5445`), the
  conflict dialog (`:6194`) and search results (`:6309`).
- **Taps do nothing in:** the welcome screen, history, TOC, timer, style, the frontmatter editor,
  the finished screen and help pages 1-2. There every action needs a key.
- **The Ctrl keys are awkward on a phone.** Every feature beyond typing is a Ctrl key. On Termux
  that means the on-screen Ctrl latch, then the letter (`tests/ux/phone/README.md:32` mentions the
  latch). `Ctrl+/` (31, the AI chat, `src/dawn.c:4891`) is especially awkward.

### 2.4 Layout at phone width

- **`calc_layout()`** (`src/dawn.c:119-180`):
  - Below 80 columns (`AI_PANEL_MIN_COLS`, `src/dawn_types.h:72`) the chat becomes a bottom sheet
    of 45% of the rows. When there is not room for both, it becomes a full overlay. This is good,
    and better than anything ttt has.
  - The note keeps a fixed 4-column margin on each side at any width under 80. At 53 columns that
    is 8 of 53 columns (15%) unused.
- **Welcome margins** step at 100/60 columns (`src/dawn_render.c:142-143`). The two action
  columns force `col2 >= col1 + 20`, but nothing drops the logo or the spacing when the screen
  is short.
- **Headings use kitty text sizing**, so a scaled H1 takes 2 rows on a 24-row screen. This is
  fine, and it means a vertical toolbar row costs real space; see R5.

## 3. Recommendations

Each item says what it borrows from ttt, or says it is dawn's own suggestion. Effort: S is under a
day, M is 1-3 days, L is more than that. All of them stay inside the existing structure:
- New screens are `AppMode` values: `MODE_PUSH`, a `render()` case drawing `render_writing()`
  and then the overlay, and a `handle_input()` case.
- Taps arrive as the existing `DAWN_KEY_MOUSE_*` keys with `input_last_mouse_row/col()`.
- None of them needs a `DawnBackend` change, so the web backend gets them too.
- No tlstore marker string is touched.

### R1. One action table (borrows ttt's command registry)

**Borrowed from ttt:** `internal/command/registry.go:5-12` (ID, Title, Shortcut, Keywords,
Hidden) and `menus.go:192-206` (shortcuts resolved from the same place).

**Change:**
- Add a static `DawnAction` array in `src/dawn.c` (the handlers are `static` there):
  `{id, title, group, keys_label, keywords, bool (*enabled)(void), void (*run)(void)}`.
- `enabled` wraps the guards dawn already uses (`CAN_EDIT()`, `CAN_MODIFY()`, `app.ai_ready`,
  `DAWN_EMBED_LIVE`).
- The `case 12:` bodies in `handle_writing` become calls to `run`. The raw-int switch can stay,
  so this is not a binding table yet. It is the one list that the palette, the toolbar and help
  read from.

**Effort:** M, mostly mechanical. **Why first:** R2, R3, R4 and R8 are each cheap once it exists,
and without it every new surface repeats the hand-written list in `render_help()` that has already
fallen behind the code.

### R2. A command palette, `MODE_PALETTE` (borrows ttt's palette)

**Borrowed from ttt:**
- Prefix modes (`selectdialog_widget.go:468-487`).
- Fuzzy scoring (`fuzzy.go:9-59`).
- The shortcut shown dimmed on each row (`:250-282`).
- A single tap runs a row (`:346-367`), and a tap outside dismisses.
- Width clamped to the screen (`:109-114`).

**Change:**
- A full-width list on a phone, every action from R1 grouped by `group`: Format, Insert, View,
  Navigate, AI, Voice, Note, App.
- Disabled actions are shown dim with the reason, rather than hidden. ttt hides them with
  `Hidden`; dim teaches more.
- Typing filters the list.
- An empty query shows recent actions first, kept in `state.json`.
- The search engine already in `src/dawn_search.c` could provide note search under a prefix.
- Open it from:
  - A tappable `☰` at the right of the status line (see R4).
  - `Ctrl+K` (currently kill-to-end-of-line, `src/dawn.c:5123`). Pick a free key instead if
    kill-line should stay.
  - `?` and the welcome screen.
- New files: `render_palette()` in `src/dawn_render.c` (with a `render_palette_hit()` like
  `render_help_hit()`), and the filter in a pure `src/dawn_palette.c` so it can be unit-tested.

**Effort:** M.

### R3. Formatting commands (dawn's own; ttt has none)

**Change:** a pure `src/dawn_format.c` working on the gap buffer and block cache:
- Toggle bold, italic, strike, highlight, inline code and link on the selection (or the word
  under the cursor). Remove the markers if they are already there.
- Cycle a heading H1 → H2 → H3 → text.
- Toggle a bullet, number, task or quote on the selected lines.
- Insert a code fence, table skeleton, math block, divider, footnote, image or date.

Details:
- Each command is one `save_undo_state()` step and is guarded by `CAN_EDIT()`.
- Each becomes an R1 action, so it is in the palette at once.
- Keys: `Ctrl+B` and `Ctrl+I`/Tab are already taken (`^B` toggles cursor syntax; Tab indents), so
  put formatting under the palette and the bar, not new Ctrl keys.
- Tests: add cases in `tests/test_block.c`, or a `test-format` that links `dawn_format.c` with
  `dawn_gap`/`dawn_block`.

**Effort:** M.

### R4. A tappable formatting bar (dawn's own, using ttt's overflow and `⋮` patterns)

**Borrowed from ttt:**
- `»` overflow that keeps the important item visible (`internal/widgets/tabs.go:123-145, 226-236`).
- `⋮` buttons as the entry point for things otherwise reached by right-click (`callbacks.go:585-587`).
- Clickable status segments (`statusbar_widget.go:153-194`).

**Change:**
- One row, drawn in place of the status line while the keyboard is up or a selection exists, or
  always, as a setting.
- Content: `B I ~ H • ☐ ❝ ` 🔗 ☰`.
- Each target at least 3 cells wide with a cell gap. ttt's 1-cell `✕` with an exact-release check
  is the anti-example.
- Items that don't fit go behind `»`.
- With the bar hidden, the status line still gets tappable segments:
  - `☰`, which opens the palette.
  - `?`, which opens help.
  - The timer, which pauses it.
  - The notice, which opens the activity page.
- Placement:
  - Layout: a `toolbar_row` field in `Layout`, taken from `text_height` in `calc_layout()`.
  - Hit-testing: in `handle_mouse_click()` before the note mapping, the same way `tap_is_over_chat()`
    routes taps today.
  - In sheet mode the bar sits above the chat input.

**Decide on press or release.** Fire on release inside the same target. ttt's menus fire on press
(`contextmenu_widget.go:272-289`), which runs the wrong row when a finger lands slightly off. Dawn
already separates press and release (`src/dawn_backend_posix.c:1419-1422`).

**Effort:** M.

### R5. Help that fits and is generated (borrows ttt's "?" topics)

**Borrowed from ttt:** curated topics that pull their rows from the registry
(`internal/ui/palette_help.go:35-80`), and the hint row (`welcome_view.go:33-34`).

**Change:**
- Build page 1 of `render_help()` from the R1 table, grouped.
- Clamp the box to `app.rows`. Scroll it by swipe and arrows, and close it with a tap outside.
- Add a page "writing Markdown" listing the syntax with a rendered example each (headings, lists,
  tasks, tables, math, footnotes, `:emoji:`).
- Add `?` to the status line hints. Make `?` open help in a note when the note is read-only, and
  always from the bar.
- Fix the README key table at the same time.

**Effort:** S. This is the cheapest fix with a visible effect.

### R6. A tappable, adaptive welcome screen (borrows ttt's welcome)

**Borrowed from ttt:**
- Rows that run on press (`welcome_view.go:310-325`).
- The layout that tries smaller titles and drops spacing before content (`:68-91`).
- Each action showing its shortcut, plus a recents list (`welcome.go:22-42, 262`).

**Change:**
- Make each action in `render_welcome()` a hit region (like `render_help_hit()`) and add a
  `DAWN_KEY_MOUSE_CLICK` case to `MODE_WELCOME` (`src/dawn.c:5509`).
- At phone width use one column of full-width rows (`new note`, `recent notes`, `search`,
  `tour`, `settings`, `help`), and drop the logo when it is short.
- Add the last 3 notes from history as rows.
- Make history, TOC, timer and style lists tappable the same way.

**Effort:** S-M.

### R7. A tour note and one-time hints (borrows ttt's onramp idea; the content is dawn's own)

**Borrowed from ttt:** `palette_help.go` calls itself a new-user onramp. The "how to get the
menubar back" notice (`internal/app/menubar.go:30-38`) is the model for a one-time hint.

**Tour note:**
- On first run (no notes yet), offer a "Tour" row. It opens a built-in read-write sample note,
  `welcome to dawn.md` (a `const char[]` written through `dawn_fsio`).
- It shows every element rendered, and tells the reader to put the cursor inside one to see its
  Markdown.
- It is also reachable from the palette ("Help: open the tour").

**One-time hints:**
- Each is one `notice_post` the first time a situation arises:
  - The first note: "tap ☰ for every command".
  - The first selection: "tap B / I in the bar, or ☰".
  - The first `|` table row: "see Help → Markdown".
  - The first time the chat is available: "Ctrl+/ or ☰ → Ask AI".
- Keep a `hints_seen` bitmask in `settings.json` (`src/dawn_settings.c`), with a setting to turn
  hints off.

**Effort:** S each.

### R8. Slash commands (dawn's own; neither ttt nor dawn has them)

**Change:**
- Typing `/` as the first character of an empty line opens a small inline list under the cursor.
  It is R2's list filtered to Format and Insert actions; typing narrows it, and Enter or a tap
  inserts.
- Esc, a space or a second `/` keeps a literal `/`.
- Hook: next to `check_auto_list_space()` in the `default:` branch of `handle_writing`.
- Draw it as a popup anchored at the cursor. Flip it above the cursor when there is no room
  below, as ttt's autocomplete does (`internal/ui/autocomplete_widget.go:206-230`).
- Off in plain mode (`^R`) and in code blocks.

**Effort:** M (needs R1/R3).

### R9. Phone-width layout fixes (dawn's own, informed by ttt's failures)

**Change:**
- In `calc_layout()`, make `margin` 1-2 below 60 columns instead of 4. At 53 columns that gives
  back 4-6 columns.
- Make every popup clamp both dimensions. `render_popup_box()` takes the height on trust today.
- Give the status line a priority order: notice > selection > words > hints. Drop the low ones
  one at a time instead of all at once (ttt's BUG-036 / `statusbar_widget.go:86-104` shows the
  failure).
- Keep `?1003h` off. Any-motion reports (which ttt uses for hover) are noise on touch.

**Effort:** S.

### R10. Long-press and a settings screen (later)

**Long-press:**
- Detect it from the clock in `dawn_frame()`: a `touch_state.press_active` older than ~450 ms
  with no drag.
- Open a small action sheet for the word or selection: Copy, Cut, Paste, Format ▸, Ask AI, Read
  aloud.
- This replaces ttt's right-click menus, which Termux can't send
  (`src/dawn_backend_posix.c:1413-1428` keeps only buttons 0/32/64/65). Make sure it does not
  break drag-select, which also starts with a press.

**Settings:**
- A `MODE_SETTINGS` form over `settings.json`: theme, style, timer, meaning index, reduced motion,
  hints, toolbar.
- It borrows ttt's Settings form (`internal/app/settings_view.go`) without its 64-column rows: one
  setting per row, and a tap toggles it.

**Effort:** M each.

### What not to copy from ttt

- A menubar. Six labels need about 45 columns and never collapse.
- Right-click-only menus.
- Activating menu items on press.
- 1-cell close targets.
- Any-motion mouse mode.
- A 3-row tab bar.
- Fixed-width settings rows.

## 4. Roadmap

| # | Change | Borrows from ttt | Main dawn files | Effort |
|---|--------|------------------|-----------------|--------|
| 1 | R5 help that fits the phone, is generated, `?` hint in status line; fix README keys | `palette_help.go` topics | `dawn_render.c` `render_help`, `render_popup_box`; `dawn.c` status hints; `README.md` | S |
| 2 | R9 margins, popup clamp, status priorities | lessons from audit BUG-036..040 | `dawn.c` `calc_layout`, `render_status_bar`; `dawn_render.c` | S |
| 3 | R6 tappable adaptive welcome, tappable lists | `welcome_view.go:68-91, 310-325` | `dawn_render.c` `render_welcome`; `dawn.c` `MODE_WELCOME`/history/TOC cases | S-M |
| 4 | R1 action table | `registry.go`, `menus.go:192-206` | `dawn.c` | M |
| 5 | R2 command palette `MODE_PALETTE` | palette modes, fuzzy, row shortcuts, tap-to-run | `dawn_types.h`, `dawn.c`, `dawn_render.c`, new `dawn_palette.c` | M |
| 6 | R3 formatting commands | none (dawn's own) | new `dawn_format.c`, tests | M |
| 7 | R4 formatting bar + tappable status segments | `»` overflow, `⋮`, clickable status segments | `dawn.c` `calc_layout`, `handle_mouse_click`, `render_status_bar` | M |
| 8 | R7 tour note, one-time hints | onramp topics, menubar-restore notice | `dawn_settings.c`, `dawn_file.c`, `dawn_notice.c` | S |
| 9 | R8 slash commands | autocomplete flip-above | `dawn.c` `handle_writing` default branch, `dawn_render.c` | M |
| 10 | R10 long-press sheet, settings screen | settings form (not its widths) | `dawn.c` `touch_state`, `dawn_frame`; new mode | M+M |

Items 1-3 need no new architecture and each is a visible fix on the phone. Item 4 is the hinge:
5, 7, 9 and the generated help all read from it. When any of this ships, bump `DAWN_COMMIT` in
`../tlstore/recipes/cross/build-dawn.sh` as usual. None of it touches the marker strings that
recipe checks.
