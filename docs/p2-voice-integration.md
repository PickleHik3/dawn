# P2 voice: wiring into dawn.c

Branch `p2-voice` adds read-aloud (spec §09, Ctrl+Q) and the dawn side of the dictation marks
protocol (`dawn-dictation-marks.md` v1.1) without touching `src/dawn.c`. Everything dawn.c needs
is in `src/dawn_voice.h`. You need six small edits, listed below. The line numbers are from
`src/dawn.c` at 658732f.

Nothing else is needed: `CMakeLists.txt` already globs `src/dawn*.c`, so re-run cmake to pick up
the new files. The backend already sets mode 7727 and parses the marks. Undo goes through the
existing `save_undo_state()`, and the stop-on-exit hook goes through `backend->on_shutdown`.

## 1. Include (top of file, with the other module headers, ~line 30)

```c
#include "dawn_voice.h"
```

## 2. Key hook: `handle_input()`, right after the theme-report block (after line 4896)

Put it after the `if (key == DAWN_KEY_THEME_DARK || key == DAWN_KEY_THEME_LIGHT) { ... return; }`
block and before `switch (app.mode) {`:

```c
    // Voice helpers (dawn_voice): dictation events, Ctrl+Q read-aloud, any key stopping it.
    if (voice_handle_key(key))
        return;
```

What it consumes:
- `DAWN_KEY_DICTATION`, always. It drains the backend queue and inserts or replaces phrases.
- Ctrl+Q (17) in MODE_WRITING when the chat is not focused. It starts reading.
- While reading, Esc and Ctrl+Q only stop it. Any other key or tap stops it and then goes on to
  dawn as usual. Scroll, drag and release are passed through without stopping.

Optional: line 4879 acks a sticky error notice on any key. To stop a dictation event from
clearing "not saved", change that line to
`if (key != DAWN_KEY_MOUSE_RELEASE && key != DAWN_KEY_DICTATION)`.

## 3. Frame tick: `dawn_frame()`, before `handle_input();` (line 5842)

```c
#if HAS_LIBAI
    ai_pump();
    ai_title_tick();
#endif
    voice_tick();          // <- add
    handle_input();
    render();
```

The main loop already renders every frame and the backend drops identical frames. So the return
value (true while animating) doesn't have to be used.

## 4. Per-byte colour: `render_block()` paragraph text loop, after line 6657

This goes after the selection/background block and its comment
`// If MD_MARK or MD_CODE, background was already set by block_apply_style`, and before
`if (IS_ROW_VISIBLE(...))`:

```c
                // Read-aloud highlight and dictation glow (dawn_voice): over the markdown style,
                // under the selection.
                VoiceStyle voice;
                if (voice_style_at(rs->pos, &voice)) {
                    if (voice.has_fg)
                        set_fg(voice.fg);
                    if (voice.has_bg && !in_sel)
                        set_bg(voice.bg);
                }
```

## 5. Same for headings: `render_header_element()`, the per-grapheme loop (lines 1910-1917)

Without this, headings are read aloud but not highlighted:

```c
            for (size_t p = line_start; p < render_end;) {
                block_apply_style(line_style);
                if (selecting && p >= sel_s && p < sel_e)
                    set_bg(get_select());
                VoiceStyle voice;                                    // <- add
                if (voice_style_at(p, &voice)) {                     // <- add
                    if (voice.has_fg)                                // <- add
                        set_fg(voice.fg);                            // <- add
                    if (voice.has_bg && !(selecting && p >= sel_s && p < sel_e)) // <- add
                        set_bg(voice.bg);                            // <- add
                }                                                    // <- add
                // Styled content - use active_style for inline formatting context
                output_grapheme(&app.text, &p, rs->active_style);
            }
```

## 6. Status word and shimmer overlay

### 6a. `render_status_bar()`: between the notice block's closing `}` (line 2913) and `// Right side hints` (line 2915)

```c
    // The voice helpers' one word (dawn_voice): "listening" while the launcher's mic is open,
    // "reading aloud" during Ctrl+Q. Dim, in the hints' corner. NULL in focus mode.
    const char* voice_text = voice_status_text();
    if (voice_text) {
        int32_t voice_col = status_right - (int32_t)strlen(voice_text) + 1;
        if (voice_col > status_left + 20) {
            move_to(app.rows, voice_col);
            set_fg(get_dim());
            out_str(voice_text);
        }
        return;
    }
```

A notice (for example a read-aloud failure) still wins the corner while it is showing. The word
takes the hints' place.

### 6b. `render_writing()`: just before `move_to(cursor_screen_row, rs.cursor_col);` (line 6444)

```c
    if (!app.view_detached)
        voice_draw_overlay(cursor_screen_row, rs.cursor_col, L.margin + L.text_width - rs.cursor_col + 1);
```

This paints the listening shimmer into the blank cells right of the cursor, only when the cursor
is at the end of a line. When text follows the cursor, `voice_style_at()` tints that text instead.

## Behaviour summary (for review)

- **Ctrl+Q**: reads the selection, or from the start of the cursor's word to the end of the note.
  Sentences are built from the rendered text. Headings, list items and table rows each end a
  sentence. Code blocks, inline code, images, URLs, HTML, math, footnote refs, emoji shortcodes
  and markup characters are left out.
  - Each sentence is one `POST /v1/ai/speak` `{"input": …}`, and the next goes out when that one
    returns.
  - The sentence being spoken gets a background of `bg` 45% of the way to `tertiary_container`
    (`get_highlight_bg()`).
  - The word under the estimate glows from ink to `primary` (`get_accent()`), with a 6-byte
    falloff. The pace comes from the last sentence's `audioSeconds / bytes` and
    `firstSoundMs`; the first sentence uses 0.068 s/byte and a 450 ms lead-in.
  - Stopping sends `POST /v1/ai/speak/stop`. A failure posts the notice
    `read aloud · <reason>`. Selecting nothing speakable posts `nothing to read aloud`.
- **Dictation**: `listen` shows "listening" and the shimmer. `end`, `end;reason=cancel`, or 60 s
  with no marks clears them.
  - `phrase;id=N` plus a paste: inserted at the cursor as one undo step (it replaces a
    selection), shown in `primary` and settling to ink over 1 s with ease-out. This also works
    outside a listen/end pair.
  - `replace;id=N` plus a paste: if the span still holds the exact bytes, or the same text is
    found elsewhere (nearest occurrence), it is swapped with snapshots on both sides, like the
    chat's `apply_edit()`. It fades up from the page over 250 ms, then primary→ink. Otherwise it
    is ignored.
  - With the chat input focused, the text goes into the chat line. Other modes drop it.
- **Focus mode**: text is still inserted and read aloud still speaks. There is no glow, shimmer
  or status word; the sentence highlight stays, since reading was asked for.
- **Reduced motion**: `DAWN_REDUCED_MOTION=1` (or `voice_set_reduced_motion(true)`) turns off the
  glow and shimmer. The launcher has no terminal-visible reduce-motion signal yet.
- **Builds without the launcher bridge** (macOS, `USE_LIBAI=OFF`, web): Ctrl+Q posts
  `read aloud needs termux launcher`. Dictation still works wherever the backend supplies
  `take_dictation` (POSIX only).

## Optional, not required

- Help screen (Ctrl+O): add a `^Q  read aloud` line next to the other shortcuts.
- `render_writing_plain()` (Ctrl+R plain mode) has no hook, so there is no highlight there.
  Reading still works.
- Inline elements drawn by their own renderers (links, emoji, entities, inline math) don't get
  the per-byte colour. The sentence highlight skips over them.
