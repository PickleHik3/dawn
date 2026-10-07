// dawn.c
//! Frontends call into this via dawn_app.h API

#include "dawn_app.h"
#include "dawn_history.h"
#include "dawn_types.h"

#include "dawn_block.h"
#include "dawn_chat.h"
#include "dawn_clipboard.h"
#include "dawn_date.h"
#include "dawn_embed.h"
#include "dawn_file.h"
#include "dawn_fm.h"
#include "dawn_footnote.h"
#include "dawn_gap.h"
#include "dawn_highlight.h"
#include "dawn_image.h"
#include "dawn_input.h"
#include "dawn_nav.h"
#include "dawn_notepath.h"
#include "dawn_notice.h"
#include "dawn_session.h"
#include "dawn_render.h"
#include "dawn_scrollind.h"
#include "dawn_search.h"
#include "dawn_settings.h"
#include "dawn_status.h"
#include "dawn_tex.h"
#include "dawn_theme.h"
#include "dawn_title.h"
#include "dawn_timer.h"
#include "dawn_toc.h"
#include "dawn_utils.h"
#include "dawn_voice.h"
#include "dawn_wrap.h"
#include <errno.h>
#include <stdarg.h>

// Platform capability check macro
#define HAS_CAP(cap) dawn_ctx_has(&app.ctx, cap)

// #region Consolidated Macros and Types

//! Maximum nesting depth for inline markdown styles
#define MAX_STYLE_DEPTH 8

//! Layout calculation result
typedef struct {
    int32_t text_area_cols;
    int32_t ai_cols;
    int32_t ai_start_col;
    bool ai_overlay; //!< The chat covers the whole screen (narrow and short) instead of a side panel
    bool ai_sheet; //!< The chat is a bottom sheet under the note (narrow terminals)
    int32_t ai_top; //!< First screen row of the chat (1 unless it is a sheet)
    int32_t ai_rows; //!< Rows the chat occupies (all of them unless it is a sheet)
    int32_t note_rows; //!< Rows left to the note, status line included (all unless a sheet is up)
    int32_t margin;
    int32_t text_width;
    int32_t top_margin;
    int32_t text_height;
} Layout;

//! Render context passed to rendering functions
typedef struct {
    Layout L;
    int32_t max_row;
    size_t len;
    int32_t* cursor_virtual_row;
    int32_t* cursor_col;
    bool is_print_mode; //!< True when rendering in print mode (render all, no scroll bounds)
} RenderCtx;

//! Inline style stack entry for tracking nested markdown formatting
typedef struct {
    MdStyle style;
    size_t dlen;
    size_t close_pos;
} StyleStackEntry;

//! Render state for second pass
typedef struct {
    int32_t virtual_row;
    int32_t col_width;
    size_t pos;
    MdStyle line_style;
    bool in_block_math;
    StyleStackEntry style_stack[MAX_STYLE_DEPTH];
    int32_t style_depth;
    MdStyle active_style;
    int32_t cursor_virtual_row;
    int32_t cursor_col;
    // Run-based rendering state
    int32_t current_run_idx; //!< Index of current run in block's runs array
    const InlineRun* runs; //!< Pointer to block's runs array (NULL if not using runs)
    int32_t run_count; //!< Number of runs in the array
} RenderState;

// #endregion

// #region Macros for Common Patterns

//! Check if cursor is in a range and syntax hiding is disabled
#define CURSOR_IN(start, end) CURSOR_IN_RANGE(app.cursor, (start), (end), app.hide_cursor_syntax)

//! Check if editing is allowed (not in focus mode or preview mode)
#define CAN_EDIT() (!app.focus_mode && !app.preview_mode)

//! Check if any editing is allowed (not in preview mode)
#define CAN_MODIFY() (!app.preview_mode)

//! Check if key is a printable character (not a special key)
#define IS_PRINTABLE(k) ((k) >= 32 && (k) != 127 && (k) < DAWN_KEY_UP)

// #endregion

// #region Pure Helper Functions

//! Calculate layout dimensions based on window size and AI panel state
static inline Layout calc_layout(void)
{
    Layout l = { 0 };
    l.text_area_cols = app.cols;
    l.ai_cols = 0;
    l.ai_start_col = app.cols + 1;
    l.ai_top = 1;
    l.ai_rows = app.rows;
    l.note_rows = app.rows;

    if (app.ai_open && app.cols < AI_PANEL_MIN_COLS) {
        // A phone-width terminal has no room beside the note: the chat rises from the bottom as a
        // sheet, full width, and the note keeps the rows above it. Recomputed every frame, so the
        // keyboard showing or hiding (which changes app.rows) resizes both.
        l.ai_cols = app.cols;
        l.ai_start_col = 1;
        int32_t sheet_rows = app.rows * AI_SHEET_HEIGHT / 100;
        if (sheet_rows < AI_SHEET_MIN_ROWS)
            sheet_rows = AI_SHEET_MIN_ROWS;
        if (app.rows - sheet_rows < AI_SHEET_MIN_NOTE_ROWS)
            sheet_rows = app.rows - AI_SHEET_MIN_NOTE_ROWS;
        if (sheet_rows >= AI_SHEET_MIN_ROWS) {
            l.ai_sheet = true;
            l.ai_rows = sheet_rows;
            l.ai_top = app.rows - sheet_rows + 1;
            l.note_rows = app.rows - sheet_rows;
        } else {
            // Too short for both: the chat takes the whole screen, and the note keeps its
            // full-width layout underneath for when the chat closes.
            l.ai_overlay = true;
        }
    } else if (app.ai_open) {
        l.ai_cols = app.cols * AI_PANEL_WIDTH / 100;
        if (l.ai_cols < 30)
            l.ai_cols = 30;
        if (l.ai_cols > app.cols - 40)
            l.ai_cols = app.cols - 40;
        l.text_area_cols = app.cols - l.ai_cols - 1;
        l.ai_start_col = l.text_area_cols + 1;
    }

    if (l.ai_cols < 1)
        l.ai_cols = 1;
    if (l.text_area_cols < 1)
        l.text_area_cols = 1;
    l.margin = l.text_area_cols > 80 ? (l.text_area_cols - 70) / 2 : 4;
    l.text_width = l.text_area_cols - l.margin * 2;
    if (l.text_width < 1) {
        l.margin = 0;
        l.text_width = l.text_area_cols;
    }
    l.top_margin = 2;
    // Text starts on row top_margin. Without a sheet it ends two rows above the bottom (a blank
    // row, then the status line). Under a sheet there is no status line (the chat has its own
    // input line), so it ends one row above the sheet: row note_rows is the one blank row between
    // the note's last line and the sheet's edge.
    l.text_height = l.ai_sheet ? l.note_rows - l.top_margin : app.rows - l.top_margin - 2;
    if (l.text_height < 1)
        l.text_height = 1;
    return l;
}

//! How wide the status panel (dawn_status) may be over the note: 40 columns, less on a phone,
//! leaving the note's left side clear.
static inline int32_t status_panel_cols(const Layout* L)
{
    int32_t cols = L->text_area_cols - 4;
    return cols < 40 ? cols : 40;
}

//! Calculate screen row from virtual row
#define VROW_TO_SCREEN(L, vrow, scroll_y) ((L)->top_margin + ((vrow) - (scroll_y)))

//! Check if platform is in print mode
#define IS_PRINT_MODE() (app.ctx.mode == DAWN_MODE_PRINT)

//! Check if screen row is visible (always true in print mode)
#define IS_ROW_VISIBLE(L, screen_row, max_row) \
    (IS_PRINT_MODE() || ((screen_row) >= (L)->top_margin && (screen_row) < (max_row)))

//! Check if cursor is within a range, respecting hide_cursor_syntax toggle
#define CURSOR_IN_RANGE(cursor, start, end, hide_syntax) \
    (((cursor) >= (start) && (cursor) < (end)) && !(hide_syntax))

//! Track cursor position during rendering
static inline void track_cursor(const RenderCtx* ctx, RenderState* rs)
{
    if (rs->pos == app.cursor) {
        rs->cursor_virtual_row = rs->virtual_row;
        rs->cursor_col = ctx->L.margin + 1 + rs->col_width;
    }
}

#define GET_LINE_SCALE(line_style) \
    (HAS_CAP(DAWN_CAP_TEXT_SIZING) ? block_get_scale(line_style) : 1)

//! Fill rest of line with selection background if position is selected
#define FILL_SEL_TO_EOL(ctx, rs, pos, sel_s, sel_e, screen_row)                               \
    do {                                                                                       \
        if (has_selection() && (pos) >= (sel_s) && (pos) < (sel_e) &&                          \
            IS_ROW_VISIBLE(&(ctx)->L, (screen_row), (ctx)->max_row)) {                         \
            int32_t _rem = (ctx)->L.text_width - (rs)->col_width;                              \
            if (_rem > 0) {                                                                    \
                move_to((screen_row), (ctx)->L.margin + 1 + (rs)->col_width);                  \
                set_bg(get_select());                                                          \
                out_spaces(_rem);                                                              \
            }                                                                                  \
        }                                                                                      \
    } while (0)

//! Skip leading whitespace for wrapped lines
static inline size_t skip_leading_space(const GapBuffer* gb, size_t pos, size_t end)
{
    while (pos < end) {
        size_t char_len;
        utf8proc_int32_t cp = gap_utf8_at(gb, pos, &char_len);
        if (cp != ' ')
            break;
        pos += char_len;
    }
    return pos;
}

//! Delete selection if present, updating cursor
static inline void delete_selection_if_any(void)
{
    if (has_selection()) {
        size_t s, e;
        get_selection(&s, &e);
        gap_delete(&app.text, s, e - s);
        app.cursor = s;
        app.selecting = false;
    }
}

//! Find start of current line from cursor position
static inline size_t find_line_start(size_t cursor)
{
    size_t result = cursor;
    while (result > 0 && gap_at(&app.text, result - 1) != '\n')
        result--;
    return result;
}

//! Get the current run, advancing run_idx if needed
//! Returns NULL if no run covers the position
static inline const InlineRun* get_current_run(RenderState* rs)
{
    if (!rs->runs)
        return NULL;
    // Advance to run containing current position
    while (rs->current_run_idx < rs->run_count) {
        const InlineRun* run = &rs->runs[rs->current_run_idx];
        if (rs->pos < run->byte_end) {
            // Current position is within this run
            return run;
        }
        rs->current_run_idx++;
    }
    return NULL;
}

//! Check if position is at the start of a run
#define AT_RUN_START(rs, run) ((run) && (rs)->pos == (run)->byte_start)

//! Check if a list/blockquote item is empty
static inline bool is_item_content_empty(const GapBuffer* gb, size_t cursor, size_t content_start)
{
    if (cursor == content_start)
        return true;
    if (content_start < gap_len(gb) && gap_at(gb, content_start) == '\n')
        return true;
    return false;
}

//! Insert a string at cursor position and advance cursor
static inline void insert_str_at_cursor(GapBuffer* gb, size_t* cursor, const char* str)
{
    while (*str) {
        gap_insert(gb, *cursor, *str++);
        (*cursor)++;
    }
}

//! Insert N copies of a character at cursor position
static inline void insert_chars_at_cursor(GapBuffer* gb, size_t* cursor, char c, int32_t count)
{
    for (int32_t i = 0; i < count; i++) {
        gap_insert(gb, *cursor, c);
        (*cursor)++;
    }
}

//! Handle empty list/quote item - delete marker and insert newline
static inline void handle_empty_list_item(GapBuffer* gb, size_t* cursor, size_t line_start)
{
    gap_delete(gb, line_start, *cursor - line_start);
    *cursor = line_start;
    gap_insert(gb, *cursor, '\n');
    (*cursor)++;
}

//! Get block at position from cache (may return NULL if cache is stale)
static inline Block* get_block_at(size_t pos)
{
    BlockCache* bc = (BlockCache*)app.block_cache;
    if (!bc || !bc->valid || bc->text_len != gap_len(&app.text))
        return NULL;
    return block_at_pos(bc, pos);
}

//! Check if position is in a list item block
//! @param pos byte position to check
//! @param out_indent output: leading spaces count
//! @param out_content_start output: byte position of list content
//! @param out_list_type output: 1=unordered, 2=ordered
//! @param out_task_state output: 0=not task, 1=unchecked, 2=checked
//! @return true if position is in a list item
static inline bool is_in_list_item(size_t pos, int32_t* out_indent, size_t* out_content_start,
    int32_t* out_list_type, int32_t* out_task_state)
{
    Block* b = get_block_at(pos);
    if (!b || b->type != BLOCK_LIST_ITEM)
        return false;
    if (pos >= b->end)
        return false; // Position is past this block (trailing blank lines)
    if (out_indent)
        *out_indent = b->data.list.indent;
    if (out_content_start)
        *out_content_start = b->data.list.content_start;
    if (out_list_type)
        *out_list_type = b->data.list.list_type;
    if (out_task_state)
        *out_task_state = b->data.list.task_state;
    return true;
}

//! Check if position is in an image block
//! @return pointer to Block if in image, NULL otherwise
static inline Block* get_image_block_at(size_t pos)
{
    Block* b = get_block_at(pos);
    if (!b || b->type != BLOCK_IMAGE)
        return NULL;
    return b;
}

//! Check if we just typed a list marker and should auto-insert a space
//! Called after inserting a character. Checks if cursor is right after a list marker.
//! @param key the character that was just typed
static size_t auto_list_space_cursor = (size_t)-1; //!< Cursor just after the last auto-inserted marker space

static inline void check_auto_list_space(int32_t key)
{
    if (key == ' ') {
        // The writer typed their own space right after the auto-space: swallow it
        if (auto_list_space_cursor != (size_t)-1 && auto_list_space_cursor + 1 == app.cursor
            && app.cursor >= 2 && gap_at(&app.text, app.cursor - 2) == ' ') {
            gap_delete(&app.text, app.cursor - 1, 1);
            app.cursor--;
        }
        auto_list_space_cursor = (size_t)-1;
        return;
    }
    auto_list_space_cursor = (size_t)-1;
    // Only check for '.' ')' '-' '*' '+'
    if (key != '.' && key != ')' && key != '-' && key != '*' && key != '+')
        return;

    size_t len = gap_len(&app.text);
    if (app.cursor == 0 || app.cursor > len)
        return;

    // Find line start
    size_t line_start = app.cursor;
    while (line_start > 0 && gap_at(&app.text, line_start - 1) != '\n')
        line_start--;

    // Skip leading spaces
    size_t p = line_start;
    while (p < app.cursor && gap_at(&app.text, p) == ' ')
        p++;

    if (key == '-' || key == '*' || key == '+') {
        // Unordered list: marker must be at p, cursor right after it
        if (p + 1 == app.cursor && gap_at(&app.text, p) == (char)key) {
            gap_insert(&app.text, app.cursor++, ' ');
            auto_list_space_cursor = app.cursor;
        }
        // For '-', if user typed '--' but auto-space got in the way, undo it
        // Pattern: "- -" where cursor is after second dash
        else if (key == '-' &&
                 p + 3 == app.cursor &&
                 gap_at(&app.text, p) == '-' &&
                 gap_at(&app.text, p + 1) == ' ' &&
                 gap_at(&app.text, p + 2) == '-') {
            gap_delete(&app.text, p + 1, 1);
            app.cursor--;
        }
    } else {
        // Ordered list: digits followed by . or )
        // Check if we have digits then the marker we just typed
        size_t digit_start = p;
        while (p < app.cursor && gap_at(&app.text, p) >= '0' && gap_at(&app.text, p) <= '9')
            p++;
        // Need at least one digit, and cursor should be right after the marker
        if (p > digit_start && p + 1 == app.cursor && gap_at(&app.text, p) == (char)key) {
            gap_insert(&app.text, app.cursor++, ' ');
            auto_list_space_cursor = app.cursor;
        }
    }
}

//! Get current text width for word wrapping
static inline int32_t get_text_width(void) { return calc_layout().text_width; }

//! Recalculate wrap segment after prefix rendering
static inline void recalc_wrap_seg(int32_t text_width, int32_t col_width, size_t pos, size_t line_end,
    size_t* seg_end, int32_t* seg_width)
{
    int32_t available = text_width - col_width;
    if (available < 1)
        available = 1;
    *seg_end = gap_find_wrap_point(&app.text, pos, line_end, available, seg_width);
}

// #endregion

// #region Image Helpers

//! Helper to resolve image path and calculate display rows
static int32_t calc_image_rows_for_block(const RenderCtx* ctx, const Block* block,
    char* resolved_out, size_t resolved_size)
{
    size_t path_start = block->data.image.path_start;
    size_t path_len = block->data.image.path_len;
    int32_t img_w = block->data.image.width;
    int32_t img_h = block->data.image.height;

    char raw_path[512];
    size_t plen = path_len < sizeof(raw_path) - 1 ? path_len : sizeof(raw_path) - 1;
    for (size_t i = 0; i < plen; i++) {
        raw_path[i] = gap_at(&app.text, path_start + i);
    }
    raw_path[plen] = '\0';

    char cached_path[512];
    if (!image_resolve_and_cache_to(raw_path, NULL, cached_path, sizeof(cached_path))) {
        return 0;
    }

    if (resolved_out && resolved_size > 0) {
        strncpy(resolved_out, cached_path, resolved_size - 1);
        resolved_out[resolved_size - 1] = '\0';
    }

    if (!image_is_supported(cached_path))
        return 0;

    int32_t img_cols = 0, img_rows_spec = 0;

    if (img_w < 0)
        img_cols = ctx->L.text_width * (-img_w) / 100;
    else if (img_w > 0)
        img_cols = img_w;
    if (img_cols > ctx->L.text_width)
        img_cols = ctx->L.text_width;
    if (img_cols <= 0)
        img_cols = ctx->L.text_width / 2;

    if (img_h < 0)
        img_rows_spec = ctx->L.text_height * (-img_h) / 100;
    else if (img_h > 0)
        img_rows_spec = img_h;

    int32_t pixel_w, pixel_h;
    if (image_get_size(cached_path, &pixel_w, &pixel_h)) {
        return image_calc_rows(pixel_w, pixel_h, img_cols, img_rows_spec);
    }
    return 0;
}

// #endregion

// #region Global State

//! Global application state
App app = { 0 };

// #endregion

// #region Undo/Redo

//! Whether the text is what the snapshot at undo_pos holds.
static bool undo_at_current(void)
{
    if (app.undo_count == 0)
        return false;
    const GapBuffer* gb = &app.text;
    size_t len = gap_len(gb);
    const char* saved = app.undo_stack[app.undo_pos].text;
    if (app.undo_stack[app.undo_pos].text_len != len)
        return false;
    // The text is the buffer with its gap cut out: the part before the gap, then the part after.
    size_t front = gb->gap_start;
    return memcmp(gb->buffer, saved, front) == 0
        && memcmp(gb->buffer + gb->gap_end, saved + front, len - front) == 0;
}

//! Save current text state to undo stack
void save_undo_state(void)
{
    // Whatever follows is a change of its own; typing no longer joins the step before it.
    app.undo_typing = 0;
    app.dirty = true;

    if (app.undo_pos < app.undo_count - 1) {
        for (int32_t i = app.undo_pos + 1; i < app.undo_count; i++) {
            free(app.undo_stack[i].text);
        }
        app.undo_count = app.undo_pos + 1;
    }

    // The top of the stack already holds this text (a change was just undone, or an edit tool
    // took its "after" snapshot): only the cursor is new, so no second copy.
    if (undo_at_current()) {
        app.undo_stack[app.undo_pos].cursor = app.cursor;
        return;
    }

    if (app.undo_count >= MAX_UNDO) {
        free(app.undo_stack[0].text);
        memmove(&app.undo_stack[0], &app.undo_stack[1], (MAX_UNDO - 1) * sizeof(app.undo_stack[0]));
        app.undo_count--;
        app.undo_pos--;
    }

    size_t text_len = gap_len(&app.text);
    char* saved_text = malloc(text_len > 0 ? text_len : 1); // an empty note is a state too
    if (saved_text) {
        gap_copy_to(&app.text, 0, text_len, saved_text);
        app.undo_stack[app.undo_count].text = saved_text;
        app.undo_stack[app.undo_count].text_len = text_len;
        app.undo_stack[app.undo_count].cursor = app.cursor;
        app.undo_count++;
        app.undo_pos = app.undo_count - 1;
    }
}

void undo_reset(void)
{
    for (int32_t i = 0; i < app.undo_count; i++)
        free(app.undo_stack[i].text);
    app.undo_count = 0;
    app.undo_pos = 0;
    app.undo_typing = 0;
    // The text as loaded is the floor: Ctrl+Z stops here instead of reaching the note before.
    save_undo_state();
}

//! Whether a typed character continues a word: letters, digits, '_' and anything non-ASCII.
static inline bool is_word_key(int32_t key)
{
    return key >= 128 || key == '_' || (key >= '0' && key <= '9') || (key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z');
}

//! The undo step for typing key at the cursor. A word typed straight through is one step, with
//! the spaces after it; a new word after a space, punctuation, a line break, a move of the cursor,
//! a change of any other kind or a pause over a second starts the next one. Call before the
//! insert; the cursor after it goes to undo_typed().
static void save_undo_typing(int32_t key)
{
    int64_t now = DAWN_BACKEND(app)->clock(DAWN_CLOCK_MS);
    bool word = is_word_key(key);
    bool space = key == ' ';
    bool joins = app.undo_typing != 0 && !has_selection() && app.cursor == app.undo_typing_cursor
        && now - app.undo_typing_ms <= 1000
        && ((word && app.undo_typing == 1) || (space && app.undo_typing != 0));
    if (!joins)
        save_undo_state();
    app.undo_typing = word ? 1 : space ? 2 : 0;
    app.undo_typing_ms = now;
    app.dirty = true;
}

//! Where the last typed character left the cursor: the next one joins its step only from there.
static inline void undo_typed(void)
{
    app.undo_typing_cursor = app.cursor;
}

//! Restore undo state at given position
static void restore_undo_state(int32_t pos)
{
    size_t current_len = gap_len(&app.text);
    if (current_len > 0)
        gap_delete(&app.text, 0, current_len);
    gap_insert_str(&app.text, 0, app.undo_stack[pos].text, app.undo_stack[pos].text_len);
    app.cursor = app.undo_stack[pos].cursor;
    size_t len = gap_len(&app.text);
    if (app.cursor > len)
        app.cursor = len;
    // The block cache only reparses when the length changes; undoing a same-length edit (a task
    // box tick, a one-letter fix) would otherwise keep drawing the old blocks.
    if (app.block_cache)
        block_cache_invalidate((BlockCache*)app.block_cache);
}

//! Restore cursor to a saved position, clamping to buffer bounds and UTF-8 boundary
static void restore_cursor_position(size_t pos)
{
    size_t len = gap_len(&app.text);
    if (pos > len)
        pos = len;
    if (pos > 0 && pos < len) {
        size_t adjusted = gap_utf8_prev(&app.text, pos + 1);
        if (adjusted < pos)
            pos = adjusted;
    }
    app.cursor = pos;
}

static void undo(void)
{
#if HAS_LIBAI
    // A live title that just landed is the last change: Ctrl+Z puts the old title (and file
    // name) back first.
    if (title_undo())
        return;
#endif
    // Snapshots are taken before a change, so the latest text has none of its own yet: take it
    // now, so one Ctrl+Z undoes one change and Ctrl+Y can come back to this.
    if (app.undo_pos == app.undo_count - 1 && !undo_at_current())
        save_undo_state();
    if (app.undo_pos > 0) {
        app.undo_pos--;
        restore_undo_state(app.undo_pos);
        app.undo_typing = 0;
        app.dirty = true;
    }
}

static void redo(void)
{
    if (app.undo_pos < app.undo_count - 1) {
        app.undo_pos++;
        restore_undo_state(app.undo_pos);
        app.undo_typing = 0;
        app.dirty = true;
    }
}

// #endregion

// #region Smart Editing Helpers

static bool check_smart_delete_symbol(size_t* del_start, size_t* del_len)
{
    if (app.cursor < 3)
        return false;

    char c1 = gap_at(&app.text, app.cursor - 1);
    char c2 = gap_at(&app.text, app.cursor - 2);
    char c3 = gap_at(&app.text, app.cursor - 3);

    if (c1 == ')' && c2 == 'c' && c3 == '(') {
        *del_start = app.cursor - 3;
        *del_len = 3;
        return true;
    }
    if (c1 == ')' && c2 == 'r' && c3 == '(') {
        *del_start = app.cursor - 3;
        *del_len = 3;
        return true;
    }
    if (app.cursor >= 4) {
        char c4 = gap_at(&app.text, app.cursor - 4);
        if (c1 == ')' && c2 == 'm' && c3 == 't' && c4 == '(') {
            *del_start = app.cursor - 4;
            *del_len = 4;
            return true;
        }
    }
    return false;
}

//! Scan backwards for paired delimiter
static bool scan_for_paired_delim(char delim, size_t count, size_t* del_start, size_t* del_len)
{
    size_t check_count = count;
    for (size_t i = app.cursor - count; i > 0 && i >= count; i--) {
        bool match = true;
        for (size_t j = 0; j < check_count; j++) {
            if (gap_at(&app.text, i - 1 - j) != delim) {
                match = false;
                break;
            }
        }
        if (match) {
            *del_start = i - check_count;
            *del_len = app.cursor - *del_start;
            return true;
        }
    }
    return false;
}

static bool check_smart_delete_delimiter(size_t* del_start, size_t* del_len)
{
    size_t len = gap_len(&app.text);
    if (app.cursor == 0)
        return false;

    char c = gap_at(&app.text, app.cursor - 1);

    // ** (bold) or * (italic)
    if (c == '*') {
        if (app.cursor >= 2 && gap_at(&app.text, app.cursor - 2) == '*') {
            if (scan_for_paired_delim('*', 2, del_start, del_len))
                return true;
        } else {
            for (size_t i = app.cursor - 1; i > 0; i--) {
                char prev = gap_at(&app.text, i - 1);
                if (prev == '*') {
                    bool is_double = false;
                    if (i >= 2 && gap_at(&app.text, i - 2) == '*')
                        is_double = true;
                    if (i < len && gap_at(&app.text, i) == '*')
                        is_double = true;
                    if (!is_double) {
                        *del_start = i - 1;
                        *del_len = app.cursor - (i - 1);
                        return true;
                    }
                }
            }
        }
    }

    // ~~ (strikethrough)
    if (c == '~' && app.cursor >= 2 && gap_at(&app.text, app.cursor - 2) == '~') {
        if (scan_for_paired_delim('~', 2, del_start, del_len))
            return true;
    }

    // == (highlight)
    if (c == '=' && app.cursor >= 2 && gap_at(&app.text, app.cursor - 2) == '=') {
        if (scan_for_paired_delim('=', 2, del_start, del_len))
            return true;
    }

    // $ (inline math)
    if (c == '$') {
        for (size_t i = app.cursor - 1; i > 0; i--) {
            if (gap_at(&app.text, i - 1) == '$') {
                *del_start = i - 1;
                *del_len = app.cursor - (i - 1);
                return true;
            }
        }
    }
    return false;
}

static bool check_smart_delete_structure(size_t* del_start, size_t* del_len)
{
    if (app.cursor == 0)
        return false;
    char c = gap_at(&app.text, app.cursor - 1);

    // Check for image block ending at cursor
    if (c == ')' || c == '}') {
        Block* img = get_image_block_at(app.cursor - 1);
        if (img && img->end == app.cursor) {
            *del_start = img->start;
            *del_len = img->end - img->start;
            return true;
        }
    }

    // Check inline runs for links and footnote refs ending at cursor
    Block* b = get_block_at(app.cursor - 1);
    if (b && b->inline_runs && b->inline_run_count > 0) {
        for (int32_t i = 0; i < b->inline_run_count; i++) {
            const InlineRun* run = &b->inline_runs[i];
            if (run->byte_end == app.cursor) {
                if (run->type == RUN_LINK) {
                    *del_start = run->byte_start;
                    *del_len = run->byte_end - run->byte_start;
                    return true;
                }
                if (run->type == RUN_FOOTNOTE_REF) {
                    *del_start = run->byte_start;
                    *del_len = run->byte_end - run->byte_start;
                    return true;
                }
            }
        }
    }
    return false;
}

static bool smart_backspace(void)
{
    size_t del_start, del_len;
    if (check_smart_delete_symbol(&del_start, &del_len) || check_smart_delete_structure(&del_start, &del_len) || check_smart_delete_delimiter(&del_start, &del_len)) {
        gap_delete(&app.text, del_start, del_len);
        app.cursor = del_start;
        return true;
    }
    return false;
}

static void check_auto_newline(char typed_char)
{
    size_t len = gap_len(&app.text);

    if (typed_char == '-' && app.cursor >= 3) {
        // Check if cursor is at end of an HR block
        Block* hr = get_block_at(app.cursor - 1);
        if (hr && hr->type == BLOCK_HR && hr->end == app.cursor) {
            gap_insert(&app.text, app.cursor, '\n');
            app.cursor++;
            return;
        }
    }

    if (typed_char == ')' || typed_char == '}') {
        // Check if cursor is at end of an image block
        Block* img = get_image_block_at(app.cursor - 1);
        if (img && img->end == app.cursor) {
            gap_insert(&app.text, app.cursor, '\n');
            app.cursor++;
            return;
        }
    }

    if (typed_char == '$' && app.cursor >= 4) {
        if (app.cursor >= 2 && gap_at(&app.text, app.cursor - 1) == '$' && gap_at(&app.text, app.cursor - 2) == '$') {
            for (size_t i = app.cursor - 2; i >= 2; i--) {
                if (gap_at(&app.text, i - 1) == '$' && gap_at(&app.text, i - 2) == '$') {
                    gap_insert(&app.text, app.cursor, '\n');
                    app.cursor++;
                    return;
                }
                if (i == 0)
                    break;
            }
        }
    }

    if (typed_char == '`' && app.cursor >= 3) {
        if (gap_at(&app.text, app.cursor - 1) == '`' && gap_at(&app.text, app.cursor - 2) == '`' && gap_at(&app.text, app.cursor - 3) == '`') {
            size_t line_start = find_line_start(app.cursor);
            if (line_start + 3 == app.cursor) {
                // Only a fence that closes an open block gets the newline: count the fence lines
                // before this one, an odd count means a block is open. An opening fence stays as is,
                // so what is typed after it becomes the info string.
                size_t fences = 0;
                size_t ls = 0;
                while (ls < line_start) {
                    if (ls + 3 <= len && gap_at(&app.text, ls) == '`' && gap_at(&app.text, ls + 1) == '`' && gap_at(&app.text, ls + 2) == '`')
                        fences++;
                    while (ls < line_start && gap_at(&app.text, ls) != '\n')
                        ls++;
                    ls++;
                }
                bool found_opening = (fences % 2) == 1;
                if (found_opening) {
                    gap_insert(&app.text, app.cursor, '\n');
                    app.cursor++;
                    return;
                }
            }
        }
    }
}

// #endregion

// #region Chat Markdown Rendering

//! Whether the reply line starting at text is a step the chat noted ("✓ read the note",
//! "✗ couldn't …", "⏹ stopped") rather than the model's own words. Those are drawn dim, as is.
static bool chat_is_step_line(const char* text, size_t len)
{
    static const char* const marks[] = { "✓ ", "✗ ", "⏹ " };
    for (size_t i = 0; i < sizeof(marks) / sizeof(marks[0]); i++) {
        size_t n = strlen(marks[i]);
        if (len >= n && memcmp(text, marks[i], n) == 0)
            return true;
    }
    return false;
}

//! Print text with inline markdown formatting for AI chat
static void chat_print_md(const char* text, size_t start, int32_t len)
{
    bool in_bold = false, in_italic = false, in_code = false;
    bool in_link_text = false, in_link_url = false;

    for (int32_t i = 0; i < len; i++) {
        size_t pos = start + (size_t)i;
        char c = text[pos];
        char next = (i + 1 < len) ? text[pos + 1] : '\0';

        if (c == '`' && !in_link_url) {
            in_code = !in_code;
            if (in_code) {
                set_dim(true);
            } else {
                reset_attrs();
                set_fg(get_fg());
                set_bg(get_ai_bg());
                if (in_bold)
                    set_bold(true);
                if (in_italic)
                    set_italic(true);
            }
            continue;
        }

        if (in_code) {
            out_char(c);
            continue;
        }

        if (c == '*' && next == '*' && !in_link_url) {
            in_bold = !in_bold;
            if (in_bold)
                set_bold(true);
            else {
                reset_attrs();
                set_fg(get_fg());
                set_bg(get_ai_bg());
                if (in_italic)
                    set_italic(true);
            }
            i++;
            continue;
        }

        if (c == '*' && !in_link_url) {
            in_italic = !in_italic;
            if (in_italic)
                set_italic(true);
            else {
                reset_attrs();
                set_fg(get_fg());
                set_bg(get_ai_bg());
                if (in_bold)
                    set_bold(true);
            }
            continue;
        }

        if (c == '[' && !in_link_text && !in_link_url) {
            in_link_text = true;
            set_fg(get_accent());
            set_underline(UNDERLINE_STYLE_CURLY);
            continue;
        }
        if (c == ']' && in_link_text && next == '(') {
            in_link_text = false;
            in_link_url = true;
            reset_attrs();
            set_fg(get_fg());
            set_bg(get_ai_bg());
            if (in_bold)
                set_bold(true);
            if (in_italic)
                set_italic(true);
            i++;
            continue;
        }
        if (c == ')' && in_link_url) {
            in_link_url = false;
            continue;
        }
        if (in_link_url)
            continue;

        out_char(c);
    }
    reset_attrs();
    set_fg(get_fg());
    set_bg(get_ai_bg());
}

// #endregion

// #region Render Helpers - Grapheme Output

//! Output grapheme and advance position, returning display width
#define output_grapheme_advance(gb, pos, style) output_grapheme((gb), (pos), (style))

//! Get grapheme width and next position without output
#define grapheme_width_next(gb, pos, next) gap_grapheme_width((gb), (pos), (next))

//! Wrap check and render a grapheme (raw content - skips typo/emoji/entity replacements)
//! Used by render_raw_dimmed_block and render_cursor_in_element for showing source
static void wrap_and_render_grapheme_raw(const RenderCtx* ctx, RenderState* rs)
{
    size_t next;
    int32_t gw = grapheme_width_next(&app.text, rs->pos, &next);
    if (rs->col_width + gw > ctx->L.text_width && rs->col_width > 0) {
        rs->virtual_row++;
        rs->col_width = 0;
    }
    int32_t screen_row = VROW_TO_SCREEN(&ctx->L, rs->virtual_row, app.scroll_y);
    if (IS_ROW_VISIBLE(&ctx->L, screen_row, ctx->max_row)) {
        if (rs->col_width == 0)
            move_to(screen_row, ctx->L.margin + 1);
        rs->col_width += output_grapheme_advance(&app.text, &rs->pos, MD_CODE);
    } else {
        rs->col_width += gw;
        rs->pos = next;
    }
}

// #endregion

// Forward declarations
static void render_writing(void);

static void update_title(void)
{
    switch (app.mode) {
    case MODE_WELCOME:
        DAWN_BACKEND(app)->set_title("Dawn");
        break;
    case MODE_HISTORY:
        DAWN_BACKEND(app)->set_title("Dawn | History");
        break;
    case MODE_TIMER_SELECT:
        DAWN_BACKEND(app)->set_title("Dawn | Timer");
        break;
    case MODE_HELP:
        DAWN_BACKEND(app)->set_title("Dawn | Help");
        break;
    case MODE_WRITING:
    case MODE_FM_EDIT:
    case MODE_BLOCK_EDIT:
    case MODE_TOC:
    case MODE_SEARCH: {
        // Use document title if available, otherwise "Dawn"
        const char* title = fm_get_string(app.frontmatter, "title");
        DAWN_BACKEND(app)->set_title(title ? title : "Dawn");
        break;
    }
    default:
        DAWN_BACKEND(app)->set_title("Dawn");
        break;
    }
}

// #region Frontmatter Editor Helpers

//! Parse ISO 8601 datetime string into FmFieldDatetime
static bool parse_datetime(const char* s, FmFieldDatetime* dt)
{
    if (!dawn_parse_iso_date(s, &dt->d))
        return false;
    dt->part = 0;
    return true;
}

//! Callback for populating fm_edit from frontmatter iteration
static bool fm_edit_populate_cb(const FmEntry* entry, void* user_data)
{
    Frontmatter* fm = (Frontmatter*)user_data;
    if (app.fm_edit.field_count >= FM_EDIT_MAX_FIELDS)
        return false;

    // Skip lastmod - it's auto-updated on save
    if (strcmp(entry->key, "lastmod") == 0)
        return true;

    FmEditField* field = &app.fm_edit.fields[app.fm_edit.field_count++];
    memset(field, 0, sizeof(*field));
    strncpy(field->key, entry->key, 63);
    field->key[63] = '\0';

    if (entry->type == FM_BOOL) {
        field->kind = FM_FIELD_BOOL;
        field->boolean.value = entry->value && (strcmp(entry->value, "true") == 0 || strcmp(entry->value, "yes") == 0);
    } else if (entry->type == FM_SEQUENCE) {
        field->kind = FM_FIELD_LIST;
        field->list.count = 0;
        field->list.selected = 0;
        field->list.cursor = 0;
        field->list.flow_style = fm_is_sequence_flow(fm, entry->key);
        int count = fm_get_sequence_count(fm, entry->key);
        for (int i = 0; i < count && field->list.count < FM_EDIT_MAX_LIST_ITEMS; i++) {
            const char* item = fm_get_sequence_item(fm, entry->key, i);
            if (item) {
                size_t len = strlen(item);
                if (len >= FM_EDIT_VALUE_SIZE)
                    len = FM_EDIT_VALUE_SIZE - 1;
                memcpy(field->list.items[field->list.count], item, len);
                field->list.items[field->list.count][len] = '\0';
                field->list.item_lens[field->list.count] = len;
                field->list.count++;
            }
        }
    } else if (entry->value && parse_datetime(entry->value, &field->datetime)) {
        field->kind = FM_FIELD_DATETIME;
    } else {
        field->kind = FM_FIELD_STRING;
        if (entry->value) {
            strncpy(field->str.value, entry->value, FM_EDIT_VALUE_SIZE - 1);
            field->str.value[FM_EDIT_VALUE_SIZE - 1] = '\0';
            field->str.len = strlen(field->str.value);
            field->str.cursor = 0; // Start at beginning
            field->str.scroll = 0;
        }
    }

    return true;
}

//! Initialize fm_edit state from frontmatter
static void fm_edit_init(void)
{
    memset(&app.fm_edit, 0, sizeof(app.fm_edit));

    if (app.frontmatter) {
        fm_iterate(app.frontmatter, fm_edit_populate_cb, app.frontmatter);
    }

    if (app.fm_edit.field_count == 0) {
        FmEditField* field = &app.fm_edit.fields[0];
        strcpy(field->key, "title");
        field->kind = FM_FIELD_STRING;
        field->str.value[0] = '\0';
        field->str.len = 0;
        field->str.cursor = 0;
        app.fm_edit.field_count = 1;
    }
}

//! Save fm_edit state back to frontmatter
static void fm_edit_save(void)
{
    if (!app.frontmatter)
        app.frontmatter = fm_create();
    const char* title_before = fm_get_string(app.frontmatter, "title");
    char* old_title = title_before ? dawn_strdup(title_before) : NULL;

    for (int32_t i = 0; i < app.fm_edit.field_count; i++) {
        FmEditField* field = &app.fm_edit.fields[i];

        switch (field->kind) {
        case FM_FIELD_BOOL:
            fm_set_bool(app.frontmatter, field->key, field->boolean.value);
            break;

        case FM_FIELD_DATETIME: {
            char buf[64];
            dawn_format_iso_date(&field->datetime.d, buf, sizeof(buf));
            fm_set_string(app.frontmatter, field->key, buf);
            break;
        }

        case FM_FIELD_LIST: {
            const char* items[FM_EDIT_MAX_LIST_ITEMS];
            for (int32_t j = 0; j < field->list.count; j++) {
                field->list.items[j][field->list.item_lens[j]] = '\0';
                items[j] = field->list.items[j];
            }
            fm_set_sequence(app.frontmatter, field->key, items, field->list.count, field->list.flow_style);
            break;
        }

        case FM_FIELD_STRING:
        default:
            field->str.value[field->str.len] = '\0';
            if (field->str.len > 0) {
                fm_set_string(app.frontmatter, field->key, field->str.value);
            } else {
                fm_remove(app.frontmatter, field->key);
            }
            break;
        }
    }

    // Auto-update lastmod with current ISO 8601 datetime
    DawnTime lt;
    DAWN_BACKEND(app)->localtime(&lt);
    char lastmod_buf[32];
    dawn_format_iso_time(&lt, lastmod_buf, sizeof(lastmod_buf));
    fm_set_string(app.frontmatter, "lastmod", lastmod_buf);

#if HAS_LIBAI
    // A title the writer typed is theirs: live titles never touch it again.
    const char* title_after = fm_get_string(app.frontmatter, "title");
    if ((old_title == NULL) != (title_after == NULL) || (old_title && title_after && strcmp(old_title, title_after) != 0))
        title_user_edited();
#endif
    free(old_title);

    // The user asked for frontmatter in this note, plain file or not.
    app.write_fm = true;
    app.dirty = true;
}

// #endregion

// #region Render Helpers - Raw Content

//! Render raw dimmed content (block element with newlines)
//! Shows source text dimmed, skips all replacements (typo, emoji, entity)
//! Selection background takes precedence when active
static void render_raw_dimmed_block(const RenderCtx* ctx, RenderState* rs, size_t end_pos)
{
    size_t sel_s, sel_e;
    get_selection(&sel_s, &sel_e);
    bool selecting = has_selection();

    while (rs->pos < end_pos && rs->pos < ctx->len) {
        int32_t screen_row = VROW_TO_SCREEN(&ctx->L, rs->virtual_row, app.scroll_y);
        track_cursor(ctx, rs);

        // Set colors - selection takes precedence
        bool in_sel = selecting && rs->pos >= sel_s && rs->pos < sel_e;
        if (in_sel) {
            set_bg(get_select());
            set_fg(get_fg());
        } else {
            set_bg(get_bg());
            set_fg(get_dim());
        }

        char ch = gap_at(&app.text, rs->pos);
        if (ch == '\n') {
            rs->pos++;
            rs->virtual_row++;
            rs->col_width = 0;
            if (IS_ROW_VISIBLE(&ctx->L, screen_row, ctx->max_row))
                move_to(screen_row + 1, ctx->L.margin + 1);
        } else if (ch == '\t') {
            int32_t tab_width = 4 - (rs->col_width % 4);
            if (IS_ROW_VISIBLE(&ctx->L, screen_row, ctx->max_row)) {
                for (int32_t ti = 0; ti < tab_width; ti++) {
                    out_char(' ');
                }
            }
            rs->col_width += tab_width;
            rs->pos++;
        } else {
            wrap_and_render_grapheme_raw(ctx, rs);
        }
    }
    set_fg(get_fg());
    set_bg(get_bg());
}

//! Render raw prefix with cursor tracking
//! Shows source text dimmed, skips all replacements (typo, emoji, entity)
static void render_raw_prefix(const RenderCtx* ctx, RenderState* rs, size_t content_end)
{
    set_fg(get_dim());
    while (rs->pos < content_end && rs->pos < ctx->len) {
        track_cursor(ctx, rs);
        rs->col_width += output_grapheme_advance(&app.text, &rs->pos, MD_CODE);
    }
    set_fg(get_fg());
}

// #endregion

// #region Table Rendering Helpers

//! Check if character at position is a break character (space or dash)
static bool is_cell_break_char(size_t pos)
{
    char c = gap_at(&app.text, pos);
    return c == ' ' || c == '-';
}

//! Calculate wrapped lines for a table cell using word-boundary aware wrapping
static int32_t calc_cell_wrapped_lines_with_runs(const InlineParseResult* runs, int32_t col_width)
{
    if (!runs || runs->run_count == 0)
        return 1;

    int32_t lines = 1;
    int32_t line_width = 0;

    // Track last break point
    int32_t last_break_run = -1;
    size_t last_break_pos = 0;
    int32_t width_at_break = 0;

    int32_t run_idx = 0;
    size_t pos = 0;

    // Find first visible run
    while (run_idx < runs->run_count && runs->runs[run_idx].type == RUN_DELIM) {
        run_idx++;
    }
    if (run_idx < runs->run_count) {
        pos = runs->runs[run_idx].byte_start;
    }

    while (run_idx < runs->run_count) {
        const InlineRun* run = &runs->runs[run_idx];

        // Skip delimiter runs
        if (run->type == RUN_DELIM) {
            run_idx++;
            if (run_idx < runs->run_count) {
                pos = runs->runs[run_idx].byte_start;
            }
            continue;
        }

        // Process characters in this run
        while (pos < run->byte_end) {
            // Check for break character BEFORE measuring
            bool is_break = is_cell_break_char(pos);

            size_t next_pos;
            int32_t gw = grapheme_width_next(&app.text, pos, &next_pos);

            // Would this exceed width?
            if (line_width + gw > col_width && line_width > 0) {
                // Wrap at last break point if available
                if (last_break_run >= 0 && width_at_break > 0) {
                    // Restart from after the break point
                    run_idx = last_break_run;
                    pos = last_break_pos;
                    line_width = 0;
                    last_break_run = -1;
                    width_at_break = 0;
                    lines++;

                    // Skip any leading spaces on new line
                    while (run_idx < runs->run_count) {
                        const InlineRun* r = &runs->runs[run_idx];
                        if (r->type == RUN_DELIM) {
                            run_idx++;
                            if (run_idx < runs->run_count)
                                pos = runs->runs[run_idx].byte_start;
                            continue;
                        }
                        if (pos < r->byte_end && gap_at(&app.text, pos) == ' ') {
                            size_t np;
                            grapheme_width_next(&app.text, pos, &np);
                            pos = np;
                        } else {
                            break;
                        }
                    }
                    continue;
                } else {
                    // No break point - wrap mid-word
                    lines++;
                    line_width = gw;
                    last_break_run = -1;
                    width_at_break = 0;
                }
            } else {
                line_width += gw;
            }

            // Record break point AFTER the space/dash
            if (is_break) {
                last_break_run = run_idx;
                last_break_pos = next_pos;
                width_at_break = line_width;
            }

            pos = next_pos;
        }

        // Move to next run
        run_idx++;
        if (run_idx < runs->run_count) {
            pos = runs->runs[run_idx].byte_start;
        }
    }

    return lines;
}

//! Find wrap point for table cell line (word-boundary aware)
//! Returns the end position for this line and updates state to continue from
typedef struct {
    int32_t run_idx;
    size_t pos;
} CellLineEnd;

static CellLineEnd find_cell_line_end(const InlineParseResult* runs, int32_t start_run, size_t start_pos,
    int32_t col_width, int32_t* out_width)
{
    CellLineEnd result = { start_run, start_pos };
    if (!runs || runs->run_count == 0) {
        if (out_width)
            *out_width = 0;
        return result;
    }

    int32_t line_width = 0;

    // Track last break point
    int32_t last_break_run = -1;
    size_t last_break_pos = 0;
    int32_t width_at_break = 0;

    int32_t run_idx = start_run;
    size_t pos = start_pos;

    // Skip initial delimiters
    while (run_idx < runs->run_count && runs->runs[run_idx].type == RUN_DELIM) {
        run_idx++;
        if (run_idx < runs->run_count)
            pos = runs->runs[run_idx].byte_start;
    }

    while (run_idx < runs->run_count) {
        const InlineRun* run = &runs->runs[run_idx];

        if (run->type == RUN_DELIM) {
            run_idx++;
            if (run_idx < runs->run_count)
                pos = runs->runs[run_idx].byte_start;
            continue;
        }

        while (pos < run->byte_end) {
            bool is_break = is_cell_break_char(pos);

            size_t next_pos;
            int32_t gw = grapheme_width_next(&app.text, pos, &next_pos);

            if (line_width + gw > col_width && line_width > 0) {
                // Would exceed - wrap here
                if (last_break_run >= 0 && width_at_break > 0) {
                    // Wrap at last break point
                    result.run_idx = last_break_run;
                    result.pos = last_break_pos;
                    if (out_width)
                        *out_width = width_at_break;
                } else {
                    // No break point - wrap mid-word
                    result.run_idx = run_idx;
                    result.pos = pos;
                    if (out_width)
                        *out_width = line_width;
                }
                return result;
            }

            line_width += gw;

            if (is_break) {
                last_break_run = run_idx;
                last_break_pos = next_pos;
                width_at_break = line_width;
            }

            pos = next_pos;
        }

        run_idx++;
        if (run_idx < runs->run_count)
            pos = runs->runs[run_idx].byte_start;
    }

    // Reached end of content
    result.run_idx = run_idx;
    result.pos = pos;
    if (out_width)
        *out_width = line_width;
    return result;
}

//! Skip leading spaces at the start of a wrapped line
static void skip_cell_leading_spaces(const InlineParseResult* runs, int32_t* run_idx, size_t* pos)
{
    while (*run_idx < runs->run_count) {
        const InlineRun* r = &runs->runs[*run_idx];
        if (r->type == RUN_DELIM) {
            (*run_idx)++;
            if (*run_idx < runs->run_count)
                *pos = runs->runs[*run_idx].byte_start;
            continue;
        }
        if (*pos < r->byte_end && gap_at(&app.text, *pos) == ' ') {
            size_t np;
            grapheme_width_next(&app.text, *pos, &np);
            *pos = np;
            if (*pos >= r->byte_end) {
                (*run_idx)++;
                if (*run_idx < runs->run_count)
                    *pos = runs->runs[*run_idx].byte_start;
            }
        } else {
            break;
        }
    }
}

//! Calculate column widths for table
static void calc_table_col_widths(int32_t col_count, int32_t text_width, int32_t* col_widths)
{
    int32_t border_overhead = (col_count * 3) + 1;
    int32_t available_width = text_width - border_overhead;
    int32_t base_col_width = available_width / col_count;
    if (base_col_width < 8)
        base_col_width = 8;
    if (base_col_width > 30)
        base_col_width = 30;

    for (int32_t i = 0; i < col_count; i++) {
        col_widths[i] = base_col_width;
    }
}

//! Render table horizontal border
static void render_table_hborder(const Layout* L, int32_t screen_row, int32_t max_row,
    int32_t col_count, const int32_t* col_widths,
    const char* left, const char* mid, const char* right)
{
    if (!IS_ROW_VISIBLE(L, screen_row, max_row))
        return;

    move_to(screen_row, L->margin + 1);
    set_fg(get_border());
    out_str(left);
    for (int32_t ci = 0; ci < col_count; ci++) {
        for (int32_t w = 0; w < col_widths[ci] + 2; w++)
            out_str("─");
        if (ci < col_count - 1)
            out_str(mid);
    }
    out_str(right);
    set_fg(get_fg());
}

// #endregion

// #region Block Element Rendering

//! Render image element
static bool render_image_element(const RenderCtx* ctx, RenderState* rs, const Block* block)
{
    size_t total_len = block->end - block->start;

    if (CURSOR_IN_RANGE(app.cursor, rs->pos, rs->pos + total_len, app.hide_cursor_syntax)) {
        // Cursor is inside image - render raw dimmed using block renderer for proper newline handling
        render_raw_dimmed_block(ctx, rs, rs->pos + total_len);
        return true;
    }

    // Check if images are supported
    if (!HAS_CAP(DAWN_CAP_IMAGES)) {
        // No image support - render raw text
        render_raw_dimmed_block(ctx, rs, rs->pos + total_len);
        return true;
    }

    // Track cursor position at image
    track_cursor(ctx, rs);

    char resolved_path[1024];
    resolved_path[0] = '\0';
    int32_t img_rows = calc_image_rows_for_block(ctx, block, resolved_path, sizeof(resolved_path));

    // If image can't be resolved, render raw text
    if (!resolved_path[0] || img_rows <= 0) {
        render_raw_dimmed_block(ctx, rs, rs->pos + total_len);
        return true;
    }

    rs->pos += total_len;

    int32_t img_screen_row = VROW_TO_SCREEN(&ctx->L, rs->virtual_row, app.scroll_y);
    int32_t img_end_row = img_screen_row + img_rows;

    if (img_end_row > ctx->L.top_margin && img_screen_row < ctx->max_row) {
        int32_t img_w = block->data.image.width;
        int32_t img_cols = 0;
        if (img_w < 0)
            img_cols = ctx->L.text_width * (-img_w) / 100;
        else if (img_w > 0)
            img_cols = img_w;
        else
            img_cols = ctx->L.text_width / 2;
        if (img_cols > ctx->L.text_width)
            img_cols = ctx->L.text_width;

        int32_t crop_top_rows = 0, visible_rows = img_rows;
        int32_t draw_row = img_screen_row;

        // Skip cropping in print mode - render full image
        if (!IS_PRINT_MODE()) {
            if (img_screen_row < ctx->L.top_margin) {
                crop_top_rows = ctx->L.top_margin - img_screen_row;
                visible_rows -= crop_top_rows;
                draw_row = ctx->L.top_margin;
            }
            if (img_end_row > ctx->max_row) {
                visible_rows = ctx->max_row - draw_row;
            }
        }

        if (visible_rows > 0) {
            move_to(draw_row, ctx->L.margin + 1);
            if (crop_top_rows > 0 || visible_rows < img_rows) {
                image_display_at_cropped(resolved_path, draw_row, ctx->L.margin + 1,
                    img_cols, crop_top_rows, visible_rows);
            } else {
                image_display_at(resolved_path, draw_row, ctx->L.margin + 1, img_cols, 0);
            }
        }
    }
    rs->virtual_row += img_rows;
    rs->col_width = 0;
    return true;
}

//! Render HR element
static bool render_hr_element(const RenderCtx* ctx, RenderState* rs, const Block* block)
{
    size_t hr_len = block->end - block->start;
    if (hr_len > 0 && gap_at(&app.text, block->end - 1) == '\n')
        hr_len--;
    int32_t screen_row = VROW_TO_SCREEN(&ctx->L, rs->virtual_row, app.scroll_y);
    // Show raw --- when cursor is anywhere on the HR line (including at end)
    bool cursor_in_hr = app.cursor >= rs->pos && app.cursor <= rs->pos + hr_len && !app.hide_cursor_syntax;

    // Check if HR overlaps selection
    size_t sel_s, sel_e;
    get_selection(&sel_s, &sel_e);
    bool in_sel = has_selection() && rs->pos < sel_e && rs->pos + hr_len > sel_s;

    if (cursor_in_hr) {
        set_fg(get_dim());
        for (size_t i = 0; i < hr_len && rs->pos < ctx->len; i++) {
            screen_row = VROW_TO_SCREEN(&ctx->L, rs->virtual_row, app.scroll_y);
            track_cursor(ctx, rs);
            char ch = gap_at(&app.text, rs->pos);
            if (ch == '\n') {
                rs->pos++;
                break;
            }
            wrap_and_render_grapheme_raw(ctx, rs);
        }
        // Track cursor at end of HR content
        track_cursor(ctx, rs);
        set_fg(get_fg());
    } else {
        if (IS_ROW_VISIBLE(&ctx->L, screen_row, ctx->max_row)) {
            move_to(screen_row, ctx->L.margin + 1);
            if (in_sel)
                set_bg(get_select());
            set_fg(get_dim());
            for (int32_t i = 0; i < ctx->L.text_width; i++)
                out_str("─");
            set_fg(get_fg());
            if (in_sel)
                set_bg(get_bg());
        }
        if (app.cursor >= rs->pos && app.cursor < rs->pos + hr_len) {
            rs->cursor_virtual_row = rs->virtual_row;
            rs->cursor_col = ctx->L.margin + 1;
        }
        rs->pos += hr_len;
    }
    rs->virtual_row++;
    rs->col_width = 0;
    return true;
}

//! Render header element with centered text and decorative underline
//! Used when text scaling is available for beautiful typography
static bool render_header_element(const RenderCtx* ctx, RenderState* rs, const Block* block)
{
    size_t header_content = block->data.header.content_start;
    size_t header_end = block->end;
    if (header_end > 0 && gap_at(&app.text, header_end - 1) == '\n')
        header_end--;
    int32_t header_level = block->data.header.level;
    MdStyle line_style = block_style_for_header_level(header_level);
    int32_t text_scale = GET_LINE_SCALE(line_style);
    size_t header_total = header_end - rs->pos;
    if (header_end < ctx->len && gap_at(&app.text, header_end) == '\n')
        header_total++;
    else if (header_end >= ctx->len)
        header_total++; // Include cursor at end of file

    bool cursor_in_header = CURSOR_IN_RANGE(app.cursor, rs->pos, rs->pos + header_total, app.hide_cursor_syntax);

    if (cursor_in_header) {
        // Editing mode: show raw markdown with scaling, left-aligned
        int32_t screen_row = VROW_TO_SCREEN(&ctx->L, rs->virtual_row, app.scroll_y);
        MdFracScale frac = block_get_frac_scale(line_style);
        current_text_scale = frac.scale;
        current_frac_num = frac.num;
        current_frac_denom = frac.denom;

        // Get selection range
        size_t sel_s, sel_e;
        get_selection(&sel_s, &sel_e);
        bool selecting = has_selection();

        // Render the raw header syntax including # prefix
        if (IS_ROW_VISIBLE(&ctx->L, screen_row, ctx->max_row)) {
            move_to(screen_row, ctx->L.margin + 1);
        }

        // Track character position (unscaled) for wrapping calculation
        int32_t char_col = 0;
        int32_t available_width = ctx->L.text_width / text_scale;
        if (available_width < 1)
            available_width = 1;

        for (size_t p = rs->pos; p < header_end && p < ctx->len;) {
            screen_row = VROW_TO_SCREEN(&ctx->L, rs->virtual_row, app.scroll_y);

            // Track cursor with scaled column position
            if (p == app.cursor) {
                rs->cursor_virtual_row = rs->virtual_row;
                rs->cursor_col = ctx->L.margin + 1 + (char_col * text_scale);
            }

            if (char_col >= available_width) {
                rs->virtual_row += text_scale;
                char_col = 0;
                screen_row = VROW_TO_SCREEN(&ctx->L, rs->virtual_row, app.scroll_y);
                if (IS_ROW_VISIBLE(&ctx->L, screen_row, ctx->max_row)) {
                    move_to(screen_row, ctx->L.margin + 1);
                }
            }

            block_apply_style(line_style);
            if (selecting && p >= sel_s && p < sel_e)
                set_bg(get_select());

            size_t next;
            int32_t gw = grapheme_width_next(&app.text, p, &next);
            if (IS_ROW_VISIBLE(&ctx->L, screen_row, ctx->max_row)) {
                // Raw mode - showing markdown source, skip replacements
                output_grapheme(&app.text, &p, MD_CODE);
            } else {
                p = next;
            }
            char_col += gw;
        }

        // A cursor at the end of the heading sits in the cell after its last sized character,
        // which holds nothing sized, so the terminal draws it one cell tall. Write one sized
        // blank there: the cursor then lands on a scaled cell and is drawn heading-tall.
        if (header_end == app.cursor && char_col < available_width
            && (current_text_scale > 1 || (current_frac_num > 0 && current_frac_denom > 0))
            && HAS_CAP(DAWN_CAP_TEXT_SIZING)) {
            screen_row = VROW_TO_SCREEN(&ctx->L, rs->virtual_row, app.scroll_y);
            if (IS_ROW_VISIBLE(&ctx->L, screen_row, ctx->max_row)) {
                block_apply_style(line_style);
                if (current_frac_num > 0 && current_frac_denom > current_frac_num)
                    print_scaled_frac_char(' ', current_text_scale, current_frac_num, current_frac_denom);
                else
                    print_scaled_char(' ', current_text_scale);
            }
        }

        // Track cursor at newline position
        if (header_end < ctx->len && gap_at(&app.text, header_end) == '\n') {
            if (header_end == app.cursor) {
                rs->cursor_virtual_row = rs->virtual_row;
                rs->cursor_col = ctx->L.margin + 1 + (char_col * text_scale);
            }
        }
        // Track cursor at end of file (header with no trailing newline)
        if (header_end == app.cursor && header_end >= ctx->len) {
            rs->cursor_virtual_row = rs->virtual_row;
            rs->cursor_col = ctx->L.margin + 1 + (char_col * text_scale);
        }
        rs->pos = header_end;
        if (rs->pos < ctx->len && gap_at(&app.text, rs->pos) == '\n')
            rs->pos++;

        rs->virtual_row += text_scale;
        rs->col_width = 0;
        rs->line_style = 0;
        current_text_scale = 1;
        current_frac_num = 0;
        current_frac_denom = 0;
        block_apply_style(0);
        return true;
    }

    // Beautiful mode: centered header with balanced word wrapping
    int32_t screen_row = VROW_TO_SCREEN(&ctx->L, rs->virtual_row, app.scroll_y);

    // Get selection range
    size_t sel_s, sel_e;
    get_selection(&sel_s, &sel_e);
    bool selecting = has_selection();

    // Skip any leading whitespace after the # prefix
    size_t content_start = header_content;
    while (content_start < header_end && gap_at(&app.text, content_start) == ' ') {
        content_start++;
    }

    // Also trim trailing whitespace
    size_t content_end = header_end;
    while (content_end > content_start && gap_at(&app.text, content_end - 1) == ' ') {
        content_end--;
    }

    MdFracScale frac = block_get_frac_scale(line_style);

    // Available width in character cells (not scaled cells)
    int32_t available_char_width = ctx->L.text_width / text_scale;
    if (available_char_width < 1)
        available_char_width = 1;

    // Calculate total content width
    int32_t total_content_width = 0;
    for (size_t p = content_start; p < content_end;) {
        size_t next;
        total_content_width += grapheme_width_next(&app.text, p, &next);
        p = next;
    }

    // For balanced wrapping, find the optimal break point that creates
    // the most evenly sized lines. Collect all word break positions first.
    size_t break_positions[64];
    int32_t break_widths[64]; // cumulative width at each break
    int32_t break_count = 0;
    int32_t cumulative_width = 0;

    for (size_t p = content_start; p < content_end && break_count < 63;) {
        char c = gap_at(&app.text, p);
        size_t next;
        int32_t gw = grapheme_width_next(&app.text, p, &next);
        cumulative_width += gw;
        p = next;

        // Record position after each space as a potential break point
        if (c == ' ') {
            break_positions[break_count] = p;
            break_widths[break_count] = cumulative_width;
            break_count++;
        }
    }

    // Find the break point that creates most balanced lines
    size_t best_break = content_end;
    int32_t best_diff = total_content_width; // worst case: all on one line

    if (total_content_width > available_char_width && break_count > 0) {
        for (int32_t i = 0; i < break_count; i++) {
            int32_t first_line_width = break_widths[i] - 1; // exclude trailing space
            int32_t second_line_width = total_content_width - break_widths[i];

            // Both lines must fit within available width
            if (first_line_width <= available_char_width && second_line_width <= available_char_width) {
                int32_t diff = first_line_width > second_line_width
                    ? first_line_width - second_line_width
                    : second_line_width - first_line_width;
                if (diff < best_diff) {
                    best_diff = diff;
                    best_break = break_positions[i];
                }
            }
        }
    }

    // Word-wrap the header content into lines
    size_t line_start = content_start;
    while (line_start < content_end) {
        size_t line_end; // where this line's content ends (for advancing)
        size_t render_end; // where to stop rendering (excludes trailing space)
        int32_t line_width;

        // Use the pre-calculated optimal break for first line if wrapping needed
        if (line_start == content_start && best_break < content_end) {
            line_end = best_break;
            render_end = line_end;
            // Trim trailing spaces from render end
            while (render_end > line_start && gap_at(&app.text, render_end - 1) == ' ') {
                render_end--;
            }
            // Calculate width of what we'll actually render
            line_width = 0;
            for (size_t p = line_start; p < render_end;) {
                size_t next;
                line_width += grapheme_width_next(&app.text, p, &next);
                p = next;
            }
        } else {
            // For subsequent lines or single line, take everything remaining
            line_end = content_end;
            render_end = content_end;
            line_width = 0;
            for (size_t p = line_start; p < render_end;) {
                size_t next;
                line_width += grapheme_width_next(&app.text, p, &next);
                p = next;
            }
        }

        // Skip leading spaces on continuation lines
        if (line_start > content_start) {
            while (line_start < line_end && gap_at(&app.text, line_start) == ' ') {
                line_start++;
            }
            render_end = line_end; // render_end is same as line_end for continuation
            // Recalculate line width after trimming
            line_width = 0;
            for (size_t p = line_start; p < render_end;) {
                size_t next;
                line_width += grapheme_width_next(&app.text, p, &next);
                p = next;
            }
        }

        // Calculate centering for this line
        int32_t scaled_line_width = line_width * text_scale;
        int32_t left_padding = (ctx->L.text_width - scaled_line_width) / 2;
        if (left_padding < 0)
            left_padding = 0;

        // Render this line (only up to render_end, excluding trailing spaces)
        screen_row = VROW_TO_SCREEN(&ctx->L, rs->virtual_row, app.scroll_y);
        if (IS_ROW_VISIBLE(&ctx->L, screen_row, ctx->max_row)) {
            current_text_scale = frac.scale;
            current_frac_num = frac.num;
            current_frac_denom = frac.denom;

            move_to(screen_row, ctx->L.margin + 1 + left_padding);

            for (size_t p = line_start; p < render_end;) {
                block_apply_style(line_style);
                if (selecting && p >= sel_s && p < sel_e)
                    set_bg(get_select());
                VoiceStyle voice;
                if (voice_style_at(p, &voice)) {
                    if (voice.has_fg)
                        set_fg(voice.fg);
                    if (voice.has_bg && !(selecting && p >= sel_s && p < sel_e))
                        set_bg(voice.bg);
                }
                // Styled content - use active_style for inline formatting context
                output_grapheme(&app.text, &p, rs->active_style);
            }

            // Draw decorative underline on separate row for H2+ headers (only after last line)
            bool is_last_line = (line_end >= content_end);
            if (header_level > 1 && is_last_line) {
                // Reset text scale for underline
                current_text_scale = 1;
                current_frac_num = 0;
                current_frac_denom = 0;
                block_apply_style(0);

                int32_t underline_row = screen_row + text_scale;
                if (IS_ROW_VISIBLE(&ctx->L, underline_row, ctx->max_row)) {
                    // Decorative underline is ~1/3 width of text, centered
                    int32_t underline_width = scaled_line_width / 3;
                    if (underline_width < 4)
                        underline_width = 4;
                    int32_t underline_padding = left_padding + (scaled_line_width - underline_width) / 2;

                    move_to(underline_row, ctx->L.margin + 1 + underline_padding);
                    set_fg(get_dim());
                    for (int32_t i = 0; i < underline_width; i++) {
                        out_str("─");
                    }
                    set_fg(get_fg());
                }
                rs->virtual_row++; // account for underline row
            }
        }

        // Track cursor position
        if (app.cursor >= rs->pos && app.cursor < rs->pos + header_total) {
            rs->cursor_virtual_row = rs->virtual_row;
            rs->cursor_col = ctx->L.margin + 1 + left_padding;
        }

        rs->virtual_row += text_scale;
        line_start = line_end;
    }

    // Skip to end of header
    rs->pos = header_end;
    if (rs->pos < ctx->len && gap_at(&app.text, rs->pos) == '\n')
        rs->pos++;

    rs->col_width = 0;
    rs->line_style = 0;
    current_text_scale = 1;
    current_frac_num = 0;
    current_frac_denom = 0;
    block_apply_style(0);

    (void)header_level;
    return true;
}
#define CODE_TAB_WIDTH 4
//! Render code block element
static bool render_code_block_element(const RenderCtx* ctx, RenderState* rs, const Block* block)
{
    size_t cb_total_len = block->end - block->start;
    int32_t screen_row = VROW_TO_SCREEN(&ctx->L, rs->virtual_row, app.scroll_y);

    // Cursor inside code block - render raw dimmed
    if (CURSOR_IN_RANGE(app.cursor, rs->pos, rs->pos + cb_total_len, app.hide_cursor_syntax)) {
        render_raw_dimmed_block(ctx, rs, rs->pos + cb_total_len);
        rs->col_width = 0;
        return true;
    }

    // Extract language identifier
    char lang[32] = { 0 };
    if (block->data.code.lang_len > 0) {
        size_t copy_len = block->data.code.lang_len < sizeof(lang) - 1 ? block->data.code.lang_len : sizeof(lang) - 1;
        gap_copy_to(&app.text, block->data.code.lang_start, copy_len, lang);
        lang[copy_len] = '\0';
    }

    // Extract code content
    size_t content_len = block->data.code.content_len;
    char* code = malloc(content_len + 1);
    if (!code) {
        rs->pos += cb_total_len;
        rs->col_width = 0;
        return true;
    }

    gap_copy_to(&app.text, block->data.code.content_start, content_len, code);
    code[content_len] = '\0';

    // Apply syntax highlighting
    size_t hl_len = 0;
    char* highlighted = highlight_code(app.hl_ctx, code, content_len,
        lang[0] ? lang : NULL, &hl_len);
    const char* src = highlighted ? highlighted : code;
    const char* p = src;

    // Get selection range for code block content
    size_t sel_s, sel_e;
    get_selection(&sel_s, &sel_e);
    bool selecting = has_selection();
    size_t src_pos = block->data.code.content_start; // Track position in source document

    // Track cursor position at block start
    track_cursor(ctx, rs);

    bool first_line = true;
    int32_t label_width = lang[0] ? utf8_display_width(lang, strlen(lang)) + 1 : 0; // +1 for trailing space

    while (*p || first_line) {
        screen_row = VROW_TO_SCREEN(&ctx->L, rs->virtual_row, app.scroll_y);

        // Early exit if past visible area (but not in print mode)
        if (!ctx->is_print_mode && screen_row > ctx->max_row)
            break;

        bool visible = IS_ROW_VISIBLE(&ctx->L, screen_row, ctx->max_row);

        if (visible) {
            move_to(screen_row, ctx->L.margin + 1);
            set_bg(get_code_bg());

            if (!ctx->is_print_mode) {
                // Terminal mode: pre-clear line for clean overwrite
                clear_range(ctx->L.text_width);
            }
            // Print mode: render content first, pad after
        }

        // Render line content
        int32_t vis_col = 0;
        while (*p && *p != '\n') {
            // Handle ANSI escape sequences from syntax highlighter
            if (*p == '\x1b' && *(p + 1) == '[') {
                const char* seq_start = p;
                p += 2;
                while (*p && *p != 'm')
                    p++;
                if (*p == 'm')
                    p++;

                if (visible) {
                    out_str_n(seq_start, (size_t)(p - seq_start));
                    // Apply selection or code background
                    bool in_sel = selecting && src_pos >= sel_s && src_pos < sel_e;
                    set_bg(in_sel ? get_select() : get_code_bg());
                }
                continue;
            }

            // Check selection for this source position
            bool in_sel = selecting && src_pos >= sel_s && src_pos < sel_e;
            if (visible) {
                set_bg(in_sel ? get_select() : get_code_bg());
            }

            // Handle tab expansion
            if (*p == '\t') {
                int32_t tab_width = CODE_TAB_WIDTH - (vis_col % CODE_TAB_WIDTH);
                if (visible && vis_col + tab_width <= ctx->L.text_width) {
                    out_spaces(tab_width);
                }
                vis_col += tab_width;
                p++;
                src_pos++;
                continue;
            }

            // Calculate character width and byte length
            int32_t char_width = 1;
            int32_t char_bytes = 1;
            uint8_t c = (uint8_t)*p;

            if (c >= 0x80) {
                utf8proc_int32_t cp;
                utf8proc_ssize_t bytes = utf8proc_iterate((const utf8proc_uint8_t*)p, -1, &cp);
                if (bytes > 0 && cp >= 0) {
                    char_width = utf8proc_charwidth(cp);
                    if (char_width < 0)
                        char_width = 1;
                    char_bytes = (int32_t)bytes;
                }
            }

            // Check if character fits on line (first line has less space due to label)
            int32_t line_limit = first_line ? ctx->L.text_width - label_width : ctx->L.text_width;
            if (vis_col + char_width > line_limit)
                break;

            // Output character
            if (visible) {
                out_str_n(p, (size_t)char_bytes);
            }
            vis_col += char_width;
            p += char_bytes;
            src_pos += char_bytes;
        }

        if (visible) {
            // Pad remaining space and optionally render language label
            int32_t line_limit = first_line ? ctx->L.text_width - label_width : ctx->L.text_width;

            // Fill gap between code content and label (or end of line)
            if (vis_col < line_limit) {
                out_spaces(line_limit - vis_col);
            }

            // Render language label on first line (right-aligned)
            if (first_line && lang[0]) {
                set_fg(get_dim());
                out_str(lang);
                out_char(' ');
            } else if (ctx->is_print_mode && vis_col < ctx->L.text_width) {
                // Print mode without label - fill to line end
                out_spaces(ctx->L.text_width - line_limit);
            }

            reset_attrs();
            set_bg(get_bg());
        }

        first_line = false;
        rs->virtual_row++;

        if (*p == '\n') {
            p++;
            src_pos++;
        }
    }

    free(highlighted);
    free(code);

    rs->pos += cb_total_len;
    rs->col_width = 0;
    return true;
}

//! Render block math element
static bool render_block_math_element(const RenderCtx* ctx, RenderState* rs, const Block* block)
{
    size_t total_len = block->end - block->start;

    if (CURSOR_IN_RANGE(app.cursor, rs->pos, rs->pos + total_len, app.hide_cursor_syntax)) {
        render_raw_dimmed_block(ctx, rs, rs->pos + total_len);
    } else {
        // Check if math overlaps selection
        size_t sel_s, sel_e;
        get_selection(&sel_s, &sel_e);
        bool in_sel = has_selection() && rs->pos < sel_e && rs->pos + total_len > sel_s;

        // Use cached sketch if available, otherwise render and cache
        TexSketch* sketch = (TexSketch*)block->data.math.tex_sketch;
        if (!sketch) {
            size_t content_len = block->data.math.content_len;
            char* latex = malloc(content_len + 1);
            if (latex) {
                gap_copy_to(&app.text, block->data.math.content_start, content_len, latex);
                latex[content_len] = '\0';
                sketch = tex_render_string(latex, content_len, true);
                free(latex);
                ((Block*)block)->data.math.tex_sketch = sketch;
            }
        }

        if (sketch) {
            for (int32_t r = 0; r < sketch->height; r++) {
                int32_t screen_row = VROW_TO_SCREEN(&ctx->L, rs->virtual_row, app.scroll_y);
                if (IS_ROW_VISIBLE(&ctx->L, screen_row, ctx->max_row)) {
                    move_to(screen_row, ctx->L.margin + 1);
                    if (in_sel)
                        set_bg(get_select());
                    set_fg(get_accent());
                    for (int32_t c = 0; c < sketch->rows[r].count; c++) {
                        if (sketch->rows[r].cells[c].data) {
                            out_str(sketch->rows[r].cells[c].data);
                        }
                    }
                    set_fg(get_fg());
                    if (in_sel)
                        set_bg(get_bg());
                }
                rs->virtual_row++;
            }
        }
        rs->pos += total_len;
    }
    rs->col_width = 0;
    return true;
}

//! Render table element
static bool render_table_element(const RenderCtx* ctx, RenderState* rs, const Block* block)
{
    int32_t screen_row = VROW_TO_SCREEN(&ctx->L, rs->virtual_row, app.scroll_y);
    size_t total_len = block->end - block->start;
    int32_t col_count = block->data.table.col_count;
    int32_t row_count = block->data.table.row_count;

    // Get selection range
    size_t sel_s, sel_e;
    get_selection(&sel_s, &sel_e);
    bool selecting = has_selection();

    if (CURSOR_IN_RANGE(app.cursor, rs->pos, rs->pos + total_len, app.hide_cursor_syntax)) {
        render_raw_dimmed_block(ctx, rs, rs->pos + total_len);
    } else {
        int32_t* col_widths = malloc((size_t)col_count * sizeof(int32_t));
        int32_t* row_heights = calloc((size_t)row_count, sizeof(int32_t));
        InlineParseResult** cell_runs = calloc((size_t)row_count * (size_t)col_count, sizeof(InlineParseResult*));

        if (!col_widths || !row_heights || !cell_runs) {
            free(col_widths);
            free(row_heights);
            free(cell_runs);
            rs->pos += total_len;
            rs->col_width = 0;
            return true;
        }

#define CELL_RUNS(r, c) cell_runs[(r) * col_count + (c)]

        calc_table_col_widths(col_count, ctx->L.text_width, col_widths);

        // Pre-parse inline runs for all cells and calculate row heights
        for (int32_t ri = 0; ri < row_count; ri++) {
            if (ri == 1) {
                row_heights[ri] = 1;
                continue;
            }

            int32_t cells = block->data.table.row_cell_counts[ri];
            const uint32_t* cell_starts_row = block->data.table.cell_starts[ri];
            const uint16_t* cell_lens_row = block->data.table.cell_lens[ri];

            int32_t max_lines = 1;
            for (int32_t ci = 0; ci < cells && ci < col_count; ci++) {
                // Parse inline runs for this cell
                CELL_RUNS(ri, ci) = block_parse_table_cell(block, &app.text,
                    cell_starts_row[ci], cell_lens_row[ci]);
                if (CELL_RUNS(ri, ci)) {
                    int32_t cell_lines = calc_cell_wrapped_lines_with_runs(CELL_RUNS(ri, ci), col_widths[ci]);
                    if (cell_lines > max_lines)
                        max_lines = cell_lines;
                }
            }
            row_heights[ri] = max_lines;
        }

        // Top border
        screen_row = VROW_TO_SCREEN(&ctx->L, rs->virtual_row, app.scroll_y);
        render_table_hborder(&ctx->L, screen_row, ctx->max_row, col_count, col_widths, "┌", "┬", "┐");
        rs->virtual_row++;

        // Render rows
        for (int32_t ri = 0; ri < row_count; ri++) {
            if (ri == 1) {
                // Delimiter row
                screen_row = VROW_TO_SCREEN(&ctx->L, rs->virtual_row, app.scroll_y);
                render_table_hborder(&ctx->L, screen_row, ctx->max_row, col_count, col_widths, "├", "┼", "┤");
                rs->virtual_row++;
                continue;
            }

            // Use pre-parsed inline runs for this row
            int32_t cells = block->data.table.row_cell_counts[ri];

            // Track render state per cell: current run index and position within run
            int32_t* cell_run_idx = calloc((size_t)col_count, sizeof(int32_t));
            size_t* cell_run_pos = calloc((size_t)col_count, sizeof(size_t));
            if (!cell_run_idx || !cell_run_pos) {
                free(cell_run_idx);
                free(cell_run_pos);
                continue;
            }
            for (int32_t ci = 0; ci < cells && ci < col_count; ci++) {
                if (CELL_RUNS(ri, ci) && CELL_RUNS(ri, ci)->run_count > 0) {
                    cell_run_pos[ci] = CELL_RUNS(ri, ci)->runs[0].byte_start;
                }
            }

            for (int32_t line = 0; line < row_heights[ri]; line++) {
                screen_row = VROW_TO_SCREEN(&ctx->L, rs->virtual_row, app.scroll_y);

                if (IS_ROW_VISIBLE(&ctx->L, screen_row, ctx->max_row)) {
                    move_to(screen_row, ctx->L.margin + 1);
                    set_fg(get_border());
                    out_str("│");

                    for (int32_t ci = 0; ci < col_count; ci++) {
                        bool is_header = (ri == 0);
                        MdAlign align = (ci < col_count) ? block->data.table.align[ci] : MD_ALIGN_DEFAULT;
                        InlineParseResult* runs = (ci < cells) ? CELL_RUNS(ri, ci) : NULL;

                        // Find wrap point for this line using word-boundary aware algorithm
                        int32_t content_width = 0;
                        CellLineEnd line_end = find_cell_line_end(runs, cell_run_idx[ci], cell_run_pos[ci],
                            col_widths[ci], &content_width);

                        int32_t padding = col_widths[ci] - content_width;
                        if (padding < 0)
                            padding = 0;
                        int32_t left_pad = 0, right_pad = padding;

                        switch (align) {
                        case MD_ALIGN_RIGHT:
                            left_pad = padding;
                            right_pad = 0;
                            break;
                        case MD_ALIGN_CENTER:
                            left_pad = padding / 2;
                            right_pad = padding - left_pad;
                            break;
                        default:
                            break;
                        }

                        reset_attrs();
                        set_bg(get_bg());
                        out_char(' ');
                        for (int32_t p = 0; p < left_pad; p++)
                            out_char(' ');

                        if (is_header)
                            set_bold(true);
                        set_fg(get_fg());

                        // Render cell content up to the wrap point
                        while (runs && cell_run_idx[ci] < runs->run_count) {
                            const InlineRun* run = &runs->runs[cell_run_idx[ci]];

                            // Check if we've reached the wrap point
                            if (cell_run_idx[ci] > line_end.run_idx || (cell_run_idx[ci] == line_end.run_idx && cell_run_pos[ci] >= line_end.pos)) {
                                break;
                            }

                            if (run->type == RUN_DELIM) {
                                cell_run_idx[ci]++;
                                if (cell_run_idx[ci] < runs->run_count) {
                                    cell_run_pos[ci] = runs->runs[cell_run_idx[ci]].byte_start;
                                }
                                continue;
                            }

                            // Apply style for this run
                            reset_attrs();
                            set_bg(get_bg());
                            if (is_header)
                                set_bold(true);
                            if (run->style)
                                block_apply_style(run->style);
                            else
                                set_fg(get_fg());

                            // Render characters until end of run or wrap point
                            size_t run_render_end = run->byte_end;
                            if (cell_run_idx[ci] == line_end.run_idx && line_end.pos < run_render_end) {
                                run_render_end = line_end.pos;
                            }

                            while (cell_run_pos[ci] < run_render_end) {
                                // Apply selection if in range
                                bool in_sel = selecting && cell_run_pos[ci] >= sel_s && cell_run_pos[ci] < sel_e;
                                if (in_sel) {
                                    set_bg(get_select());
                                }

                                size_t next_pos;
                                grapheme_width_next(&app.text, cell_run_pos[ci], &next_pos);

                                for (size_t j = cell_run_pos[ci]; j < next_pos; j++) {
                                    out_char(gap_at(&app.text, j));
                                }
                                cell_run_pos[ci] = next_pos;

                                // Reset background after selection
                                if (in_sel) {
                                    set_bg(get_bg());
                                }
                            }

                            if (cell_run_pos[ci] >= run->byte_end) {
                                cell_run_idx[ci]++;
                                if (cell_run_idx[ci] < runs->run_count) {
                                    cell_run_pos[ci] = runs->runs[cell_run_idx[ci]].byte_start;
                                }
                            }
                        }

                        // Skip leading spaces for next line
                        if (runs) {
                            skip_cell_leading_spaces(runs, &cell_run_idx[ci], &cell_run_pos[ci]);
                        }

                        reset_attrs();
                        set_bg(get_bg());
                        for (int32_t p = 0; p < right_pad; p++)
                            out_char(' ');
                        out_char(' ');

                        set_fg(get_border());
                        out_str("│");
                    }
                }
                rs->virtual_row++;
            }

            free(cell_run_idx);
            free(cell_run_pos);

            // Row divider
            if (ri < row_count - 1 && ri != 0) {
                screen_row = VROW_TO_SCREEN(&ctx->L, rs->virtual_row, app.scroll_y);
                render_table_hborder(&ctx->L, screen_row, ctx->max_row, col_count, col_widths, "├", "┼", "┤");
                rs->virtual_row++;
            }
        }

        // Bottom border
        screen_row = VROW_TO_SCREEN(&ctx->L, rs->virtual_row, app.scroll_y);
        render_table_hborder(&ctx->L, screen_row, ctx->max_row, col_count, col_widths, "└", "┴", "┘");
        rs->virtual_row++;

        // Free pre-parsed cell runs
        for (int32_t ri = 0; ri < row_count; ri++) {
            for (int32_t ci = 0; ci < col_count; ci++) {
                if (CELL_RUNS(ri, ci)) {
                    block_parse_result_free(CELL_RUNS(ri, ci));
                }
            }
        }

#undef CELL_RUNS
        free(col_widths);
        free(row_heights);
        free(cell_runs);

        rs->pos += total_len;
    }
    rs->col_width = 0;
    return true;
}

// #endregion

// #region AI Panel Rendering

//! The chat's largest scroll offset as of its last render (0 when everything fits): the offset
//! at which it shows its oldest message, which is where a swipe down closes a sheet.
static int32_t chat_max_scroll = 0;

//! Where the last render put the model's name in the chat header (row, first and last column,
//! 0 when not drawn), so a tap on it opens the model picker.
static int32_t chat_name_row = 0, chat_name_col0 = 0, chat_name_col1 = 0;

//! Where the last render drew the chat's "also in:" line (row 0 when not drawn) and the note piece
//! it names, so a tap on that row opens it.
static struct {
    int32_t row, col0, col1; //!< Its row and columns [col0, col1)
    char path[EMBED_PATH_MAX];
    uint32_t start;
} also_in;

#if HAS_LIBAI
//! The model picker: a small list over the chat's messages, opened by tapping the model's name in
//! the header or Ctrl+L in the chat. Choosing switches at once; esc or Ctrl+L closes it.
static struct {
    bool open;
    int32_t sel; //!< The highlighted row
    int32_t first_row, rows, col0, col1; //!< Where the last render drew the rows, for taps
} chat_picker;

static void chat_picker_open(void)
{
    ai_model_info_t models[AI_MAX_MODELS];
    int32_t count = ai_models(models, AI_MAX_MODELS);
    const char* current = session_model_id();
    chat_picker.open = true;
    chat_picker.sel = 0;
    for (int32_t i = 0; i < count && i < AI_MAX_MODELS; i++)
        if (strcmp(models[i].id, current) == 0)
            chat_picker.sel = i;
    ai_models_refresh();
}

static void chat_picker_choose(int32_t index)
{
    ai_model_info_t models[AI_MAX_MODELS];
    int32_t count = ai_models(models, AI_MAX_MODELS);
    chat_picker.open = false;
    if (index >= 0 && index < count && index < AI_MAX_MODELS)
        session_pick_model(models[index].id);
}

//! A key while the picker is open. Everything goes to the list; nothing reaches the input.
static void chat_picker_key(int32_t key)
{
    int32_t count = ai_models(NULL, 0);
    if (count > AI_MAX_MODELS)
        count = AI_MAX_MODELS;
    switch (key) {
    case '\x1b':
    case 12: // Ctrl+L, the key that opened it
    case 31:
        chat_picker.open = false;
        break;
    case DAWN_KEY_UP:
    case 16:
        if (chat_picker.sel > 0)
            chat_picker.sel--;
        break;
    case DAWN_KEY_DOWN:
    case 14:
        if (chat_picker.sel < count - 1)
            chat_picker.sel++;
        break;
    case '\r':
    case '\n':
        chat_picker_choose(chat_picker.sel);
        break;
    default:
        break;
    }
}

//! Draw the picker over the chat's message rows: one row per model, the one in use marked, the
//! highlighted one on the selected-row surface.
static void render_chat_picker(int32_t top, int32_t max_rows, int32_t col, int32_t width)
{
    ai_model_info_t models[AI_MAX_MODELS];
    int32_t count = ai_models(models, AI_MAX_MODELS);
    if (count > AI_MAX_MODELS)
        count = AI_MAX_MODELS;
    const char* current = session_model_id();
    int32_t rows = count > 0 ? count : 1;
    if (rows > max_rows)
        rows = max_rows;
    if (chat_picker.sel >= count)
        chat_picker.sel = count > 0 ? count - 1 : 0;
    // Keep the highlighted row in view when the list is taller than the room.
    int32_t first = chat_picker.sel >= rows ? chat_picker.sel - rows + 1 : 0;
    chat_picker.first_row = top;
    chat_picker.rows = count > 0 ? rows : 0;
    chat_picker.col0 = col;
    chat_picker.col1 = col + width;
    for (int32_t r = 0; r < rows; r++) {
        int32_t i = first + r;
        bool sel = count > 0 && i == chat_picker.sel;
        move_to(top + r, col);
        set_bg(sel ? get_row_select_bg() : get_modal_bg());
        out_spaces(width);
        move_to(top + r, col + 1);
        if (count <= 0) {
            set_fg(get_dim());
            out_str(count < 0 ? "looking for models…" : "no models yet");
            continue;
        }
        bool in_use = strcmp(models[i].id, current) == 0;
        set_fg(get_accent());
        out_str(in_use ? "• " : "  ");
        set_fg(sel ? get_fg() : get_dim());
        const char* name = models[i].name;
        int32_t fit = chat_wrap_line(name, strlen(name), 0, width - 4);
        for (int32_t c = 0; c < fit; c++)
            out_char(name[c]);
    }
}
#endif

//! The chat header: the model's name, bold, and under it one dim line saying what the session is
//! doing ("has read this note", "waking the model · 12 s", …). Returns the rows used (2).
static int32_t render_chat_header(int32_t row, int32_t col, int32_t width, int32_t hint_room)
{
    char name[96] = "AI", line[160] = "";
#if HAS_LIBAI
    session_header(name, sizeof(name), line, sizeof(line));
#endif
    int32_t name_width = width - hint_room;
    if (name_width < 4)
        name_width = 4;
    move_to(row, col);
    set_bg(get_ai_bg());
    set_fg(get_fg());
    set_bold(true);
    int32_t fit = chat_wrap_line(name, strlen(name), 0, name_width);
    for (int32_t c = 0; c < fit; c++)
        out_char(name[c]);
    set_bold(false);
    chat_name_row = row;
    chat_name_col0 = col;
    chat_name_col1 = col + (fit > 0 ? (int32_t)utf8_display_width(name, (size_t)fit) : 0);

    move_to(row + 1, col);
    set_bg(get_ai_bg());
    set_fg(get_dim());
    fit = chat_wrap_line(line, strlen(line), 0, width);
    for (int32_t c = 0; c < fit; c++)
        out_char(line[c]);
    return 2;
}

//! Render AI panel
static void render_ai_panel(const Layout* L)
{
    int32_t padding = 1;
    int32_t prefix_len = 4;
    // As an overlay or a sheet there is no border column: the panel starts at the screen's edge.
    int32_t border_cols = (L->ai_overlay || L->ai_sheet) ? 0 : 1;
    int32_t content_start = L->ai_start_col + border_cols + padding;
    int32_t content_width = L->ai_cols - border_cols - (padding * 2);
    if (content_width < 1)
        content_width = 1;
    int32_t first_line_width = content_width - prefix_len;
    int32_t cont_line_width = content_width - prefix_len;
    if (first_line_width < 1)
        first_line_width = 1;
    if (cont_line_width < 1)
        cont_line_width = 1;

    // The chat's own rows: the whole height beside the note or as an overlay, the lower part of
    // the screen as a sheet.
    int32_t top = L->ai_top;
    int32_t bottom = L->ai_top + L->ai_rows - 1;
    bool sheet = L->ai_sheet;

    if (!app.ai_focused)
        set_dim(true);

    // Draw border and clear
    for (int32_t row = top; row <= bottom; row++) {
        move_to(row, L->ai_start_col);
        set_bg(get_ai_bg());
        set_fg(get_border());
        if (!L->ai_overlay && !sheet)
            out_str("│");
        clear_range(L->ai_cols - border_cols);
    }

    const char* hint = app.ai_thinking && app.ai_focused && !app.ai_stopping ? "esc stop" : "esc close";
    int32_t handle_col = L->ai_start_col + (L->ai_cols - 2) / 2;
    int32_t hint_room = (int32_t)strlen(hint) + 2;
    if (sheet) {
        // A sheet's edge is only the change of surface (page to surface_container) and a short
        // drag handle, then the header: the model's name and what it is doing. The stop hint
        // shares the name's row, and only while a reply is running.
        move_to(top, handle_col);
        set_bg(get_ai_bg());
        set_fg(get_dim());
        out_str("──");
        render_chat_header(top + 1, content_start, content_width, hint_room);
        if (app.ai_thinking && app.ai_focused && !app.ai_stopping) {
            int32_t hint_col = L->ai_start_col + L->ai_cols - (int32_t)strlen(hint) - padding;
            if (hint_col > content_start + 4) {
                move_to(top + 1, hint_col);
                set_bg(get_ai_bg());
                set_fg(get_dim());
                out_str(hint);
            }
        }
    } else {
        // Header: the model's name and one dim line, then the rule
        render_chat_header(1, content_start, content_width, hint_room);
        reset_attrs();
        set_bg(get_ai_bg());

        // Header separator
        move_to(3, L->ai_start_col);
        set_bg(get_ai_bg());
        set_fg(get_border());
        if (!L->ai_overlay)
            out_str("├");
        int32_t dashes = L->ai_overlay ? L->ai_cols : L->ai_cols - 2;
        for (int32_t ic = 0; ic < dashes; ic++)
            out_str("─");

        // Hint
        int32_t hint_col = L->ai_start_col + L->ai_cols - (int32_t)strlen(hint) - padding;
        if (hint_col < content_start + 5)
            hint_col = content_start + 5;
        if (hint_col + (int32_t)strlen(hint) <= L->ai_start_col + L->ai_cols) {
            move_to(1, hint_col);
            set_bg(get_ai_bg());
            set_fg(get_dim());
            out_str(hint);
        }
    }

    // Calculate input area
    int32_t input_width = content_width - 2;
    if (input_width < 1)
        input_width = 1;
    int32_t input_lines = 1, icol = 0;
    for (size_t i = 0; i < app.ai_input_len; i++) {
        if (app.ai_input[i] == '\n') {
            input_lines++;
            icol = 0;
        } else {
            icol++;
            if (icol >= input_width) {
                input_lines++;
                icol = 0;
            }
        }
    }
    if (input_lines > AI_INPUT_MAX_LINES)
        input_lines = AI_INPUT_MAX_LINES;

    int32_t input_start_row = bottom - input_lines;
    int32_t msg_area_start = sheet ? top + 3 : 5;
    int32_t msg_area_end = input_start_row - 2;
    int32_t msg_area_height = msg_area_end - msg_area_start;
    if (msg_area_height < 1)
        msg_area_height = 1;

    // Calculate message lines
    int32_t total_lines = 0;
    int32_t *msg_start_lines = NULL, *msg_line_counts = NULL;
    int32_t max_scroll = 0;

#if HAS_LIBAI
    // Turns the model no longer holds word for word (a compaction, another note's chat loaded
    // from disk) stay as dim scrollback, with one faint divider above the first one it holds.
    int32_t held_from = session_held_from();
#else
    int32_t held_from = 0;
#endif
    int32_t divider_line = -1;

    if (app.chat_count > 0) {
        msg_start_lines = malloc(sizeof(int32_t) * (size_t)app.chat_count);
        msg_line_counts = malloc(sizeof(int32_t) * (size_t)app.chat_count);
        if (!msg_start_lines || !msg_line_counts) {
            free(msg_start_lines);
            free(msg_line_counts);
            goto skip_chat;
        }

        for (int32_t i = 0; i < app.chat_count; i++) {
            if (i == held_from && held_from > 0)
                divider_line = total_lines++;
            msg_start_lines[i] = total_lines;
            ChatMessage* m = &app.chat_msgs[i];

            int32_t lines = 0;
            size_t pos = 0;
            while (pos < m->len) {
                int32_t width = (lines == 0) ? first_line_width : cont_line_width;
                int32_t chars = chat_wrap_line(m->text, m->len, pos, width);
                if (chars == 0)
                    break;
                if (chars == -1) {
                    lines++;
                    pos++;
                    continue;
                }
                lines++;
                pos += (size_t)chars;
                if (pos < m->len && (m->text[pos] == '\n' || m->text[pos] == ' '))
                    pos++;
            }
            if (lines == 0)
                lines = 1;

            msg_line_counts[i] = lines;
            total_lines += lines + 1;
        }
        if (held_from > 0 && held_from >= app.chat_count)
            divider_line = total_lines++;
    }

    int32_t thinking_line = -1;
    if (app.ai_thinking) {
        thinking_line = total_lines;
        total_lines++;
    }

    max_scroll = total_lines > msg_area_height ? total_lines - msg_area_height : 0;
    chat_max_scroll = max_scroll;
    if (app.chat_scroll < 0)
        app.chat_scroll = 0;
    if (app.chat_scroll > max_scroll)
        app.chat_scroll = max_scroll;

    int32_t first_visible = max_scroll - app.chat_scroll;
    if (first_visible < 0)
        first_visible = 0;
    int32_t last_visible = first_visible + msg_area_height;

    // Render messages
    int32_t screen_row = msg_area_start;

    for (int32_t i = 0; i <= app.chat_count && screen_row < msg_area_end; i++) {
        // The divider sits on its own line, just above the first message the model holds.
        bool divider_here = divider_line >= 0 && (i == app.chat_count ? held_from >= app.chat_count : i == held_from);
        if (divider_here && divider_line >= first_visible && divider_line < last_visible) {
            move_to(screen_row, content_start);
            set_bg(get_ai_bg());
            set_fg(get_border());
            for (int32_t c = 0; c < content_width; c++)
                out_str("─");
            screen_row++;
        }
        if (i == app.chat_count || screen_row >= msg_area_end)
            break;
        ChatMessage* m = &app.chat_msgs[i];
        int32_t msg_start = msg_start_lines[i];
        int32_t msg_lines = msg_line_counts[i];
        bool old_turn = i < held_from;

        if (msg_start + msg_lines < first_visible)
            continue;
        if (msg_start >= last_visible)
            break;

        size_t pos = 0;
        int32_t line_in_msg = 0;
        bool step_line = false; // a "✓ did this" line of the reply, and its wrapped rest

        while (pos < m->len && screen_row < msg_area_end) {
            int32_t global_line = msg_start + line_in_msg;
            bool visible = (global_line >= first_visible && global_line < last_visible);

            int32_t width = (line_in_msg == 0) ? first_line_width : cont_line_width;
            int32_t chars = chat_wrap_line(m->text, m->len, pos, width);
            if (chars == 0)
                break;
            if (chars == -1) {
                if (visible)
                    screen_row++;
                pos++;
                line_in_msg++;
                continue;
            }
            if (pos == 0 || m->text[pos - 1] == '\n')
                step_line = !m->is_user && chat_is_step_line(m->text + pos, m->len - pos);

            if (visible) {
                move_to(screen_row, content_start);
                set_bg(get_ai_bg());

                if (line_in_msg == 0) {
                    if (m->is_user) {
                        set_fg(old_turn ? get_dim() : get_accent());
                        out_str("you ");
                    } else {
                        set_fg(get_dim());
                        out_str("ai  ");
                    }
                } else {
                    out_str("    ");
                }

                set_fg(step_line || old_turn ? get_dim() : get_fg());
                if (m->is_user || step_line || old_turn) {
                    for (int32_t c = 0; c < chars; c++)
                        out_char(m->text[pos + c]);
                } else {
                    chat_print_md(m->text, pos, chars);
                }
                screen_row++;
            }

            pos += (size_t)chars;
            if (pos < m->len && (m->text[pos] == '\n' || m->text[pos] == ' '))
                pos++;
            line_in_msg++;
        }

        if (m->len == 0 && !(app.ai_thinking && !m->is_user)) {
            int32_t global_line = msg_start;
            if (global_line >= first_visible && global_line < last_visible) {
                move_to(screen_row, content_start);
                set_bg(get_ai_bg());
                if (m->is_user) {
                    set_fg(get_accent());
                    out_str("you ");
                } else {
                    set_fg(get_dim());
                    out_str("ai  ");
                }
                screen_row++;
            }
        }

        int32_t blank_line = msg_start + msg_lines;
        if (blank_line >= first_visible && blank_line < last_visible)
            screen_row++;
    }

    // Thinking indicator: a spinner, what the AI is doing now, and how long the turn has taken
    if (app.ai_thinking && thinking_line >= first_visible && thinking_line < last_visible && screen_row < msg_area_end) {
        move_to(screen_row, content_start);
        set_bg(get_ai_bg());
        set_fg(get_dim());
        out_str("ai  ");
        int64_t now = DAWN_BACKEND(app)->clock(DAWN_CLOCK_MS);
        static const char* const spinner[] = { "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏" };
        set_fg(get_accent());
        out_str(spinner[(now / 125) % 10]);
        set_fg(get_dim());
        int64_t secs = app.ai_turn_started > 0 && now > app.ai_turn_started ? (now - app.ai_turn_started) / 1000 : 0;
        char status[160];
        // A status that counts its own seconds ("waking the model · 12 s") gets no second count.
        if (strstr(app.ai_status, "·"))
            snprintf(status, sizeof(status), " %s", app.ai_status);
        else
            snprintf(status, sizeof(status), " %s %llds", app.ai_status[0] ? app.ai_status : "thinking…", (long long)secs);
        // As much as fits after the spinner; the wrap helper counts columns, not bytes.
        int32_t fit = chat_wrap_line(status, strlen(status), 0, first_line_width - 1);
        if (fit > 0)
            for (int32_t c = 0; c < fit; c++)
                out_char(status[c]);
    }

#if HAS_LIBAI
    if (chat_picker.open)
        render_chat_picker(msg_area_start, msg_area_height, content_start, content_width);
#endif

    // The pill in the chat's right padding column, over the message rows
    scrollind_show(SCROLLIND_CHAT, L->ai_start_col + L->ai_cols - 1, msg_area_start, msg_area_height,
        total_lines, msg_area_height, first_visible);

    free(msg_start_lines);
    free(msg_line_counts);

skip_chat:
    // Scroll indicator: its own row under the header, or beside a sheet's handle when it fits.
    // Where the fading pill can be drawn it says the same thing without words.
    if (max_scroll > 0 && app.chat_scroll > 0 && !scrollind_available()
        && (!sheet || content_start + 18 < handle_col)) {
        move_to(sheet ? top : 4, content_start);
        set_fg(get_dim());
        set_bg(get_ai_bg());
        out_str("↑ scroll for more");
    }

    // "also in: <title>" (dawn_embed): another note that covers the same ground, dim, on the gap
    // row between the messages and the input. Only when an index exists and a message row is left
    // above it (the header's rows stay the header's), and never in focus mode.
    // A tap on it opens that note there; the arrow (nf-md-open_in_new with a Nerd Font, which
    // spreads over the first of its two spaces) says so.
    EmbedHit rel[1];
    also_in.row = 0;
    if (!app.focus_mode && msg_area_end > msg_area_start && embed_ready() && app.session_path
        && embed_related(app.session_path, embed_related_floor(), rel, 1) == 1) {
        char line[EMBED_TITLE_MAX + 24];
        snprintf(line, sizeof(line), "%salso in: %s", app.nerd_font ? "\xf3\xb0\x8f\x8c  " : "-> ", rel[0].note_title);
        int32_t fit = chat_wrap_line(line, strlen(line), 0, content_width);
        move_to(msg_area_end, content_start);
        set_bg(get_ai_bg());
        set_fg(get_dim());
        for (int32_t c = 0; c < fit; c++)
            out_char(line[c]);
        also_in.row = msg_area_end;
        also_in.col0 = content_start;
        also_in.col1 = content_start + content_width;
        snprintf(also_in.path, sizeof(also_in.path), "%s", rel[0].path);
        also_in.start = rel[0].start;
    }

    // Input area: one rung up the tonal ladder (surface_container_high) from the chat, which is
    // what sets it apart - no separator line. The row above the prompt is the band's own padding.
    int32_t band_col = L->ai_start_col + border_cols;
    int32_t band_cols = L->ai_cols - border_cols;
    set_bg(get_input_bg());
    for (int32_t row = input_start_row - 1; row <= bottom; row++) {
        move_to(row, band_col);
        out_spaces(band_cols);
    }
    move_to(input_start_row, content_start);
    set_fg(get_accent());
    out_str("> ");
    set_fg(get_fg());

    int32_t cur_row = input_start_row, cur_col = 2;
    int32_t cursor_row = input_start_row, cursor_col = content_start + 2;

    for (size_t i = 0; i < app.ai_input_len && cur_row <= bottom; i++) {
        if (i == app.ai_input_cursor) {
            cursor_row = cur_row;
            cursor_col = content_start + cur_col;
        }

        char c = app.ai_input[i];
        if (c == '\n') {
            cur_row++;
            cur_col = 0;
            if (cur_row <= bottom) {
                move_to(cur_row, content_start);
                set_bg(get_input_bg());
            }
            continue;
        }

        if (cur_col >= input_width + 2) {
            cur_row++;
            cur_col = 0;
            if (cur_row > bottom)
                break;
            move_to(cur_row, content_start);
            set_bg(get_input_bg());
        }

        out_char(c);
        cur_col++;
    }

    if (app.ai_input_cursor >= app.ai_input_len) {
        cursor_row = cur_row;
        cursor_col = content_start + cur_col;
    }

    if (app.ai_focused) {
        move_to(cursor_row, cursor_col);
        cursor_visible(true);
        status_cursor(cursor_row, cursor_col);
    }
    reset_attrs();
}

// #endregion

// #region Status Bar Rendering

//! Render status bar
static void render_status_bar(const Layout* L)
{
    int32_t words = count_words(&app.text);
    int32_t status_left = L->margin + 1;
    int32_t status_right = L->margin + L->text_width;

    move_to(app.rows, 1);
    for (int32_t i = 0; i < L->text_area_cols; i++)
        out_char(' ');

    move_to(app.rows, status_left);
    set_fg(get_dim());

    bool need_sep = false;

    if (app.timer_mins > 0 && app.timer_on) {
        int32_t rem = timer_remaining();
        float prog = (float)rem / (app.timer_mins * 60.0f);
        DawnColor tc = color_lerp(get_dim(), get_accent(), prog);
        set_fg(tc);
        if (app.timer_paused)
            out_str("⏸ ");
        char time_buf[16];
        snprintf(time_buf, sizeof(time_buf), "%d:%02d", rem / 60, rem % 60);
        out_str(time_buf);
        need_sep = true;
    }

    if (need_sep) {
        set_fg(get_border());
        out_str(" · ");
    }
    set_fg(get_dim());
    char words_buf[32];
    snprintf(words_buf, sizeof(words_buf), "%d word%s", words, words == 1 ? "" : "s");
    out_str(words_buf);

    // The note could not be written; it stays here until a save gets through.
    if (app.save_failed) {
        set_fg(get_border());
        out_str(" · ");
        set_fg(get_accent());
        out_str("not saved");
    }

    // The note changed elsewhere and the writer chose to keep editing: nothing is written to it
    // until a save finds it unchanged or the dialog is answered.
    if (app.save_paused) {
        set_fg(get_border());
        out_str(" · ");
        set_fg(get_accent());
        out_str("saving paused");
    }

    if (app.focus_mode) {
        set_fg(get_border());
        out_str(" · ");
        set_fg(get_accent());
        out_str("focus");
    }

    if (has_selection()) {
        size_t sel_s, sel_e;
        get_selection(&sel_s, &sel_e);
        set_fg(get_border());
        out_str(" · ");
        set_fg(get_dim());
        char sel_buf[32];
        snprintf(sel_buf, sizeof(sel_buf), "%zu sel", sel_e - sel_s);
        out_str(sel_buf);
    }

    // Right-aligned notice (dawn_notice), in place of the usual hints: both live in the same
    // corner and a notice is meant to be seen, so it takes priority while it's showing. Nothing in
    // focus mode - focus mode already hides everything else here.
    const char* notice_text;
    NoticeKind notice_kind;
    float notice_fresh;
    if (!app.focus_mode && notice_current(&notice_text, &notice_kind, &notice_fresh)) {
        DawnColor c = (notice_kind == NOTICE_ERROR)
            ? get_error_color() // stays at full strength until notice_ack(), no fade
            : color_lerp(get_bg(), get_dim(), notice_fresh);
        // A notice longer than the corner is cut at a character boundary and ends in "…",
        // rather than not showing at all; the activity list keeps the whole text.
        // What the left side actually used (words, timer, "not saved", selection) plus a gap.
        int32_t left_end = status_left + (int32_t)strlen(words_buf) + 2
            + (app.timer_mins > 0 && app.timer_on ? 10 : 0) + (app.save_failed ? 12 : 0) + (app.save_paused ? 16 : 0)
            + (has_selection() ? 12 : 0);
        int32_t room = status_right - left_end;
        size_t len = strlen(notice_text);
        if (room > 1 && len > (size_t)room) {
            len = (size_t)room - 1;
            while (len > 0 && ((unsigned char)notice_text[len] & 0xC0) == 0x80)
                len--;
            while (len > 0 && notice_text[len - 1] == ' ')
                len--;
        }
        bool cut = len < strlen(notice_text);
        int32_t notice_col = status_right - (int32_t)len - (cut ? 1 : 0) + 1;
        if (room > 1 && notice_col >= left_end) {
            move_to(app.rows, notice_col);
            set_fg(c);
            out_str_n(notice_text, len);
            if (cut)
                out_str("…");
        }
        return;
    }

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

    // Right side hints
    char hints[64] = "";
    int32_t hints_len = 0;

    if (app.timer_on) {
        hints_len += snprintf(hints + hints_len, sizeof(hints) - (size_t)hints_len, "^P");
    }
#if HAS_LIBAI
    if (app.ai_ready) {
        if (hints_len > 0)
            hints_len += snprintf(hints + hints_len, sizeof(hints) - (size_t)hints_len, " · ");
        hints_len += snprintf(hints + hints_len, sizeof(hints) - (size_t)hints_len, "^/");
    }
#endif
    if (hints_len > 0)
        hints_len += snprintf(hints + hints_len, sizeof(hints) - (size_t)hints_len, " · ");
    snprintf(hints + hints_len, sizeof(hints) - (size_t)hints_len, "esc");

    int32_t hints_col = status_right - (int32_t)strlen(hints) + 1;
    if (hints_col > status_left + 20) {
        move_to(app.rows, hints_col);
        set_fg(get_dim());
        out_str(hints);
    }
}

// #endregion

// #region Inline Element Rendering

//! Render raw dimmed content for cursor-in-element case (common helper for inline elements)
//! Shows source text dimmed, skips all replacements (typo, emoji, entity)
static void render_cursor_in_element(const RenderCtx* ctx, RenderState* rs, size_t element_len)
{
    set_fg(get_dim());
    size_t end_pos = rs->pos + element_len;
    while (rs->pos < end_pos && rs->pos < ctx->len) {
        track_cursor(ctx, rs);
        wrap_and_render_grapheme_raw(ctx, rs);
    }
    set_fg(get_fg());
}

//! Render inline math element
static bool render_inline_math(const RenderCtx* ctx, RenderState* rs, const InlineRun* run)
{
    size_t math_total = run->byte_end - run->byte_start;
    size_t content_start = run->data.math.content_start;
    size_t content_len = run->data.math.content_len;
    int32_t screen_row = VROW_TO_SCREEN(&ctx->L, rs->virtual_row, app.scroll_y);

    if (CURSOR_IN(rs->pos, rs->pos + math_total)) {
        render_cursor_in_element(ctx, rs, math_total);
        return true;
    }

    // Check if math overlaps selection
    size_t sel_s, sel_e;
    get_selection(&sel_s, &sel_e);
    bool in_sel = has_selection() && rs->pos < sel_e && rs->pos + math_total > sel_s;

    TexSketch* sketch = (TexSketch*)run->data.math.tex_sketch;
    if (!sketch) {
        char* latex = malloc(content_len + 1);
        if (latex) {
            for (size_t i = 0; i < content_len; i++) {
                latex[i] = gap_at(&app.text, content_start + i);
            }
            latex[content_len] = '\0';
            sketch = tex_render_inline(latex, content_len, true);
            free(latex);
            // Cache for later use
            ((InlineRun*)run)->data.math.tex_sketch = sketch;
        }
    }

    if (sketch && sketch->height == 1) {
        rs->pos += math_total;
        if (IS_ROW_VISIBLE(&ctx->L, screen_row, ctx->max_row)) {
            if (in_sel)
                set_bg(get_select());
            set_fg(get_accent());
            for (int32_t c = 0; c < sketch->rows[0].count; c++) {
                if (sketch->rows[0].cells[c].data) {
                    out_str(sketch->rows[0].cells[c].data);
                }
            }
            set_fg(get_fg());
            if (in_sel)
                set_bg(get_bg());
        }
        rs->col_width += sketch->width;
        return true;
    } else if (sketch && sketch->height > 1) {
        // Multi-row inline math - position at current column
        int32_t start_col = ctx->L.margin + 1 + rs->col_width;
        rs->pos += math_total;
        for (int32_t r = 0; r < sketch->height; r++) {
            screen_row = VROW_TO_SCREEN(&ctx->L, rs->virtual_row, app.scroll_y);
            if (IS_ROW_VISIBLE(&ctx->L, screen_row, ctx->max_row)) {
                move_to(screen_row, start_col);
                if (in_sel)
                    set_bg(get_select());
                set_fg(get_accent());
                for (int32_t c = 0; c < sketch->rows[r].count; c++) {
                    if (sketch->rows[r].cells[c].data) {
                        out_str(sketch->rows[r].cells[c].data);
                    }
                }
                set_fg(get_fg());
                if (in_sel)
                    set_bg(get_bg());
            }
            rs->virtual_row++;
        }
        rs->col_width += sketch->width;
        return true;
    }

    rs->pos += math_total;
    if (IS_ROW_VISIBLE(&ctx->L, screen_row, ctx->max_row)) {
        set_fg(get_accent());
        set_italic(true);
        for (size_t i = 0; i < content_len; i++)
            out_char(gap_at(&app.text, content_start + i));
        reset_attrs();
        set_bg(get_bg());
        set_fg(get_fg());
    }
    rs->col_width += (int32_t)content_len;
    return true;
}

//! Render link element
//! Only these schemes are worth an OSC 8 wrapper: they're what the launcher's confirm strip
//! knows how to act on. Anything else (a bare footnote-style reference, a relative path, "ftp:",
//! ...) renders as before - underlined and accent-colored, just not click-through.
static bool url_scheme_is_linkable(const char* url)
{
    return strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0
        || strncmp(url, "mailto:", 7) == 0;
}

static bool render_link(const RenderCtx* ctx, RenderState* rs, const InlineRun* run)
{
    size_t link_total = run->byte_end - run->byte_start;
    int32_t screen_row = VROW_TO_SCREEN(&ctx->L, rs->virtual_row, app.scroll_y);
    int32_t text_scale = GET_LINE_SCALE(rs->line_style);

    if (CURSOR_IN(rs->pos, rs->pos + link_total)) {
        render_cursor_in_element(ctx, rs, link_total);
        return true;
    }

    char url[1024];
    size_t ulen = run->data.link.url_len < sizeof(url) - 1 ? run->data.link.url_len : sizeof(url) - 1;
    gap_copy_to(&app.text, run->data.link.url_start, ulen, url);
    url[ulen] = '\0';
    rs->pos += link_total;

    // OSC 8 has zero width (out_char() is never called for it), so it never disturbs col_width
    // or the wrap math below; it also costs nothing extra to skip when the backend can't use it.
    bool use_osc8 = dawn_ctx_has(&app.ctx, DAWN_CAP_HYPERLINKS) && url_scheme_is_linkable(url);

    size_t link_pos = run->data.link.text_start;
    size_t link_end = run->data.link.text_start + run->data.link.text_len;
    bool in_code = false;
    bool link_started = false;

    while (link_pos < link_end) {
        screen_row = VROW_TO_SCREEN(&ctx->L, rs->virtual_row, app.scroll_y);
        int32_t available_width = ctx->L.text_width / text_scale;

        // Check if we need to wrap before outputting next char
        if (rs->col_width >= available_width) {
            // Close hyperlink before wrapping
            if (link_started && IS_ROW_VISIBLE(&ctx->L, screen_row, ctx->max_row)) {
                clear_underline();
                reset_attrs();
                if (use_osc8)
                    DAWN_BACKEND(app)->link_end();
                set_bg(get_bg());
            }
            link_started = false;

            // Wrap to next line
            rs->virtual_row += text_scale;
            rs->col_width = 0;
            screen_row = VROW_TO_SCREEN(&ctx->L, rs->virtual_row, app.scroll_y);

            // Skip leading spaces on wrapped line
            while (link_pos < link_end && gap_at(&app.text, link_pos) == ' ') {
                link_pos++;
            }
            if (link_pos >= link_end)
                break;

            // Move cursor to start of new line
            if (IS_ROW_VISIBLE(&ctx->L, screen_row, ctx->max_row)) {
                move_to(screen_row, ctx->L.margin + 1);
            }
        }

        if (IS_ROW_VISIBLE(&ctx->L, screen_row, ctx->max_row)) {
            // Start/restart hyperlink on this line
            if (!link_started) {
                if (use_osc8)
                    DAWN_BACKEND(app)->link_begin(url);
                set_underline(UNDERLINE_STYLE_SINGLE);
                set_underline_color(get_underline_color_token());
                set_fg(get_link_color());
                if (in_code)
                    set_dim(true);
                link_started = true;
            }

            char ch = gap_at(&app.text, link_pos);
            if (ch == '`') {
                in_code = !in_code;
                link_pos++;
                set_dim(in_code);
                continue;
            }

            size_t next_pos;
            int32_t gw = grapheme_width_next(&app.text, link_pos, &next_pos);
            for (size_t j = link_pos; j < next_pos && j < link_end; j++) {
                out_char(gap_at(&app.text, j));
            }
            rs->col_width += gw;
            link_pos = next_pos;
        } else {
            char ch = gap_at(&app.text, link_pos);
            if (ch == '`') {
                in_code = !in_code;
                link_pos++;
                continue;
            }

            size_t next_pos;
            int32_t gw = grapheme_width_next(&app.text, link_pos, &next_pos);
            rs->col_width += gw;
            link_pos = next_pos;
        }
    }

    // Close hyperlink at end
    screen_row = VROW_TO_SCREEN(&ctx->L, rs->virtual_row, app.scroll_y);
    if (link_started && IS_ROW_VISIBLE(&ctx->L, screen_row, ctx->max_row)) {
        clear_underline();
        reset_attrs();
        if (use_osc8)
            DAWN_BACKEND(app)->link_end();
        set_bg(get_bg());
        set_fg(get_fg());
    }
    return true;
}

//! Render footnote reference
static bool render_footnote_ref(const RenderCtx* ctx, RenderState* rs, const InlineRun* run)
{
    size_t fnref_total = run->byte_end - run->byte_start;
    size_t id_start = run->data.footnote.id_start;
    size_t id_len = run->data.footnote.id_len;
    int32_t screen_row = VROW_TO_SCREEN(&ctx->L, rs->virtual_row, app.scroll_y);

    if (CURSOR_IN(rs->pos, rs->pos + fnref_total)) {
        render_cursor_in_element(ctx, rs, fnref_total);
    } else {
        rs->pos += fnref_total;
        if (IS_ROW_VISIBLE(&ctx->L, screen_row, ctx->max_row)) {
            set_fg(get_accent());
            out_str("[");
            for (size_t i = 0; i < id_len; i++)
                out_char(gap_at(&app.text, id_start + i));
            out_str("]");
            set_fg(get_fg());
        }
        rs->col_width += (int32_t)id_len + 2;
    }
    return true;
}

//! Render heading ID (hidden unless cursor is inside)
static bool render_heading_id(const RenderCtx* ctx, RenderState* rs, const InlineRun* run)
{
    size_t total = run->byte_end - run->byte_start;
    if (CURSOR_IN(rs->pos, rs->pos + total)) {
        render_cursor_in_element(ctx, rs, total);
    } else {
        rs->pos += total;
    }
    return true;
}

//! Render emoji shortcode
static bool render_emoji(const RenderCtx* ctx, RenderState* rs, const InlineRun* run)
{
    size_t total = run->byte_end - run->byte_start;
    int32_t screen_row = VROW_TO_SCREEN(&ctx->L, rs->virtual_row, app.scroll_y);

    if (CURSOR_IN(rs->pos, rs->pos + total)) {
        render_cursor_in_element(ctx, rs, total);
    } else {
        rs->pos += total;
        if (IS_ROW_VISIBLE(&ctx->L, screen_row, ctx->max_row)) {
            out_str(run->data.emoji.emoji);
        }
        rs->col_width += 2;
    }
    return true;
}

// #endregion

// #region Line Prefix Rendering

//! Render line prefix elements using pre-parsed Block data
//! Only renders prefix on FIRST line of block (rs->pos == block->start)

//! The task box and its trailing space, two cells. In Termux Launcher's terminal the bundled Nerd
//! Font draws nf-md-checkbox_blank_outline / nf-md-checkbox_marked (U+F0131 / U+F0132) two cells
//! wide over that space, where ☐/☑ come out small; a terminal without the font would show a
//! missing-glyph box, so the icons are only used when app.nerd_font says the font is there.
static const char* task_glyph(bool checked)
{
    if (app.nerd_font) // the icon spreads over the first space; the second keeps a gap to the text
        return checked ? "\xf3\xb0\x84\xb2  " : "\xf3\xb0\x84\xb1  ";
    return checked ? "☑ " : "☐ ";
}

//! How many cells task_glyph() takes: the box and its gap.
static inline int32_t task_glyph_cols(void)
{
    return app.nerd_font ? 3 : 2;
}

static void render_line_prefixes(const RenderCtx* ctx, RenderState* rs,
    const Block* block, size_t line_end,
    size_t* seg_end, int32_t* seg_width)
{
    size_t len = ctx->len;
    int32_t text_scale = GET_LINE_SCALE(rs->line_style);

    // Only render prefix on the first line of the block
    bool is_first_line = (rs->pos == block->start);

    switch (block->type) {
    case BLOCK_LIST_ITEM: {
        if (!is_first_line)
            break;

        int32_t task_state = block->data.list.task_state;
        int32_t list_indent = block->data.list.indent;
        size_t content_start = block->data.list.content_start;

        if (task_state > 0) {
            // Task list item
            if (CURSOR_IN(rs->pos, content_start)) {
                render_raw_prefix(ctx, rs, content_start);
            } else {
                rs->pos = content_start;
                set_fg(get_dim());
                for (int32_t i = 0; i < list_indent; i++) {
                    out_char(' ');
                    rs->col_width++;
                }
                out_str(task_glyph(task_state == 2));
                set_fg(get_fg());
                rs->col_width += task_glyph_cols();
            }
        } else {
            // Regular list item
            if (CURSOR_IN(rs->pos, content_start)) {
                render_raw_prefix(ctx, rs, content_start);
            } else {
                set_fg(get_dim());
                for (int32_t i = 0; i < list_indent; i++) {
                    out_char(' ');
                    rs->col_width++;
                }
                if (block->data.list.list_type == 1) {
                    out_str("• ");
                    rs->col_width += 2;
                } else {
                    // Parse number from source for ordered list
                    size_t p = rs->pos + list_indent;
                    int32_t num = 0;
                    while (p < len && gap_at(&app.text, p) >= '0' && gap_at(&app.text, p) <= '9') {
                        num = num * 10 + (gap_at(&app.text, p) - '0');
                        p++;
                    }
                    char num_buf[16];
                    int32_t printed = snprintf(num_buf, sizeof(num_buf), "%d. ", num);
                    out_str(num_buf);
                    rs->col_width += printed;
                }
                set_fg(get_fg());
                rs->pos = content_start;
            }
        }
        recalc_wrap_seg(ctx->L.text_width, rs->col_width, rs->pos, line_end, seg_end, seg_width);
        break;
    }

    case BLOCK_HEADER: {
        if (!is_first_line) {
            // Wrapped header line - just recalculate with scale
            int32_t header_scale = HAS_CAP(DAWN_CAP_TEXT_SIZING) ? text_scale : 1;
            int32_t available = (ctx->L.text_width - rs->col_width) / header_scale;
            if (available < 1)
                available = 1;
            *seg_end = gap_find_wrap_point(&app.text, rs->pos, line_end, available, seg_width);
            break;
        }

        size_t content_start = block->data.header.content_start;
        // Check if cursor is anywhere in the header block (same logic as render_header_element)
        size_t header_end = block->end;
        if (header_end > 0 && header_end <= ctx->len && gap_at(&app.text, header_end - 1) == '\n')
            header_end--;
        size_t header_check_end = header_end;
        if (header_end < ctx->len && gap_at(&app.text, header_end) == '\n')
            header_check_end++;
        else if (header_end >= ctx->len)
            header_check_end++;

        if (CURSOR_IN_RANGE(app.cursor, block->start, header_check_end, app.hide_cursor_syntax)) {
            MdFracScale frac = block_get_frac_scale(rs->line_style);
            current_text_scale = frac.scale;
            current_frac_num = frac.num;
            current_frac_denom = frac.denom;
            render_raw_prefix(ctx, rs, content_start);
        } else {
            rs->pos = content_start;
        }
        int32_t header_scale = HAS_CAP(DAWN_CAP_TEXT_SIZING) ? text_scale : 1;
        int32_t available = (ctx->L.text_width - rs->col_width) / header_scale;
        if (available < 1)
            available = 1;
        *seg_end = gap_find_wrap_point(&app.text, rs->pos, line_end, available, seg_width);
        break;
    }

    case BLOCK_BLOCKQUOTE: {
        int32_t quote_level = block->data.quote.level;

        // For blockquotes, each logical line may have "> " prefix
        // Check how many levels of "> " are at current position
        size_t skip_pos = rs->pos;
        int32_t found_level = 0;
        while (skip_pos < len) {
            if (gap_at(&app.text, skip_pos) == '>') {
                skip_pos++;
                if (skip_pos < len && gap_at(&app.text, skip_pos) == ' ') {
                    skip_pos++;
                }
                found_level++;
            } else {
                break;
            }
        }

        // Use found_level for this line, fall back to block's level for wrapped lines
        int32_t render_level = found_level > 0 ? found_level : quote_level;

        if (found_level > 0) {
            // This line has "> " prefix(es) to skip
            if (CURSOR_IN(rs->pos, skip_pos)) {
                render_raw_prefix(ctx, rs, skip_pos);
            } else {
                rs->pos = skip_pos;
            }
        }

        // Always show quote bars
        set_fg(get_accent());
        for (int32_t i = 0; i < render_level; i++) {
            out_str("┃ ");
            rs->col_width += 2;
        }
        set_fg(get_fg());
        set_italic(true);
        recalc_wrap_seg(ctx->L.text_width, rs->col_width, rs->pos, line_end, seg_end, seg_width);
        break;
    }

    case BLOCK_FOOTNOTE_DEF: {
        if (!is_first_line)
            break;

        size_t fn_id_start = block->data.footnote.id_start;
        size_t fn_id_len = block->data.footnote.id_len;
        size_t content_start = block->data.footnote.content_start;
        if (CURSOR_IN(rs->pos, content_start)) {
            render_raw_prefix(ctx, rs, content_start);
        } else {
            rs->pos = content_start;
            set_fg(get_accent());
            out_str("[");
            for (size_t i = 0; i < fn_id_len; i++)
                out_char(gap_at(&app.text, fn_id_start + i));
            out_str("] ");
            set_fg(get_fg());
            rs->col_width += (int32_t)fn_id_len + 3;
        }
        recalc_wrap_seg(ctx->L.text_width, rs->col_width, rs->pos, line_end, seg_end, seg_width);
        break;
    }

    default:
        // BLOCK_PARAGRAPH, BLOCK_CODE, BLOCK_MATH, BLOCK_TABLE, BLOCK_IMAGE, BLOCK_HR
        // have no line prefixes to render
        break;
    }
}

// #endregion

// #region Plain Mode Rendering

static WrapResult plain_wrap_cache = { 0 };
static size_t plain_wrap_text_len = 0;
static int32_t plain_wrap_width = 0;

static void render_writing_plain(void)
{
    set_bg(get_bg());
    cursor_home();

    for (int32_t r = 0; r < app.rows; r++) {
        move_to(r + 1, 1);
        clear_line();
    }

    Layout L = calc_layout();
    size_t sel_s, sel_e;
    get_selection(&sel_s, &sel_e);
    size_t len = gap_len(&app.text);

    if (plain_wrap_cache.lines == NULL || plain_wrap_text_len != len || plain_wrap_width != L.text_width) {
        if (plain_wrap_cache.lines)
            wrap_free(&plain_wrap_cache);
        wrap_init(&plain_wrap_cache);
        wrap_text(&app.text, L.text_width, &plain_wrap_cache);
        plain_wrap_text_len = len;
        plain_wrap_width = L.text_width;
    }
    WrapResult* wr = &plain_wrap_cache;

    int32_t cursor_vrow = 0, cursor_col_in_line = 0;
    for (int32_t i = 0; i < wr->count; i++) {
        if (app.cursor >= wr->lines[i].start && app.cursor <= wr->lines[i].end) {
            cursor_vrow = i;
            cursor_col_in_line = gap_display_width(&app.text, wr->lines[i].start, app.cursor);
            break;
        }
        if (app.cursor < wr->lines[i].start) {
            cursor_vrow = i > 0 ? i - 1 : 0;
            break;
        }
        cursor_vrow = i;
    }
    if (app.cursor >= len && wr->count > 0) {
        cursor_vrow = wr->count - 1;
        cursor_col_in_line = gap_display_width(&app.text, wr->lines[cursor_vrow].start, len);
    }

    // Adjust scroll with margin. While touch-detached (dawn_touch), stay put and just clamp to
    // the document's own bounds, the same as render_writing() does for WYSIWYG mode.
    int32_t scroll_margin = L.text_height > 10 ? 3 : 1;
    if (app.view_detached) {
        int32_t max_scroll = wr->count - L.text_height + scroll_margin;
        if (max_scroll < 0)
            max_scroll = 0;
        if (app.scroll_y > max_scroll)
            app.scroll_y = max_scroll;
    } else {
        if (cursor_vrow < app.scroll_y + scroll_margin) {
            app.scroll_y = cursor_vrow - scroll_margin;
        }
        if (cursor_vrow >= app.scroll_y + L.text_height - scroll_margin) {
            app.scroll_y = cursor_vrow - L.text_height + scroll_margin + 1;
        }
    }
    if (app.scroll_y < 0)
        app.scroll_y = 0;

    int32_t cursor_screen_row = L.top_margin, cursor_screen_col = L.margin + 1;
    set_fg(get_fg());

    for (int32_t i = app.scroll_y; i < wr->count && (i - app.scroll_y) < L.text_height; i++) {
        int32_t screen_row = L.top_margin + (i - app.scroll_y);
        move_to(screen_row, L.margin + 1);
        WrapLine* line = &wr->lines[i];
        size_t p = line->start;
        int32_t col = 0;

        while (p < line->end) {
            if (p == app.cursor) {
                cursor_screen_row = screen_row;
                cursor_screen_col = L.margin + 1 + col;
            }
            bool in_sel = app.selecting && p >= sel_s && p < sel_e;
            if (in_sel)
                set_bg(get_select());
            size_t next_pos;
            int32_t w = grapheme_width_next(&app.text, p, &next_pos);
            for (size_t j = p; j < next_pos; j++)
                out_char(gap_at(&app.text, j));
            if (in_sel)
                set_bg(get_bg());
            col += w;
            p = next_pos;
        }
        if (app.cursor == line->end && i == cursor_vrow) {
            cursor_screen_row = screen_row;
            cursor_screen_col = L.margin + 1 + col;
        }
        if (line->ends_with_split) {
            set_fg(get_dim());
            out_char('-');
            set_fg(get_fg());
        }
    }

    if (app.cursor >= len && wr->count > 0 && cursor_vrow <= app.scroll_y + L.text_height - 1) {
        cursor_screen_row = L.top_margin + (cursor_vrow - app.scroll_y);
        cursor_screen_col = L.margin + 1 + cursor_col_in_line;
    }

    if (L.margin > 0)
        scrollind_show(SCROLLIND_NOTE, L.text_area_cols, L.top_margin, L.text_height, wr->count,
            L.text_height, app.scroll_y);

    // The status panel's corner (dawn_status): the two blank rows under the text.
    status_area(app.rows - 2, app.rows - 1, L.text_area_cols, status_panel_cols(&L), false);
    move_to(cursor_screen_row, cursor_screen_col);
    cursor_visible(true);
    status_cursor(cursor_screen_row, cursor_screen_col);
}

//! Set by embed_poll(): the meaning index or a query changed, so the "by meaning" group ranks again.
static bool embed_news = false;

//! After embed_poll() said something moved: a meaning index that has just stopped for good (a
//! permanent error) says why once, as a sticky notice, each time it enters that state.
static void embed_tell_failure(void)
{
    static bool failed = false;
    EmbedStatus st;
    embed_status(&st);
    bool now = st.phase == EMBED_PHASE_FAILED;
    if (now && !failed) {
        char msg[EMBED_ERROR_MAX + 24];
        snprintf(msg, sizeof(msg), "meaning index: %s", st.error[0] ? st.error : "failed");
        notice_post(NOTICE_ERROR, msg);
    }
    failed = now;
}

//! A "by meaning" row's text: the piece's heading, or the first non-blank line of slice (the
//! piece's text, NUL-terminated), cut to fit out at a UTF-8 boundary.
static void meaning_row_text(char* out, size_t cap, const char* heading, const char* slice)
{
    const char* from = heading;
    size_t len = strlen(from);
    if (len == 0 && slice) {
        from = slice;
        while (*from == '\n' || *from == ' ')
            from++;
        len = strcspn(from, "\n");
    }
    if (len >= cap) {
        len = cap - 1;
        while (len > 0 && ((unsigned char)from[len] & 0xC0) == 0x80)
            len--; // not in the middle of a character
    }
    memcpy(out, from, len);
    out[len] = '\0';
}

//! Another note's body as load_file_for_editing() puts it in app.text (frontmatter removed, LF
//! endings), malloc'd and NUL-terminated, or NULL when the file can't be read.
static char* meaning_note_body(const char* path, size_t* len)
{
    size_t n = 0;
    char* raw = DAWN_BACKEND(app)->read_file(path, &n);
    if (!raw)
        return NULL;
    size_t off = embed_body_offset(raw, n);
    size_t body = n - off;
    memmove(raw, raw + off, body);
    body = embed_normalize_newlines(raw, body);
    raw[body] = '\0';
    *len = body;
    return raw;
}

//! Rank Ctrl+S's "by meaning" group for the current query: pieces of this note close in meaning,
//! minus pieces the user has edited since they were indexed and pieces an exact match already
//! covers, then the best piece of each other note, minus ones whose file changed since. Asks again
//! only when the query, the exact results or the index changed; a query still being embedded
//! shows nothing until embed_poll() says it landed.
static void refresh_meaning(SearchState* s, bool exact_changed)
{
    if (!embed_ready() || s->query_len < 3 || !app.session_path) {
        s->meaning_count = 0;
        s->meaning_here = 0;
        s->meaning_query[0] = '\0';
        return;
    }
    if (!embed_news && !exact_changed && strcmp(s->meaning_query, s->query) == 0)
        return;
    embed_news = false;
    snprintf(s->meaning_query, sizeof(s->meaning_query), "%s", s->query);
    EmbedSearchOpts opts = { .only_path = app.session_path, .min_score = embed_search_floor(), .near_best = EMBED_NEAR_BEST };
    int32_t n = 0;
    if (embed_search(s->query, (size_t)s->query_len, &opts, s->meaning, SEARCH_MEANING_HERE, &n) != EMBED_READY)
        n = 0;
    int32_t kept = 0;
    size_t doc_len = gap_len(&app.text);
    for (int32_t i = 0; i < n; i++) {
        const EmbedHit* h = &s->meaning[i];
        if ((size_t)h->start + h->len > doc_len)
            continue;
        char* slice = gap_substr(&app.text, h->start, h->start + h->len);
        bool ok = slice && embed_hit_matches(h, slice, h->len);
        for (int32_t k = 0; ok && k < s->count; k++)
            if (s->results[k].pos >= h->start && s->results[k].pos < (size_t)h->start + h->len)
                ok = false;
        if (ok) {
            meaning_row_text(s->meaning_text[kept], sizeof(s->meaning_text[kept]), h->heading, slice);
            s->meaning[kept++] = *h;
        }
        free(slice);
    }
    s->meaning_here = kept;

    // Other notes, one row each: the piece is read back from its file to check it is still there.
    EmbedHit other[SEARCH_MEANING_ELSEWHERE];
    EmbedSearchOpts elsewhere = {
        .only_path = NULL, .exclude_path = app.session_path, .min_score = embed_search_floor(), .near_best = EMBED_NEAR_BEST, .one_per_note = true
    };
    if (embed_search(s->query, (size_t)s->query_len, &elsewhere, other, SEARCH_MEANING_ELSEWHERE, &n) != EMBED_READY)
        n = 0;
    for (int32_t i = 0; i < n && kept < SEARCH_MEANING_MAX; i++) {
        const EmbedHit* h = &other[i];
        size_t body_len = 0;
        char* body = meaning_note_body(h->path, &body_len);
        if (body && (size_t)h->start + h->len <= body_len && embed_hit_matches(h, body + h->start, h->len)) {
            body[(size_t)h->start + h->len] = '\0';
            meaning_row_text(s->meaning_text[kept], sizeof(s->meaning_text[kept]), h->heading, body + h->start);
            s->meaning[kept++] = *h;
        }
        free(body);
    }
    s->meaning_count = kept;
    if (s->selected >= s->count + kept)
        s->selected = s->count + kept > 0 ? s->count + kept - 1 : 0;
}

//! Main render dispatch
static void render(void)
{
    static AppMode last_mode = (AppMode)-1;
    if (app.mode != last_mode) {
        update_title();
        last_mode = app.mode;
    }

    sync_begin();
    cursor_visible(false);
    scrollind_frame_begin();

    switch (app.mode) {
    case MODE_WELCOME:
        render_welcome();
        break;
    case MODE_TIMER_SELECT:
        render_timer_select();
        break;
    case MODE_STYLE:
        render_style_select();
        break;
    case MODE_HISTORY:
        render_history();
        break;
    case MODE_WRITING:
        render_writing();
        break;
    case MODE_FINISHED:
        render_finished();
        break;
    case MODE_FM_EDIT:
        if (app.prev_mode == MODE_WRITING)
            render_writing();
        else
            render_clear();
        render_fm_edit();
        break;
    case MODE_HELP:
        render_writing();
        render_help();
        break;
    case MODE_BLOCK_EDIT:
        render_writing();
        render_block_edit();
        break;
    case MODE_TOC:
        render_writing();
        render_toc();
        break;
    case MODE_SEARCH: {
        SearchState* search = (SearchState*)app.search_state;
        bool found = search_find(&app.text, search, DAWN_BACKEND(app)->clock(DAWN_CLOCK_MS));
        refresh_meaning(search, found);
        render_writing();
        render_search();
    } break;
    case MODE_CONFLICT:
        render_writing();
        render_conflict();
        break;
    }

    // The status panel slides out over the note (not in focus mode) or the welcome screen, never
    // over a dialog.
    status_frame(app.mode == MODE_WELCOME || (app.mode == MODE_WRITING && !app.focus_mode));
    scrollind_frame_end();
    sync_end();
    out_flush();
}

// #endregion

// #region Session Management

//! Start an empty note at path (taken over), with no frontmatter. Nothing is written until the
//! first edit is saved.
static void begin_session(char* path)
{
    gap_free(&app.text);
    gap_init(&app.text, 4096);

    free(app.session_path);
    app.session_path = path;
    image_set_base_dir_for_note(path);
    note_disk_begin_absent(); // no file yet: the first save creates it, unless one appears meanwhile

    fm_free(app.frontmatter);
    app.frontmatter = NULL;
    app.write_fm = false;

    app.cursor = 0;
    app.scroll_y = 0;
    app.view_detached = false;
    app.selecting = false;
    undo_reset();
    app.dirty = false;
    app.save_failed = false;
    app.timer_done = false;
    app.timer_on = (app.timer_mins > 0);
    if (app.timer_on) {
        app.timer_start = DAWN_BACKEND(app)->clock(DAWN_CLOCK_SEC);
    }
    app.mode = MODE_WRITING;
    app.ai_open = false;
    app.ai_input_len = 0;
    app.ai_input_cursor = 0;
    chat_clear();

#if HAS_LIBAI
    if (app.ai_ready && !app.ai_session)
        ai_init_session();
#endif
}

//! Start a new note of dawn's own, named by the time (2026-10-07_143005.md), or -2 .. -99 after
//! it when a note of that name exists already: a new note never takes an existing file's path.
//! @return false (with an error notice) when the notes directory cannot be made or no name is free
static bool new_session(void)
{
    const char* dir = history_dir();
    char msg[128];
    if (!dir[0]) {
        notice_post(NOTICE_ERROR, "couldn't start a note · the notes folder path is too long");
        return false;
    }
    if (!DAWN_BACKEND(app)->mkdir_p(dir)) {
        snprintf(msg, sizeof(msg), "couldn't make the notes folder · %s", strerror(errno));
        notice_post(NOTICE_ERROR, msg);
        return false;
    }
    DawnTime lt;
    DAWN_BACKEND(app)->localtime(&lt);
    char timestamp[20];
    dawn_format_filename_time(&lt, timestamp, sizeof(timestamp));
    char path[PATH_MAX];
#ifdef _WIN32
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    bool found = false;
    for (int32_t n = 1; n <= NOTEPATH_MAX_NUMBER && !found; n++) {
        if (!notepath_numbered(path, sizeof(path), dir, strlen(dir), sep, timestamp, n))
            break;
        found = !DAWN_BACKEND(app)->file_exists(path);
    }
    if (!found) {
        notice_post(NOTICE_ERROR, "couldn't start a note · no free name");
        return false;
    }
    begin_session(dawn_strdup(path));

    // A note of dawn's own carries frontmatter from the start.
    app.frontmatter = fm_create();
    fm_set_string(app.frontmatter, "title", "Untitled");
    fm_set_string(app.frontmatter, "author", DAWN_BACKEND(app)->username());
    char date_buf[32];
    dawn_format_iso_time(&lt, date_buf, sizeof(date_buf));
    fm_set_string(app.frontmatter, "date", date_buf);
    app.write_fm = true;
    return true;
}

//! Open a note picked in the history. One that cannot be opened leaves the list where it was,
//! with a notice (load_file_for_editing() posts its own for a file that is not text).
static bool open_from_history(const char* path)
{
    if (load_file_for_editing(path))
        return true;
    if (!load_refused_binary()) {
        char msg[128];
        snprintf(msg, sizeof(msg), "couldn't open %s · %s", notepath_base(path, NULL), strerror(errno));
        notice_post(NOTICE_ERROR, msg);
    }
    return false;
}

//! Open another note at byte pos of its body, from a meaning hit (Ctrl+S's "by meaning" group, the
//! chat's "also in:" line). The open note is saved first; one that can't be saved stays open, the
//! status bar saying why (or the conflict dialog asking), and so does one when the other can't be
//! opened (open_from_history() posts the notice). Either way the mode is MODE_WRITING after.
static bool open_note_at(const char* path, size_t pos)
{
    char target[EMBED_PATH_MAX];
    snprintf(target, sizeof(target), "%s", path);
    bool opened = save_session() && open_from_history(target);
    if (opened)
        restore_cursor_position(pos);
    app.mode = MODE_WRITING;
    clear_screen();
    return opened;
}

// #endregion

// #region Input Handlers

//! Move cursor with optional selection extension
static void move_cursor(size_t new_pos, bool extend_sel)
{
    if (extend_sel) {
        if (!app.selecting) {
            app.selecting = true;
            app.sel_anchor = app.cursor;
        }
    } else {
        app.selecting = false;
    }
    app.cursor = new_pos;
}

// #endregion

// #region Touch Input (dawn_touch)
//
// Termux sends SGR mouse events (dawn enables ?1000h ?1002h ?1006h): a tap is a button-0 press
// (DAWN_KEY_MOUSE_CLICK) then release (DAWN_KEY_MOUSE_RELEASE) at the same column/row, ?1002 adds
// motion while the button stays down (DAWN_KEY_MOUSE_DRAG, button 32) after a long-press, and a
// swipe is wheel buttons 64/65. None of these carry a position of their own; the position is
// whatever input_last_mouse_col()/row() held when the key was decoded (dawn_backend_posix.c).
//
// Double/triple tap and press-drag selection (P1 #touch) are tracked in touch_state below: a
// second press within DOUBLE_TAP_MS and DOUBLE_TAP_CELLS of the last one selects a word, a third
// selects the paragraph, and a press followed by a drag before release extends the selection from
// the press point to wherever the finger is now (no floating bar: Ctrl+C, typing and Ctrl+/ act on
// the resulting selection like they always have).

#define DOUBLE_TAP_MS 350
#define DOUBLE_TAP_CELLS 1

static struct {
    int64_t last_tap_ms; //!< clock() time of the previous press
    int32_t last_tap_row, last_tap_col; //!< screen cell of the previous press
    int32_t tap_run; //!< consecutive taps landing within the double-tap window/radius (1, 2, 3+)
    size_t press_pos; //!< buffer position of the current press, for press-then-drag extension
    bool press_active; //!< a press has landed and no release/drag-elsewhere has resolved it yet
} touch_state = { 0 };

//! Which pane a screen cell falls in. Beside the note the split is by column; as a bottom sheet
//! (calc_layout().ai_sheet) it is by row, the sheet's first row and everything under it being the
//! chat. In the full-screen overlay (too short for a sheet) the chat is drawn over every cell of
//! the note, so there is no uncovered note region to route a tap to: every tap there is a chat tap.
static bool tap_is_over_chat(const Layout* L, int32_t row, int32_t col)
{
    if (!app.ai_open)
        return false;
    if (L->ai_sheet)
        return row >= L->ai_top;
    return L->ai_overlay || col >= L->ai_start_col;
}

//! Reverse of calc_cursor_vrow_in_block's BLOCK_HEADER case. OSC 66 scales a header's text by
//! `scale`, so each character takes `scale` terminal columns and `available` characters fit per
//! wrapped row; dividing the tapped column by `scale` gets back to a character index. This is an
//! approximation, not an exact inverse: a grapheme wider than one column, or a tap that lands
//! between two scaled cells of the same character, can be off by a character. Good enough to land
//! in the right word.
static size_t map_header_vrow_to_pos(const Block* block, int32_t target_vrow, int32_t target_col, int32_t wrap_width)
{
    int32_t level = block->data.header.level;
    int32_t scale = (level == 1 && HAS_CAP(DAWN_CAP_TEXT_SIZING)) ? 2 : 1;
    int32_t available = wrap_width / scale;
    if (available < 1)
        available = 1;
    int32_t target_row = target_vrow / scale;
    int32_t target_char_col = target_col / scale;

    size_t pos = block->start;
    int32_t row = 0, char_col = 0;
    while (pos < block->end && gap_at(&app.text, pos) != '\n') {
        if (row == target_row && char_col >= target_char_col)
            return pos;
        size_t next;
        int32_t gw = gap_grapheme_width(&app.text, pos, &next);
        int32_t new_col = char_col + gw;
        if (new_col > available) {
            if (row == target_row)
                return pos; // the tapped row ends here; the tap was past its last character
            row++;
            char_col = gw;
        } else {
            char_col = new_col;
        }
        pos = next;
    }
    return pos;
}

//! Reverse of calc_cursor_vrow_in_block's BLOCK_CODE/BLOCK_MATH/BLOCK_TABLE case, which counts
//! newlines with no wrapping: walk newlines to the target line, then walk columns within it.
//! Also used, with target_col forced to 0, for tables/math/images/hr: those wrap or lay out their
//! content in ways this cannot invert exactly, so a tap there just snaps to the block's own line.
static size_t map_line_vrow_to_pos(const Block* block, int32_t target_vrow, int32_t target_col)
{
    size_t pos = block->start;
    int32_t vrow = 0;
    while (pos < block->end && vrow < target_vrow) {
        if (gap_at(&app.text, pos) == '\n')
            vrow++;
        pos++;
    }
    int32_t col = 0;
    while (pos < block->end && gap_at(&app.text, pos) != '\n') {
        size_t next;
        int32_t gw = gap_grapheme_width(&app.text, pos, &next);
        if (col + gw > target_col)
            break;
        col += gw;
        pos = next;
    }
    return pos;
}

//! Reverse of calc_cursor_vrow_in_block's default case (paragraphs, list items, block quotes,
//! footnote definitions): walk the block's wrapped lines, the same way nav_move_visual_line does,
//! until target_vrow is reached, then walk graphemes within that wrapped segment until target_col
//! is covered.
static size_t map_wrapped_vrow_to_pos(const Block* block, int32_t target_vrow, int32_t target_col, int32_t wrap_width)
{
    if (wrap_width <= 0)
        wrap_width = 1;

    size_t pos = block->start;
    int32_t vrow = 0;

    while (pos <= block->end) {
        size_t line_end = pos;
        while (line_end < block->end && gap_at(&app.text, line_end) != '\n')
            line_end++;

        size_t seg_start = pos;
        do {
            size_t seg_end = seg_start < line_end
                ? gap_find_wrap_point(&app.text, seg_start, line_end, wrap_width, NULL)
                : line_end;

            if (vrow == target_vrow) {
                size_t result = seg_start;
                int32_t width = 0;
                while (result < seg_end) {
                    size_t next;
                    int32_t gw = gap_grapheme_width(&app.text, result, &next);
                    if (width + gw > target_col)
                        break;
                    width += gw;
                    result = next;
                }
                return result;
            }
            vrow++;

            if (seg_end >= line_end)
                break;
            seg_start = seg_end;
            while (seg_start < line_end && gap_at(&app.text, seg_start) == ' ')
                seg_start++;
        } while (seg_start < line_end);

        if (line_end >= block->end)
            return line_end;
        pos = line_end + 1;
    }

    return block->end;
}

//! Map a tap's screen row/col to a byte position in the note. Uses the block cache in WYSIWYG
//! mode (the same model render_writing() draws from) and the plain-mode wrap cache in plain mode.
static size_t map_tap_to_buffer_pos(const Layout* L, int32_t screen_row, int32_t screen_col)
{
    size_t len = gap_len(&app.text);

    if (app.plain_mode) {
        if (!plain_wrap_cache.lines || plain_wrap_text_len != len || plain_wrap_width != L->text_width)
            return app.cursor; // stale cache; leave the cursor be rather than guess wrong
        int32_t vrow = screen_row - L->top_margin + app.scroll_y;
        if (vrow < 0)
            return 0;
        if (vrow >= plain_wrap_cache.count)
            return len;
        WrapLine* line = &plain_wrap_cache.lines[vrow];
        int32_t target_col = screen_col - L->margin - 1;
        if (target_col < 0)
            target_col = 0;
        size_t pos = line->start;
        int32_t width = 0;
        while (pos < line->end) {
            size_t next;
            int32_t gw = gap_grapheme_width(&app.text, pos, &next);
            if (width + gw > target_col)
                break;
            width += gw;
            pos = next;
        }
        return pos;
    }

    BlockCache* bc = (BlockCache*)app.block_cache;
    if (!bc || !bc->valid || bc->count == 0)
        return app.cursor;

    int32_t vrow = screen_row - L->top_margin + app.scroll_y;
    if (vrow < 0)
        return 0;
    if (vrow >= bc->total_vrows)
        return len;

    int32_t col = screen_col - L->margin - 1;
    if (col < 0)
        col = 0;

    Block* blk = block_at_vrow(bc, vrow);
    if (!blk)
        return app.cursor;

    int32_t vrow_in_block = vrow - blk->vrow_start;
    if (vrow_in_block < 0)
        return blk->start; // tap landed in the blank lines before the very first block
    if (vrow_in_block >= blk->vrow_count)
        return blk->end; // tap landed in the gap after this block, before the next one

    switch (blk->type) {
    case BLOCK_HEADER:
        return map_header_vrow_to_pos(blk, vrow_in_block, col, L->text_width);
    case BLOCK_CODE:
        return map_line_vrow_to_pos(blk, vrow_in_block, col);
    case BLOCK_TABLE:
    case BLOCK_MATH:
    case BLOCK_IMAGE:
    case BLOCK_HR:
        return map_line_vrow_to_pos(blk, vrow_in_block, 0);
    default:
        // BLOCK_PARAGRAPH, BLOCK_LIST_ITEM, BLOCK_BLOCKQUOTE, BLOCK_FOOTNOTE_DEF
        return map_wrapped_vrow_to_pos(blk, vrow_in_block, col, L->text_width);
    }
}

//! Task checkbox tap: toggles `- [ ]`/`- [x]` as one undo step, without moving the cursor. Only
//! the glyph columns ("☐ "/"☑ ", drawn by render_line_prefixes on the item's first line) count; a
//! tap on the item's text falls through to ordinary cursor placement.
static bool try_toggle_task_box(const Layout* L, int32_t screen_row, int32_t screen_col)
{
    if (app.plain_mode || !CAN_MODIFY())
        return false; // plain mode shows the raw "- [ ] " text, not a tappable glyph

    BlockCache* bc = (BlockCache*)app.block_cache;
    if (!bc || !bc->valid || bc->count == 0)
        return false;

    int32_t vrow = screen_row - L->top_margin + app.scroll_y;
    int32_t col = screen_col - L->margin - 1;
    if (vrow < 0 || col < 0)
        return false;

    Block* blk = block_at_vrow(bc, vrow);
    if (!blk || blk->type != BLOCK_LIST_ITEM || blk->data.list.task_state == 0)
        return false;
    if (vrow != blk->vrow_start)
        return false; // only the first line carries the box

    int32_t indent = blk->data.list.indent;
    if (col < indent || col >= indent + task_glyph_cols())
        return false;

    // The raw marker is "- [ ] "/"- [x] " starting at indent; the check char sits 3 past that
    // (md_check_task in dawn_md.c parses the very same offset).
    size_t check_pos = (size_t)blk->start + (size_t)indent + 3;
    if (check_pos >= blk->end)
        return false;
    char c = gap_at(&app.text, check_pos);
    if (c != ' ' && c != 'x' && c != 'X')
        return false;

    save_undo_state();
    gap_delete(&app.text, check_pos, 1);
    gap_insert(&app.text, check_pos, (c == ' ') ? 'x' : ' ');
    app.dirty = true;
    // Same length as before, so the block cache wouldn't notice and the box would stay unticked.
    block_cache_invalidate(bc);
    return true;
}

//! Whether a byte is part of a "word" for double-tap word selection: same class as is_word_key.
static inline bool is_word_byte(char c)
{
    return is_word_key((unsigned char)c);
}

//! Select the word touching pos (or the whitespace run touching it, if pos isn't on a word) -
//! double-tap in the note.
static void select_word_at(size_t pos)
{
    size_t len = gap_len(&app.text);
    if (len == 0) {
        app.selecting = false;
        return;
    }
    if (pos > len)
        pos = len;

    // Prefer the word to the left when sitting exactly on a boundary, like desktop double-click.
    size_t probe = pos;
    if (probe >= len || !is_word_byte(gap_at(&app.text, probe))) {
        if (probe > 0 && is_word_byte(gap_at(&app.text, probe - 1)))
            probe--;
    }
    // Hidden inline syntax (`**`, `_`, `~~`, `==`, backticks, brackets) in front of a word is not a
    // word of its own: start from the first word byte after it.
    if (probe < len && !is_word_byte(gap_at(&app.text, probe))) {
        size_t q = probe;
        if (gap_at(&app.text, q) == ' ')
            q++;
        size_t syntax_start = q;
        while (q < len && strchr("*_~=`[]", gap_at(&app.text, q)) && gap_at(&app.text, q) != '\0')
            q++;
        if (q > syntax_start && q < len && is_word_byte(gap_at(&app.text, q)))
            probe = q;
    }
    if (probe >= len) {
        app.selecting = false;
        return;
    }

    bool word = is_word_byte(gap_at(&app.text, probe));
    size_t s = probe, e = probe + 1;
    while (s > 0 && is_word_byte(gap_at(&app.text, s - 1)) == word && gap_at(&app.text, s - 1) != '\n')
        s--;
    while (e < len && is_word_byte(gap_at(&app.text, e)) == word && gap_at(&app.text, e) != '\n')
        e++;

    app.sel_anchor = s;
    app.cursor = e;
    app.selecting = (e > s);
}

//! Select the paragraph (markdown block) touching pos - triple-tap in the note.
static void select_paragraph_at(size_t pos)
{
    Block* b = get_block_at(pos);
    if (!b) {
        app.selecting = false;
        return;
    }
    app.sel_anchor = b->start;
    app.cursor = b->end;
    app.selecting = (b->end > b->start);
}

//! DAWN_KEY_MOUSE_CLICK (press) in MODE_WRITING: focuses whichever pane the tap landed in, and in
//! the note also places the cursor, like a mouse click does on desktop - unless it lands on a task
//! checkbox (toggles it in place) or continues a double/triple tap (selects word/paragraph).
static void handle_mouse_click(void)
{
    Layout L = calc_layout();
    int32_t row = input_last_mouse_row();
    int32_t col = input_last_mouse_col();

    if (tap_is_over_chat(&L, row, col)) {
        app.ai_focused = true;
        touch_state.press_active = false;
#if HAS_LIBAI
        // The picker: a tap on a row chooses it, anywhere else closes the list.
        if (chat_picker.open) {
            if (row >= chat_picker.first_row && row < chat_picker.first_row + chat_picker.rows
                && col >= chat_picker.col0 && col < chat_picker.col1) {
                int32_t first = chat_picker.sel >= chat_picker.rows ? chat_picker.sel - chat_picker.rows + 1 : 0;
                chat_picker_choose(first + row - chat_picker.first_row);
            } else {
                chat_picker.open = false;
            }
            return;
        }
        // A tap on the model's name opens it.
        if (chat_name_row > 0 && row == chat_name_row && col >= chat_name_col0 && col <= chat_name_col1)
            chat_picker_open();
#endif
        // A tap on "also in:" opens that note at the piece it names.
        if (also_in.row > 0 && row == also_in.row && col >= also_in.col0 && col < also_in.col1)
            open_note_at(also_in.path, also_in.start);
        return;
    }

    // A tap over the note focuses it, closing the chat's focus the same way Tab does; a sheet
    // stays open, dimmed, under the note. (In the full-screen overlay this branch is unreachable:
    // tap_is_over_chat() is always true there, so a tap never "closes" the overlay the way Esc
    // does - there is no uncovered note region to tap.)
    app.ai_focused = false;
    app.view_detached = false;

    if (try_toggle_task_box(&L, row, col)) {
        touch_state.press_active = false;
        touch_state.tap_run = 0;
        return;
    }

    int64_t now = DAWN_BACKEND(app)->clock(DAWN_CLOCK_MS);
    int32_t drow = row - touch_state.last_tap_row;
    int32_t dcol = col - touch_state.last_tap_col;
    if (drow < 0)
        drow = -drow;
    if (dcol < 0)
        dcol = -dcol;
    bool continues = touch_state.tap_run > 0 && now - touch_state.last_tap_ms <= DOUBLE_TAP_MS
        && drow <= DOUBLE_TAP_CELLS && dcol <= DOUBLE_TAP_CELLS;
    touch_state.tap_run = continues ? touch_state.tap_run + 1 : 1;
    touch_state.last_tap_ms = now;
    touch_state.last_tap_row = row;
    touch_state.last_tap_col = col;

    size_t pos = map_tap_to_buffer_pos(&L, row, col);
    // The first tap of a double or triple tap put the cursor on this line, which now shows its
    // raw syntax and has shifted the text under the finger; the later taps mean the same byte.
    if (touch_state.tap_run >= 2)
        pos = touch_state.press_pos;

    if (touch_state.tap_run == 2) {
        select_word_at(pos);
    } else if (touch_state.tap_run >= 3) {
        select_paragraph_at(pos);
    } else {
        app.selecting = false;
        app.cursor = pos;
    }

    // Remember the press so a drag before release extends a selection from here.
    touch_state.press_pos = pos;
    touch_state.press_active = true;
}

//! A swipe arrives as a run of wheel events with no begin or end of its own; one landing more than
//! this long after the previous starts a new swipe.
#define SWIPE_GAP_MS 250

static struct {
    int64_t last_ms; //!< clock() time of the previous wheel event
    bool over_chat; //!< the pane the current swipe started over, which keeps it for the whole swipe
    bool swallow; //!< the swipe closed the sheet; its remaining events go nowhere
} swipe_state = { 0 };

//! DAWN_KEY_MOUSE_SCROLL_UP/DOWN in MODE_WRITING: routed to whichever pane the finger is over, not
//! whichever pane has focus, so scrolling the note works while the chat is focused and vice versa.
//! The pane is chosen where the swipe starts and kept until it ends, so a swipe that drifts over a
//! sheet's edge doesn't change hands halfway. On a sheet, a swipe down (wheel up, Termux's reading
//! of a finger moving down) that starts on its top row, or with the chat already showing its
//! oldest message, closes it, as a sheet does.
static void handle_mouse_scroll(int32_t key)
{
    Layout L = calc_layout();
    int32_t row = input_last_mouse_row();
    int32_t col = input_last_mouse_col();

    int64_t now = DAWN_BACKEND(app)->clock(DAWN_CLOCK_MS);
    bool starts = swipe_state.last_ms == 0 || now - swipe_state.last_ms > SWIPE_GAP_MS;
    swipe_state.last_ms = now;
    if (starts) {
        swipe_state.over_chat = tap_is_over_chat(&L, row, col);
        swipe_state.swallow = false;
        // The finger has usually moved a row by the time the first event is sent, so the handle
        // row and the one under it both count as "on the edge".
        bool on_edge = row <= L.ai_top + 1;
        if (L.ai_sheet && swipe_state.over_chat && key == DAWN_KEY_MOUSE_SCROLL_UP
            && (on_edge || app.chat_scroll >= chat_max_scroll)) {
            app.ai_open = false;
            app.ai_focused = false;
            swipe_state.swallow = true;
            return;
        }
    }
    if (swipe_state.swallow)
        return;

    if (app.ai_open && swipe_state.over_chat) {
        if (key == DAWN_KEY_MOUSE_SCROLL_UP)
            app.chat_scroll += 3;
        else {
            app.chat_scroll -= 3;
            if (app.chat_scroll < 0)
                app.chat_scroll = 0;
        }
        return;
    }

    // Scrolling the note by touch detaches the view from the cursor so render_writing() doesn't
    // immediately snap it back before the finger even lifts; it clamps scroll_y to the document's
    // own bounds instead, until a cursor-moving or editing key reattaches the view (handle_writing).
    app.view_detached = true;
    if (key == DAWN_KEY_MOUSE_SCROLL_UP) {
        app.scroll_y -= 3;
        if (app.scroll_y < 0)
            app.scroll_y = 0;
    } else {
        app.scroll_y += 3; // clamped to the document's own bounds during render
    }
}

//! DAWN_KEY_MOUSE_DRAG (mode 1002 motion with button 0 held) in MODE_WRITING: extends the
//! selection from the original press point to wherever the finger is now, and auto-scrolls when
//! the finger sits at the top/bottom edge of the note so a long selection can reach off-screen.
static void handle_mouse_drag(void)
{
    Layout L = calc_layout();
    int32_t row = input_last_mouse_row();
    int32_t col = input_last_mouse_col();

    if (tap_is_over_chat(&L, row, col))
        return; // no drag-selection in the chat pane

    if (!touch_state.press_active) {
        // A drag reported without a press we tracked (e.g. it started over the chat pane and
        // slid onto the note) still needs an anchor: use wherever the finger is now.
        touch_state.press_pos = map_tap_to_buffer_pos(&L, row, col);
        touch_state.press_active = true;
    }

    app.ai_focused = false;
    app.view_detached = true;

    // Auto-scroll while the finger sits at the top/bottom margin row, same step as a swipe.
    if (row <= L.top_margin) {
        app.scroll_y -= 3;
        if (app.scroll_y < 0)
            app.scroll_y = 0;
    } else if (row >= L.top_margin + L.text_height) {
        app.scroll_y += 3; // clamped to the document's own bounds during render
    }

    size_t pos = map_tap_to_buffer_pos(&L, row, col);
    app.sel_anchor = touch_state.press_pos;
    app.cursor = pos;
    app.selecting = (pos != touch_state.press_pos);
}

//! DAWN_KEY_MOUSE_RELEASE (button 0 lifted) in MODE_WRITING: just ends the press/drag tracking:
//! a plain tap already placed the cursor (or toggled a task box) on the press in handle_mouse_click,
//! and a drag has already left its selection in place.
static void handle_mouse_release(void)
{
    touch_state.press_active = false;
}

// #endregion

// #region Input Handlers

static void handle_writing(int32_t key)
{
    size_t len = gap_len(&app.text);

    // Every key that reaches here edits or navigates the note (mouse scroll and click are routed
    // to handle_mouse_scroll()/handle_mouse_click() before handle_writing() is ever called), so
    // this is where a touch-detached view reattaches to the cursor (dawn_touch).
    app.view_detached = false;

    switch (key) {
    case '\x1b':
        if (app.ai_open)
            app.ai_open = false;
        else if (app.preview_mode)
            app.quit = true;
        else {
            // A note that could not be saved stays open, with the status bar saying so; the next
            // esc after that warning leaves anyway, since the disk may never come back. A note
            // changed elsewhere: while saving is paused, the first esc asks again (MODE_CONFLICT);
            // the next one leaves without overwriting, the writer's text kept in the conflict
            // copy (dawn_frame posts where).
            bool warned = app.save_failed || (app.save_paused && app.conflict_leave_asked);
            if (app.save_paused && !app.conflict_leave_asked) {
                app.conflict_leave_asked = true;
                app.conflict_prompt = true;
            }
            if (!save_session() && !warned)
                break;
            app.mode = app.timer_on ? MODE_FINISHED : MODE_WELCOME;
        }
        break;
    case 16:
        timer_toggle_pause();
        break;
    case 20:
        timer_add_minutes(5);
        break;
    case 6:
        if (!app.preview_mode)
            app.focus_mode = !app.focus_mode;
        break;
    case 18:
        app.plain_mode = !app.plain_mode;
        if (app.plain_mode)
            image_clear_all();
        break;
    case 2:
        app.hide_cursor_syntax = !app.hide_cursor_syntax;
        break;
    case 14:
        footnote_jump(&app.text, &app.cursor);
        break;
    case 15:
        app.help_page = 0;
        MODE_PUSH(MODE_HELP);
        break;

    case 12: // Ctrl+L - Table of Contents
    {
        if (!app.toc_state) {
            app.toc_state = malloc(sizeof(TocState));
            toc_init((TocState*)app.toc_state);
        }
        TocState* toc = (TocState*)app.toc_state;
        toc->filter_len = 0;
        toc->filter[0] = '\0';
        toc->selected = 0;
        toc->scroll = 0;
        toc_build(&app.text, toc);
        MODE_PUSH(MODE_TOC);
    } break;

    case 19: // Ctrl+S - Search
    {
        if (!app.search_state) {
            app.search_state = malloc(sizeof(SearchState));
            search_init((SearchState*)app.search_state);
        }
        SearchState* search = (SearchState*)app.search_state;
        search->selected = 0;
        search->scroll = 0;
        // Mark dirty to trigger immediate search with previous query
        if (search->query_len > 0) {
            search_mark_dirty(search, 0); // time=0 ensures immediate search
        }
        MODE_PUSH(MODE_SEARCH);
    } break;

    case 5: // Ctrl+E - Edit block
        if (!CAN_MODIFY())
            break;
        {
            Block* block = get_block_at(app.cursor);
            if (!block)
                break;

            app.block_edit.type = block->type;
            app.block_edit.pos = block->start;
            app.block_edit.len = block->end - block->start;
            app.block_edit.field = 0;

            switch (block->type) {
            case BLOCK_IMAGE: {
                int32_t img_w = block->data.image.width;
                int32_t img_h = block->data.image.height;

                // Load alt text
                size_t alt_len = block->data.image.alt_len;
                if (alt_len >= sizeof(app.block_edit.image.alt))
                    alt_len = sizeof(app.block_edit.image.alt) - 1;
                for (size_t i = 0; i < alt_len; i++)
                    app.block_edit.image.alt[i] = gap_at(&app.text, block->data.image.alt_start + i);
                app.block_edit.image.alt_len = alt_len;

                // Load title
                size_t title_len = block->data.image.title_len;
                if (title_len >= sizeof(app.block_edit.image.title))
                    title_len = sizeof(app.block_edit.image.title) - 1;
                for (size_t i = 0; i < title_len; i++)
                    app.block_edit.image.title[i] = gap_at(&app.text, block->data.image.title_start + i);
                app.block_edit.image.title_len = title_len;

                // Load width/height
                app.block_edit.image.width_len = 0;
                app.block_edit.image.height_len = 0;
                app.block_edit.image.width_pct = (img_w < 0);
                app.block_edit.image.height_pct = (img_h < 0);
                if (img_w != 0) {
                    int32_t val = img_w < 0 ? -img_w : img_w;
                    app.block_edit.image.width_len = (size_t)snprintf(app.block_edit.image.width,
                        sizeof(app.block_edit.image.width), "%d", val);
                }
                if (img_h != 0) {
                    int32_t val = img_h < 0 ? -img_h : img_h;
                    app.block_edit.image.height_len = (size_t)snprintf(app.block_edit.image.height,
                        sizeof(app.block_edit.image.height), "%d", val);
                }
                MODE_PUSH(MODE_BLOCK_EDIT);
                break;
            }
            // Future: case BLOCK_CODE, case BLOCK_TABLE, etc.
            default:
                break;
            }
        }
        break;

    case 7: {
        // Ctrl+G: Open frontmatter editor
        if (!CAN_MODIFY())
            break;
        fm_edit_init();
        MODE_PUSH(MODE_FM_EDIT);
        break;
    }

    case 26:
        if (CAN_MODIFY())
            undo();
        break;
    case 25:
        if (CAN_MODIFY())
            redo();
        break;

    case 31:
#if HAS_LIBAI
        // No model or TAI off: the chat doesn't open, and the status line says so once.
        if (app.ai_ready && CAN_MODIFY() && (app.ai_open || session_chat_may_open())) {
            app.ai_open = !app.ai_open;
            app.ai_focused = app.ai_open;
            if (app.ai_open) {
                if (!app.ai_session)
                    ai_init_session();
                // Warm the model now: by the time the question is typed, the note is read.
                session_chat_opened();
            }
        }
#endif
        break;

    case DAWN_KEY_LEFT:
        move_cursor(gap_utf8_prev(&app.text, app.cursor), false);
        break;
    case DAWN_KEY_RIGHT:
        move_cursor(gap_utf8_next(&app.text, app.cursor), false);
        break;
    case DAWN_KEY_UP:
        move_cursor(nav_move_visual_line_block_aware(app.cursor, -1, get_text_width(), app.hide_cursor_syntax), false);
        break;
    case DAWN_KEY_DOWN:
        move_cursor(nav_move_visual_line_block_aware(app.cursor, 1, get_text_width(), app.hide_cursor_syntax), false);
        break;
    case DAWN_KEY_ALT_LEFT:
    case DAWN_KEY_CTRL_LEFT:
        move_cursor(nav_word_left(app.cursor), false);
        break;
    case DAWN_KEY_ALT_RIGHT:
    case DAWN_KEY_CTRL_RIGHT:
        move_cursor(nav_word_right(app.cursor), false);
        break;
    case DAWN_KEY_SHIFT_LEFT:
        move_cursor(gap_utf8_prev(&app.text, app.cursor), true);
        break;
    case DAWN_KEY_SHIFT_RIGHT:
        move_cursor(gap_utf8_next(&app.text, app.cursor), true);
        break;
    case DAWN_KEY_SHIFT_UP:
        move_cursor(nav_move_visual_line_block_aware(app.cursor, -1, get_text_width(), app.hide_cursor_syntax), true);
        break;
    case DAWN_KEY_SHIFT_DOWN:
        move_cursor(nav_move_visual_line_block_aware(app.cursor, 1, get_text_width(), app.hide_cursor_syntax), true);
        break;
    case DAWN_KEY_CTRL_SHIFT_LEFT:
    case DAWN_KEY_ALT_SHIFT_LEFT:
        move_cursor(nav_word_left(app.cursor), true);
        break;
    case DAWN_KEY_CTRL_SHIFT_RIGHT:
    case DAWN_KEY_ALT_SHIFT_RIGHT:
        move_cursor(nav_word_right(app.cursor), true);
        break;
    case DAWN_KEY_HOME:
        move_cursor(nav_line_start(app.cursor), false);
        break;
    case DAWN_KEY_END:
        move_cursor(nav_line_end(app.cursor), false);
        break;

    // Ctrl+Home/End: Jump to document start/end
    case DAWN_KEY_CTRL_HOME:
        move_cursor(0, false);
        break;
    case DAWN_KEY_CTRL_END:
        move_cursor(gap_len(&app.text), false);
        break;

    // Alt+Up/Down: Jump by half-screen
    case DAWN_KEY_ALT_UP: {
        Layout L = calc_layout();
        int32_t count = L.text_height / 2;
        if (count < 1)
            count = 1;
        for (int32_t i = 0; i < count; i++) {
            size_t new_pos = nav_move_visual_line_block_aware(app.cursor, -1, get_text_width(), app.hide_cursor_syntax);
            if (new_pos == app.cursor)
                break;
            app.cursor = new_pos;
        }
        app.selecting = false;
        break;
    }
    case DAWN_KEY_ALT_DOWN: {
        Layout L = calc_layout();
        int32_t count = L.text_height / 2;
        if (count < 1)
            count = 1;
        for (int32_t i = 0; i < count; i++) {
            size_t new_pos = nav_move_visual_line_block_aware(app.cursor, 1, get_text_width(), app.hide_cursor_syntax);
            if (new_pos == app.cursor)
                break;
            app.cursor = new_pos;
        }
        app.selecting = false;
        break;
    }

    // Mouse scroll and click no longer reach here: MODE_WRITING's dispatch (handle_input) routes
    // them to handle_mouse_scroll()/handle_mouse_click() by tap/finger position before choosing
    // between handle_ai_input() and handle_writing() (dawn_touch).

    // Page Up/Down: scroll by half screen and move cursor
    case DAWN_KEY_PGUP: {
        Layout L = calc_layout();
        int32_t count = L.text_height / 2;
        if (count < 1)
            count = 1;
        for (int32_t i = 0; i < count; i++) {
            size_t new_pos = nav_move_visual_line_block_aware(app.cursor, -1, get_text_width(), app.hide_cursor_syntax);
            if (new_pos == app.cursor)
                break;
            app.cursor = new_pos;
        }
        app.selecting = false;
        break;
    }
    case DAWN_KEY_PGDN: {
        Layout L = calc_layout();
        int32_t count = L.text_height / 2;
        if (count < 1)
            count = 1;
        for (int32_t i = 0; i < count; i++) {
            size_t new_pos = nav_move_visual_line_block_aware(app.cursor, 1, get_text_width(), app.hide_cursor_syntax);
            if (new_pos == app.cursor)
                break;
            app.cursor = new_pos;
        }
        app.selecting = false;
        break;
    }

    case 1:
        app.sel_anchor = 0;
        app.cursor = gap_len(&app.text);
        app.selecting = true;
        break;

    case 3:
        if (has_selection()) {
            size_t s, e;
            get_selection(&s, &e);
            char* sel_text = gap_substr(&app.text, s, e);
            clipboard_copy(sel_text, e - s);
            free(sel_text);
        }
        break;

    case 22:
        if (!CAN_MODIFY())
            break;
        {
            size_t paste_len;
            char* paste_text = clipboard_paste(&paste_len);
            if (paste_text && paste_len > 0) {
                paste_len = normalize_line_endings(paste_text, paste_len);
                save_undo_state();
                delete_selection_if_any();
                gap_insert_str(&app.text, app.cursor, paste_text, paste_len);
                app.cursor += paste_len;
            }
            free(paste_text);
        }
        break;

    case 24:
        if (has_selection()) {
            size_t s, e;
            get_selection(&s, &e);
            char* sel_text = gap_substr(&app.text, s, e);
            clipboard_copy(sel_text, e - s);
            free(sel_text);
            if (CAN_EDIT()) {
                save_undo_state();
                delete_selection_if_any();
            }
        }
        break;

    case 127:
    case 8:
        if (!CAN_EDIT())
            break;
        save_undo_state();
        delete_selection_if_any();
        if (!app.selecting && app.cursor > 0) {
            if (!smart_backspace()) {
                size_t prev = gap_utf8_prev(&app.text, app.cursor);
                gap_delete(&app.text, prev, app.cursor - prev);
                app.cursor = prev;
            }
        }
        break;

    case DAWN_KEY_DEL:
        if (!CAN_EDIT())
            break;
        save_undo_state();
        delete_selection_if_any();
        if (!app.selecting && app.cursor < len) {
            size_t next = gap_utf8_next(&app.text, app.cursor);
            gap_delete(&app.text, app.cursor, next - app.cursor);
        }
        break;

    case 23:
        if (!CAN_EDIT())
            break;
        save_undo_state();
        delete_selection_if_any();
        if (!app.selecting) {
            size_t new_pos = nav_word_left(app.cursor);
            gap_delete(&app.text, new_pos, app.cursor - new_pos);
            app.cursor = new_pos;
        }
        break;

    case 21:
        if (!CAN_EDIT())
            break;
        save_undo_state();
        {
            size_t ls = nav_line_start(app.cursor);
            gap_delete(&app.text, ls, app.cursor - ls);
            app.cursor = ls;
            app.selecting = false;
        }
        break;

    case 11:
        if (!CAN_EDIT())
            break;
        save_undo_state();
        {
            size_t le = nav_line_end(app.cursor);
            gap_delete(&app.text, app.cursor, le - app.cursor);
            app.selecting = false;
        }
        break;

    case 4:
        if (!CAN_EDIT())
            break;
        save_undo_state();
        {
            size_t del_start, del_len;
            BlockCache* bc = (BlockCache*)app.block_cache;
            if (block_find_element_at(bc, &app.text, app.cursor, &del_start, &del_len)) {
                gap_delete(&app.text, del_start, del_len);
                app.cursor = del_start;
                app.selecting = false;
            } else if (app.cursor < len) {
                size_t next = gap_utf8_next(&app.text, app.cursor);
                gap_delete(&app.text, app.cursor, next - app.cursor);
            }
        }
        break;

    case '\t': {
        if (!CAN_MODIFY())
            break;
        save_undo_state();
        size_t line_start = find_line_start(app.cursor);
        // Use block query to check if in list
        bool in_list = is_in_list_item(line_start, NULL, NULL, NULL, NULL);
        if (in_list) {
            gap_insert(&app.text, line_start, ' ');
            gap_insert(&app.text, line_start, ' ');
            app.cursor += 2;
        } else {
            gap_insert(&app.text, app.cursor, ' ');
            app.cursor++;
            gap_insert(&app.text, app.cursor, ' ');
            app.cursor++;
        }
        break;
    }

    case DAWN_KEY_BTAB: {
        if (!CAN_MODIFY())
            break;
        size_t line_start = find_line_start(app.cursor);
        int32_t spaces = 0;
        while (line_start + spaces < gap_len(&app.text) && gap_at(&app.text, line_start + spaces) == ' ' && spaces < 2)
            spaces++;
        if (spaces > 0) {
            save_undo_state();
            gap_delete(&app.text, line_start, spaces);
            app.cursor = (app.cursor >= line_start + spaces) ? app.cursor - spaces : (app.cursor > line_start) ? line_start
                                                                                                               : app.cursor;
        }
        break;
    }

    case '\r':
    case '\n': {
        if (!CAN_MODIFY())
            break;
        save_undo_state();
        delete_selection_if_any();

        size_t line_start = find_line_start(app.cursor);

        // Check for list item using block query
        int32_t list_indent, list_type, task_state;
        size_t content_start;
        if (is_in_list_item(line_start, &list_indent, &content_start, &list_type, &task_state)) {
            if (task_state > 0) {
                // Task list item
                if (is_item_content_empty(&app.text, app.cursor, content_start)) {
                    handle_empty_list_item(&app.text, &app.cursor, line_start);
                } else {
                    gap_insert(&app.text, app.cursor++, '\n');
                    insert_chars_at_cursor(&app.text, &app.cursor, ' ', list_indent);
                    insert_str_at_cursor(&app.text, &app.cursor, "- [ ] ");
                }
            } else {
                // Regular list item
                if (is_item_content_empty(&app.text, app.cursor, content_start)) {
                    handle_empty_list_item(&app.text, &app.cursor, line_start);
                } else {
                    gap_insert(&app.text, app.cursor++, '\n');
                    insert_chars_at_cursor(&app.text, &app.cursor, ' ', list_indent);
                    if (list_type == 1) {
                        char marker[3] = { gap_at(&app.text, line_start + list_indent), ' ', '\0' };
                        insert_str_at_cursor(&app.text, &app.cursor, marker);
                    } else {
                        size_t p = line_start + list_indent;
                        int32_t num = 0;
                        while (p < gap_len(&app.text) && gap_at(&app.text, p) >= '0' && gap_at(&app.text, p) <= '9')
                            num = num * 10 + (gap_at(&app.text, p++) - '0');
                        char num_buf[16];
                        snprintf(num_buf, sizeof(num_buf), "%d. ", num + 1);
                        insert_str_at_cursor(&app.text, &app.cursor, num_buf);
                    }
                }
            }
            break;
        }

        // Check for blockquote - use md_check_blockquote directly to get content_start for THIS line
        size_t quote_content;
        int32_t quote_level = md_check_blockquote(&app.text, line_start, &quote_content);
        if (quote_level > 0) {
            if (is_item_content_empty(&app.text, app.cursor, quote_content)) {
                handle_empty_list_item(&app.text, &app.cursor, line_start);
            } else {
                gap_insert(&app.text, app.cursor++, '\n');
                for (int32_t i = 0; i < quote_level; i++)
                    insert_str_at_cursor(&app.text, &app.cursor, "> ");
            }
            break;
        }

        gap_insert(&app.text, app.cursor++, '\n');
        break;
    }

    default:
        if (!CAN_MODIFY())
            break;
        // Accept printable chars but not special keys (DAWN_KEY_UP=1000 and above)
        if (IS_PRINTABLE(key)) {
            save_undo_typing(key);
            delete_selection_if_any();
            uint8_t utf8_buf[4];
            utf8proc_ssize_t len = utf8proc_encode_char((utf8proc_int32_t)key, utf8_buf);
            if (len > 0) {
                gap_insert_str(&app.text, app.cursor, (char*)utf8_buf, (size_t)len);
                app.cursor += (size_t)len;
                if (key < 128) {
                    check_auto_newline((char)key);
                    check_auto_list_space(key);
                }
                if (key == ']')
                    footnote_maybe_create_at_cursor(&app.text, app.cursor);
            }
            undo_typed();
        }
        break;
    }
}

static void handle_ai_input(int32_t key)
{
#if HAS_LIBAI
    if (chat_picker.open) {
        chat_picker_key(key);
        return;
    }
#endif
    switch (key) {
#if HAS_LIBAI
    case 12:
        // Ctrl+L: the model picker (in the note, Ctrl+L is the outline: both are "the list").
        chat_picker_open();
        break;
#endif

    case '\x1b':
#if HAS_LIBAI
        // While a reply is on its way, esc stops it; the panel closes on the next esc.
        if (app.ai_thinking && !app.ai_stopping) {
            ai_stop();
            break;
        }
#endif
        app.ai_open = false;
        break;

    case 31:
        // The key that opened the chat closes it, from the chat too: as an overlay the note
        // never sees a key while the chat is up.
        app.ai_open = false;
        app.ai_focused = false;
        break;

    case '\r':
    case '\n':
        if (app.ai_input_len > 0 && !app.ai_thinking) {
            app.ai_input[app.ai_input_len] = '\0';
#if HAS_LIBAI
            ai_send(app.ai_input);
#endif
            app.ai_input_len = 0;
            app.ai_input_cursor = 0;
        }
        break;

    case 15:
        if (app.ai_input_len < MAX_AI_INPUT - 1) {
            memmove(app.ai_input + app.ai_input_cursor + 1,
                app.ai_input + app.ai_input_cursor,
                app.ai_input_len - app.ai_input_cursor);
            app.ai_input[app.ai_input_cursor] = '\n';
            app.ai_input_len++;
            app.ai_input_cursor++;
        }
        break;

    case 127:
    case 8:
        if (app.ai_input_cursor > 0) {
            int32_t prev = str_utf8_prev(app.ai_input, app.ai_input_cursor);
            int32_t char_len = app.ai_input_cursor - prev;
            memmove(app.ai_input + prev,
                app.ai_input + app.ai_input_cursor,
                app.ai_input_len - app.ai_input_cursor);
            app.ai_input_len -= char_len;
            app.ai_input_cursor = prev;
        }
        break;

    case 22: {
        size_t paste_len;
        char* paste_text = clipboard_paste(&paste_len);
        if (paste_text && paste_len > 0) {
            if (app.ai_input_len + paste_len >= MAX_AI_INPUT)
                paste_len = MAX_AI_INPUT - app.ai_input_len - 1;
            if (paste_len > 0) {
                memmove(app.ai_input + app.ai_input_cursor + paste_len,
                    app.ai_input + app.ai_input_cursor,
                    app.ai_input_len - app.ai_input_cursor);
                memcpy(app.ai_input + app.ai_input_cursor, paste_text, paste_len);
                app.ai_input_len += paste_len;
                app.ai_input_cursor += paste_len;
            }
        }
        free(paste_text);
        break;
    }

    case DAWN_KEY_LEFT:
        if (app.ai_input_cursor > 0)
            app.ai_input_cursor = str_utf8_prev(app.ai_input, app.ai_input_cursor);
        break;
    case DAWN_KEY_RIGHT:
        if (app.ai_input_cursor < app.ai_input_len)
            app.ai_input_cursor = str_utf8_next(app.ai_input, app.ai_input_cursor, app.ai_input_len);
        break;

    case DAWN_KEY_UP: {
        size_t ls = app.ai_input_cursor;
        while (ls > 0 && app.ai_input[ls - 1] != '\n')
            ls--;
        size_t col = app.ai_input_cursor - ls;
        if (ls > 0) {
            size_t pe = ls - 1, ps = pe;
            while (ps > 0 && app.ai_input[ps - 1] != '\n')
                ps--;
            size_t pl = pe - ps;
            app.ai_input_cursor = ps + (col < pl ? col : pl);
        }
        break;
    }

    case DAWN_KEY_DOWN: {
        size_t ls = app.ai_input_cursor;
        while (ls > 0 && app.ai_input[ls - 1] != '\n')
            ls--;
        size_t col = app.ai_input_cursor - ls;
        size_t le = app.ai_input_cursor;
        while (le < app.ai_input_len && app.ai_input[le] != '\n')
            le++;
        if (le < app.ai_input_len) {
            size_t ns = le + 1, ne = ns;
            while (ne < app.ai_input_len && app.ai_input[ne] != '\n')
                ne++;
            size_t nl = ne - ns;
            app.ai_input_cursor = ns + (col < nl ? col : nl);
        }
        break;
    }

    case DAWN_KEY_HOME:
        while (app.ai_input_cursor > 0 && app.ai_input[app.ai_input_cursor - 1] != '\n')
            app.ai_input_cursor--;
        break;
    case DAWN_KEY_END:
        while (app.ai_input_cursor < app.ai_input_len && app.ai_input[app.ai_input_cursor] != '\n')
            app.ai_input_cursor++;
        break;

    // Mouse wheel scroll is routed to handle_mouse_scroll() before this is called (dawn_touch); it
    // scrolls app.chat_scroll the same way, just by tap position rather than focus.
    case DAWN_KEY_PGUP:
        app.chat_scroll += 10;
        break;
    case DAWN_KEY_PGDN:
        app.chat_scroll -= 10;
        if (app.chat_scroll < 0)
            app.chat_scroll = 0;
        break;

    default:
        if (IS_PRINTABLE(key))
            str_insert_codepoint(app.ai_input, MAX_AI_INPUT, &app.ai_input_len, &app.ai_input_cursor, key);
        break;
    }
}

//! A key on MODE_HELP's meaning page: m switches the index off or on (and remembers it in
//! settings.json), r arms a rebuild and a second r within HELP_REBUILD_CONFIRM_MS starts it; a tap
//! on either row does the same. A tap elsewhere in the box, and the release, drag and scroll that
//! come with taps, do nothing (the release after a tap on a row would otherwise close help).
//! @return false for any other key, which closes help
static bool help_meaning_key(int32_t key)
{
    if (key == DAWN_KEY_MOUSE_RELEASE || key == DAWN_KEY_MOUSE_DRAG || key == DAWN_KEY_MOUSE_SCROLL_UP
        || key == DAWN_KEY_MOUSE_SCROLL_DOWN)
        return true;
    if (key == DAWN_KEY_MOUSE_CLICK) {
        int32_t hit = render_help_hit(input_last_mouse_row(), input_last_mouse_col());
        if (hit == HELP_HIT_OUTSIDE)
            return false;
        if (hit == HELP_HIT_BOX)
            return true;
        key = hit == HELP_HIT_TOGGLE ? 'm' : 'r';
    }
    if (key == 'm') {
        app.meaning_index = !app.meaning_index;
        embed_set_enabled(app.meaning_index);
        settings_save();
        app.help_rebuild_armed = 0;
        return true;
    }
    if (key == 'r') {
        if (!embed_enabled())
            return true; // the row says to turn it on first
        int64_t now = DAWN_BACKEND(app)->clock(DAWN_CLOCK_MS);
        if (app.help_rebuild_armed && now - app.help_rebuild_armed < HELP_REBUILD_CONFIRM_MS) {
            app.help_rebuild_armed = 0;
            embed_rebuild();
            notice_post(NOTICE_INFO, "rebuilding the meaning index");
        } else {
            app.help_rebuild_armed = now;
        }
        return true;
    }
    return false;
}

static void handle_input(void)
{
    int32_t key = input_read_key();
    if (key == DAWN_KEY_NONE)
        return;

    // Any real input acknowledges a sticky NOTICE_ERROR (a key or a tap - MOUSE_RELEASE would
    // double up with the MOUSE_CLICK that landed the tap, so it's excluded; so is a dictation
    // event, which the user didn't type).
    if (key != DAWN_KEY_MOUSE_RELEASE && key != DAWN_KEY_DICTATION)
        notice_ack();

    // The launcher's light/dark report (P1 #colors): only takes over app.theme from dawn's own
    // persisted setting when a material palette is actually loaded for at least one mode: without
    // the files there's nothing for the launcher's report to change, so dawn's own ^D toggle (and
    // whatever the user last chose) keeps working exactly as before.
    if (key == DAWN_KEY_THEME_DARK || key == DAWN_KEY_THEME_LIGHT) {
        if (theme_material_active()) {
            Theme want = (key == DAWN_KEY_THEME_DARK) ? THEME_DARK : THEME_LIGHT;
            if (app.theme != want) {
                app.theme = want;
                highlight_cleanup(app.hl_ctx);
                app.hl_ctx = highlight_init(app.theme == THEME_DARK);
            }
        }
        return;
    }

    // Voice helpers (dawn_voice): dictation events, Ctrl+Q read-aloud, any key stopping it.
    if (voice_handle_key(key))
        return;

    switch (app.mode) {
    case MODE_WELCOME:
        switch (key) {
        case 'q':
            app.quit = 1;
            break;
        case '\r':
        case '\n':
            new_session();
            break;
        case 't':
            load_history();
            app.mode = MODE_TIMER_SELECT;
            break;
        case 'h':
            load_history();
            app.mode = MODE_HISTORY;
            break;
        case 'd':
            app.theme = (app.theme == THEME_DARK) ? THEME_LIGHT : THEME_DARK;
            highlight_cleanup(app.hl_ctx);
            app.hl_ctx = highlight_init(app.theme == THEME_DARK);
            settings_save();
            break;
        case '?':
            app.help_page = 0;
            MODE_PUSH(MODE_HELP);
            break;
        }
        break;

    case MODE_TIMER_SELECT:
        switch (key) {
        case '\x1b':
            app.mode = MODE_WELCOME;
            break;
        case 'k':
        case DAWN_KEY_UP:
            if (app.preset_idx > 0)
                app.preset_idx--;
            app.timer_mins = TIMER_PRESETS[app.preset_idx];
            settings_save();
            break;
        case 'j':
        case DAWN_KEY_DOWN:
            if (app.preset_idx < (int32_t)NUM_PRESETS - 1)
                app.preset_idx++;
            app.timer_mins = TIMER_PRESETS[app.preset_idx];
            settings_save();
            break;
        case '\r':
        case '\n':
            app.mode = MODE_WELCOME;
            break;
        }
        break;

    case MODE_STYLE:
        switch (key) {
        case '\x1b':
            app.mode = MODE_WELCOME;
            break;
        case 'k':
        case DAWN_KEY_UP:
            if (app.style > STYLE_MINIMAL)
                app.style--;
            break;
        case 'j':
        case DAWN_KEY_DOWN:
            if (app.style < STYLE_ELEGANT)
                app.style++;
            break;
        case '\r':
        case '\n':
            app.mode = MODE_WELCOME;
            break;
        }
        break;

    case MODE_HISTORY:
        switch (key) {
        case '\x1b':
            app.mode = MODE_WELCOME;
            break;
        case 'k':
        case DAWN_KEY_UP:
            if (app.hist_sel > 0)
                app.hist_sel--;
            break;
        case 'j':
        case DAWN_KEY_DOWN:
            if (app.hist_sel < app.hist_count - 1)
                app.hist_sel++;
            break;
        case 'o':
        case '\r':
        case '\n':
            if (app.hist_count > 0) {
                HistoryEntry* entry = &app.history[app.hist_sel];
                if (open_from_history(entry->path))
                    restore_cursor_position(entry->cursor);
            }
            break;
        case 'e':
            if (app.hist_count > 0)
                open_in_finder(app.history[app.hist_sel].path);
            break;
        case 't':
            if (app.hist_count > 0) {
                HistoryEntry* entry = &app.history[app.hist_sel];
                if (!open_from_history(entry->path))
                    break;
                restore_cursor_position(entry->cursor);
                fm_edit_init();
                MODE_PUSH(MODE_FM_EDIT);
            }
            break;
        case 'd':
            if (app.hist_count > 0) {
                char* path = dawn_strdup(app.history[app.hist_sel].path);
                hist_remove(path);
                free(path);
                if (app.hist_sel >= app.hist_count && app.hist_sel > 0)
                    app.hist_sel--;
            }
            break;
        }
        break;

    case MODE_WRITING:
#if HAS_LIBAI
        if (!app.ai_open)
            chat_picker.open = false;
#endif
        if (app.ai_open && key == '\t') {
            // Over the note there is nothing to hand the focus to; esc brings the note back.
            if (!calc_layout().ai_overlay)
                app.ai_focused = !app.ai_focused;
            break;
        }
        // A tap or swipe is routed by where it landed, not by which pane has focus: a tap
        // anywhere focuses that pane, and a swipe scrolls whichever pane it is over (dawn_touch).
        if (key == DAWN_KEY_MOUSE_CLICK) {
            handle_mouse_click();
            break;
        }
        if (key == DAWN_KEY_MOUSE_DRAG) {
            handle_mouse_drag();
            break;
        }
        if (key == DAWN_KEY_MOUSE_RELEASE) {
            handle_mouse_release();
            break;
        }
        if (key == DAWN_KEY_MOUSE_SCROLL_UP || key == DAWN_KEY_MOUSE_SCROLL_DOWN) {
            handle_mouse_scroll(key);
            break;
        }
        if (app.ai_open && app.ai_focused) {
            handle_ai_input(key);
            // Warm the meaning index's query while the question is typed (sent once typing
            // pauses, never waited on), so the snapshot's relevant passages are ready at Enter.
            EmbedHit warm[1];
            int32_t warm_n;
            if (app.ai_input_len >= 3 && app.session_path && embed_ready())
                embed_relevant(app.session_path, app.ai_input, app.ai_input_len, warm, 1, &warm_n);
        } else {
            handle_writing(key);
        }
        break;

    case MODE_FINISHED:
        switch (key) {
        case 'q':
            app.quit = 1;
            break;
        case '\x1b':
            app.mode = MODE_WELCOME;
            break;
        case '\r':
        case '\n':
            new_session();
            break;
        case 'o':
            if (app.session_path)
                open_in_finder(app.session_path);
            break;
        case 'c':
            app.mode = MODE_WRITING;
            app.timer_on = false;
            break;
        case '/':
        case 31:
#if HAS_LIBAI
            if (app.ai_ready && session_chat_may_open()) {
                app.mode = MODE_WRITING;
                app.ai_open = true;
                app.ai_focused = true;
                if (!app.ai_session)
                    ai_init_session();
                session_chat_opened();
            }
#endif
            break;
        }
        break;

    case MODE_FM_EDIT: {
        int32_t fi = app.fm_edit.current_field;
        FmEditField* field = (fi >= 0 && fi < app.fm_edit.field_count) ? &app.fm_edit.fields[fi] : NULL;

        if (app.fm_edit.adding_field) {
            switch (key) {
            case '\x1b':
                app.fm_edit.adding_field = false;
                break;
            case '\r':
            case '\n':
                if (app.fm_edit.new_key_len > 0 && app.fm_edit.field_count < FM_EDIT_MAX_FIELDS) {
                    FmEditField* nf = &app.fm_edit.fields[app.fm_edit.field_count++];
                    memset(nf, 0, sizeof(*nf));
                    app.fm_edit.new_key[app.fm_edit.new_key_len] = '\0';
                    memcpy(nf->key, app.fm_edit.new_key, app.fm_edit.new_key_len + 1);
                    nf->kind = FM_FIELD_STRING;
                    app.fm_edit.current_field = app.fm_edit.field_count - 1;
                }
                app.fm_edit.adding_field = false;
                break;
            case 127:
            case '\b':
                if (app.fm_edit.new_key_len > 0)
                    app.fm_edit.new_key_len--;
                break;
            default:
                if (key >= 32 && key < 127 && app.fm_edit.new_key_len < 62)
                    app.fm_edit.new_key[app.fm_edit.new_key_len++] = (char)key;
                break;
            }
            break;
        }

        // For string fields, handle up/down/enter specially for multi-line editing
        bool handled_by_string = false;
        if (field && field->kind == FM_FIELD_STRING) {
            FmFieldString* str = &field->str;
            if (key == '\r' || key == '\n') {
                // Enter inserts newline
                if (str->len < FM_EDIT_VALUE_SIZE - 1) {
                    memmove(str->value + str->cursor + 1, str->value + str->cursor, str->len - str->cursor);
                    str->value[str->cursor++] = '\n';
                    str->len++;
                }
                handled_by_string = true;
            } else if (key == DAWN_KEY_UP || key == DAWN_KEY_DOWN) {
                // Navigate wrapped lines - find current line bounds and move
                size_t key_len = strlen(field->key);
                int32_t wrap_width = 70 - 4 - (int32_t)key_len - 3;
                if (wrap_width < 10)
                    wrap_width = 10;
                WrapResult wr;
                wrap_init(&wr);
                wrap_string(str->value, str->len, wrap_width, &wr);
                // Find which line the cursor is on - same logic as main editor
                int32_t cur_line = 0;
                size_t col_in_line = 0;
                for (int32_t ln = 0; ln < wr.count; ln++) {
                    WrapLine* wl = &wr.lines[ln];
                    if (str->cursor >= wl->start && str->cursor <= wl->end) {
                        cur_line = ln;
                        col_in_line = str->cursor - wl->start;
                        break;
                    }
                    if (str->cursor < wl->start) {
                        cur_line = ln > 0 ? ln - 1 : 0;
                        col_in_line = str->cursor - wr.lines[cur_line].start;
                        break;
                    }
                    cur_line = ln;
                    col_in_line = str->cursor - wl->start;
                }
                if (str->cursor >= str->len && wr.count > 0) {
                    cur_line = wr.count - 1;
                    col_in_line = str->cursor - wr.lines[cur_line].start;
                }
                int32_t target_line = key == DAWN_KEY_UP ? cur_line - 1 : cur_line + 1;
                if (target_line >= 0 && target_line < wr.count) {
                    WrapLine* tl = &wr.lines[target_line];
                    size_t line_len = tl->end - tl->start;
                    size_t target_col = col_in_line <= line_len ? col_in_line : line_len;
                    str->cursor = tl->start + target_col;
                    handled_by_string = true;
                }
                wrap_free(&wr);
            }
        }

        if (handled_by_string)
            break;

        switch (key) {
        case '\x1b':
            MODE_POP();
            break;
        case '\r':
        case '\n':
            fm_edit_save();
            save_session();
            update_title();
            MODE_POP();
            break;
        case 19: // Ctrl+S - save
            fm_edit_save();
            save_session();
            update_title();
            MODE_POP();
            break;
        case '\t':
            if (app.fm_edit.field_count > 0)
                app.fm_edit.current_field = (fi + 1) % app.fm_edit.field_count;
            break;
        case DAWN_KEY_BTAB:
            if (app.fm_edit.field_count > 0)
                app.fm_edit.current_field = (fi - 1 + app.fm_edit.field_count) % app.fm_edit.field_count;
            break;
        case DAWN_KEY_UP:
            if (fi > 0)
                app.fm_edit.current_field--;
            break;
        case DAWN_KEY_DOWN:
            if (fi < app.fm_edit.field_count - 1)
                app.fm_edit.current_field++;
            break;
        case '+':
            if (!field || field->kind != FM_FIELD_DATETIME) {
                app.fm_edit.adding_field = true;
                app.fm_edit.new_key_len = 0;
            }
            break;
        default:
            if (!field)
                break;
            switch (field->kind) {
            case FM_FIELD_BOOL:
                if (key == ' ')
                    field->boolean.value = !field->boolean.value;
                break;

            case FM_FIELD_DATETIME: {
                FmFieldDatetime* dt = &field->datetime;
                int32_t max_part = dt->d.has_time ? 5 : 2;
                switch (key) {
                case '<':
                case DAWN_KEY_LEFT:
                    if (dt->part > 0)
                        dt->part--;
                    break;
                case '>':
                case DAWN_KEY_RIGHT:
                    if (dt->part < max_part)
                        dt->part++;
                    break;
                case '-':
                case '_':
                    switch (dt->part) {
                    case 0:
                        if (dt->d.year > 1900)
                            dt->d.year--;
                        break;
                    case 1:
                        dt->d.mon = dt->d.mon > 1 ? dt->d.mon - 1 : 12;
                        break;
                    case 2:
                        dt->d.mday = dt->d.mday > 1 ? dt->d.mday - 1 : 28;
                        break;
                    case 3:
                        dt->d.hour = dt->d.hour > 0 ? dt->d.hour - 1 : 23;
                        break;
                    case 4:
                        dt->d.min = dt->d.min > 0 ? dt->d.min - 1 : 59;
                        break;
                    case 5:
                        dt->d.sec = dt->d.sec > 0 ? dt->d.sec - 1 : 59;
                        break;
                    }
                    break;
                case '=':
                case '+':
                    switch (dt->part) {
                    case 0:
                        dt->d.year++;
                        break;
                    case 1:
                        dt->d.mon = dt->d.mon < 12 ? dt->d.mon + 1 : 1;
                        break;
                    case 2:
                        dt->d.mday = dt->d.mday < 28 ? dt->d.mday + 1 : 1;
                        break;
                    case 3:
                        dt->d.hour = dt->d.hour < 23 ? dt->d.hour + 1 : 0;
                        break;
                    case 4:
                        dt->d.min = dt->d.min < 59 ? dt->d.min + 1 : 0;
                        break;
                    case 5:
                        dt->d.sec = dt->d.sec < 59 ? dt->d.sec + 1 : 0;
                        break;
                    }
                    break;
                }
                break;
            }

            case FM_FIELD_LIST: {
                FmFieldList* lst = &field->list;
                switch (key) {
                // Ctrl+arrows: switch between items
                case DAWN_KEY_CTRL_LEFT:
                case DAWN_KEY_ALT_LEFT:
                    if (lst->selected > 0)
                        lst->selected--;
                    lst->cursor = lst->item_lens[lst->selected];
                    break;
                case DAWN_KEY_CTRL_RIGHT:
                case DAWN_KEY_ALT_RIGHT:
                    if (lst->selected < lst->count - 1)
                        lst->selected++;
                    lst->cursor = lst->item_lens[lst->selected];
                    break;
                // Regular arrows: move cursor within item
                case DAWN_KEY_LEFT:
                    if (lst->cursor > 0)
                        lst->cursor--;
                    break;
                case DAWN_KEY_RIGHT:
                    if (lst->selected < lst->count && lst->cursor < lst->item_lens[lst->selected])
                        lst->cursor++;
                    break;
                case DAWN_KEY_HOME:
                    lst->cursor = 0;
                    break;
                case DAWN_KEY_END:
                    if (lst->selected < lst->count)
                        lst->cursor = lst->item_lens[lst->selected];
                    break;
                // Ctrl+N: add new item
                case 14: // Ctrl+N
                    if (lst->count < FM_EDIT_MAX_LIST_ITEMS) {
                        lst->items[lst->count][0] = '\0';
                        lst->item_lens[lst->count] = 0;
                        lst->selected = lst->count++;
                        lst->cursor = 0;
                    }
                    break;
                // Ctrl+D or backspace on empty: delete item
                case 4: // Ctrl+D
                    if (lst->count > 0) {
                        for (int32_t i = lst->selected; i < lst->count - 1; i++) {
                            memcpy(lst->items[i], lst->items[i + 1], FM_EDIT_VALUE_SIZE);
                            lst->item_lens[i] = lst->item_lens[i + 1];
                        }
                        lst->count--;
                        if (lst->selected >= lst->count && lst->selected > 0)
                            lst->selected--;
                        lst->cursor = lst->count > 0 ? lst->item_lens[lst->selected] : 0;
                    }
                    break;
                case 127:
                case '\b':
                    if (lst->cursor > 0 && lst->selected < lst->count) {
                        memmove(lst->items[lst->selected] + lst->cursor - 1,
                            lst->items[lst->selected] + lst->cursor,
                            lst->item_lens[lst->selected] - lst->cursor);
                        lst->cursor--;
                        lst->item_lens[lst->selected]--;
                    } else if (lst->cursor == 0 && lst->item_lens[lst->selected] == 0 && lst->count > 0) {
                        // Backspace on empty item: delete it
                        for (int32_t i = lst->selected; i < lst->count - 1; i++) {
                            memcpy(lst->items[i], lst->items[i + 1], FM_EDIT_VALUE_SIZE);
                            lst->item_lens[i] = lst->item_lens[i + 1];
                        }
                        lst->count--;
                        if (lst->selected >= lst->count && lst->selected > 0)
                            lst->selected--;
                        lst->cursor = lst->count > 0 ? lst->item_lens[lst->selected] : 0;
                    }
                    break;
                default:
                    if (key >= 32 && key < 127 && lst->selected < lst->count && lst->item_lens[lst->selected] < FM_EDIT_VALUE_SIZE - 1) {
                        memmove(lst->items[lst->selected] + lst->cursor + 1,
                            lst->items[lst->selected] + lst->cursor,
                            lst->item_lens[lst->selected] - lst->cursor);
                        lst->items[lst->selected][lst->cursor++] = (char)key;
                        lst->item_lens[lst->selected]++;
                    }
                    break;
                }
                break;
            }

            case FM_FIELD_STRING:
            default: {
                FmFieldString* str = &field->str;
                switch (key) {
                case 127:
                case '\b':
                    if (str->cursor > 0) {
                        memmove(str->value + str->cursor - 1, str->value + str->cursor, str->len - str->cursor);
                        str->cursor--;
                        str->len--;
                    }
                    break;
                case DAWN_KEY_DEL:
                    if (str->cursor < str->len) {
                        memmove(str->value + str->cursor, str->value + str->cursor + 1, str->len - str->cursor - 1);
                        str->len--;
                    }
                    break;
                case DAWN_KEY_LEFT:
                    if (str->cursor > 0)
                        str->cursor--;
                    break;
                case DAWN_KEY_RIGHT:
                    if (str->cursor < str->len)
                        str->cursor++;
                    break;
                case DAWN_KEY_HOME:
                    str->cursor = 0;
                    break;
                case DAWN_KEY_END:
                    str->cursor = str->len;
                    break;
                default:
                    if (key >= 32 && key < 127 && str->len < FM_EDIT_VALUE_SIZE - 1) {
                        memmove(str->value + str->cursor + 1, str->value + str->cursor, str->len - str->cursor);
                        str->value[str->cursor++] = (char)key;
                        str->len++;
                    }
                    break;
                }
                break;
            }
            }
            break;
        }
        break;
    }

    case MODE_BLOCK_EDIT:
        if (app.block_edit.type == BLOCK_IMAGE) {
            // Image block editor input handling
            switch (key) {
            case '\x1b':
                MODE_POP();
                break;
            case '\r':
            case '\n': {
                char new_syntax[2048];
                Block* block = get_block_at(app.block_edit.pos);
                if (block && block->type == BLOCK_IMAGE) {
                    // Get path from block (not editable)
                    size_t path_s = block->data.image.path_start;
                    size_t path_l = block->data.image.path_len;
                    char path[1024];
                    for (size_t i = 0; i < path_l && i < sizeof(path) - 1; i++)
                        path[i] = gap_at(&app.text, path_s + i);
                    path[path_l < sizeof(path) ? path_l : sizeof(path) - 1] = '\0';

                    // Use edited values from buffers
                    app.block_edit.image.alt[app.block_edit.image.alt_len] = '\0';
                    app.block_edit.image.title[app.block_edit.image.title_len] = '\0';

                    int32_t w_val = 0, h_val = 0;
                    if (app.block_edit.image.width_len > 0) {
                        app.block_edit.image.width[app.block_edit.image.width_len] = '\0';
                        w_val = atoi(app.block_edit.image.width);
                        if (app.block_edit.image.width_pct)
                            w_val = -w_val;
                    }
                    if (app.block_edit.image.height_len > 0) {
                        app.block_edit.image.height[app.block_edit.image.height_len] = '\0';
                        h_val = atoi(app.block_edit.image.height);
                        if (app.block_edit.image.height_pct)
                            h_val = -h_val;
                    }

                    // Build syntax: ![alt](path "title"){ width height }
                    int32_t len;
                    if (app.block_edit.image.title_len > 0)
                        len = snprintf(new_syntax, sizeof(new_syntax), "![%s](%s \"%s\")",
                            app.block_edit.image.alt, path, app.block_edit.image.title);
                    else
                        len = snprintf(new_syntax, sizeof(new_syntax), "![%s](%s)",
                            app.block_edit.image.alt, path);

                    if (w_val != 0 || h_val != 0) {
                        len += snprintf(new_syntax + len, sizeof(new_syntax) - (size_t)len, "{ ");
                        if (w_val != 0) {
                            if (w_val < 0)
                                len += snprintf(new_syntax + len, sizeof(new_syntax) - (size_t)len, "width=%d%%", -w_val);
                            else
                                len += snprintf(new_syntax + len, sizeof(new_syntax) - (size_t)len, "width=%dpx", w_val);
                        }
                        if (h_val != 0) {
                            if (w_val != 0)
                                len += snprintf(new_syntax + len, sizeof(new_syntax) - (size_t)len, " ");
                            if (h_val < 0)
                                len += snprintf(new_syntax + len, sizeof(new_syntax) - (size_t)len, "height=%d%%", -h_val);
                            else
                                len += snprintf(new_syntax + len, sizeof(new_syntax) - (size_t)len, "height=%dpx", h_val);
                        }
                        len += snprintf(new_syntax + len, sizeof(new_syntax) - (size_t)len, " }");
                    }

                    // Preserve trailing newline if block had one
                    if (app.block_edit.len > 0 && gap_at(&app.text, app.block_edit.pos + app.block_edit.len - 1) == '\n') {
                        new_syntax[len++] = '\n';
                    }

                    save_undo_state();
                    gap_delete(&app.text, app.block_edit.pos, app.block_edit.len);
                    gap_insert_str(&app.text, app.block_edit.pos, new_syntax, (size_t)len);
                    app.cursor = app.block_edit.pos;
                }
                MODE_POP();
                break;
            }
            case '\t':
                app.block_edit.field = (app.block_edit.field + 1) % 4;
                break;
            case 'p':
            case 'P':
                // Only toggle %/px for width/height fields
                if (app.block_edit.field == 2)
                    app.block_edit.image.width_pct = !app.block_edit.image.width_pct;
                else if (app.block_edit.field == 3)
                    app.block_edit.image.height_pct = !app.block_edit.image.height_pct;
                else
                    goto img_edit_char; // 'p' is valid text for alt/title
                break;
            case 127:
            case '\b':
                switch (app.block_edit.field) {
                case 0:
                    if (app.block_edit.image.alt_len > 0)
                        app.block_edit.image.alt_len--;
                    break;
                case 1:
                    if (app.block_edit.image.title_len > 0)
                        app.block_edit.image.title_len--;
                    break;
                case 2:
                    if (app.block_edit.image.width_len > 0)
                        app.block_edit.image.width_len--;
                    break;
                case 3:
                    if (app.block_edit.image.height_len > 0)
                        app.block_edit.image.height_len--;
                    break;
                }
                break;
            default:
            img_edit_char:
                if (app.block_edit.field <= 1) {
                    // Alt/Title: accept printable characters except special md chars
                    if (key >= 32 && key < 127 && key != '"' && key != '[' && key != ']') {
                        if (app.block_edit.field == 0 && app.block_edit.image.alt_len < sizeof(app.block_edit.image.alt) - 1)
                            app.block_edit.image.alt[app.block_edit.image.alt_len++] = (char)key;
                        else if (app.block_edit.field == 1 && app.block_edit.image.title_len < sizeof(app.block_edit.image.title) - 1)
                            app.block_edit.image.title[app.block_edit.image.title_len++] = (char)key;
                    }
                } else if (key >= '0' && key <= '9') {
                    // Width/Height: digits only
                    if (app.block_edit.field == 2 && app.block_edit.image.width_len < sizeof(app.block_edit.image.width) - 1)
                        app.block_edit.image.width[app.block_edit.image.width_len++] = (char)key;
                    else if (app.block_edit.field == 3 && app.block_edit.image.height_len < sizeof(app.block_edit.image.height) - 1)
                        app.block_edit.image.height[app.block_edit.image.height_len++] = (char)key;
                }
                break;
            }
        }
        // Future: else if (app.block_edit.type == BLOCK_CODE) { ... }
        break;

    case MODE_CONFLICT: {
        // r / o / k (or esc), or a tap on one of the three rows.
        int32_t choice = -1;
        if (key == DAWN_KEY_MOUSE_CLICK)
            choice = render_conflict_hit(input_last_mouse_row(), input_last_mouse_col());
        else if (key == 'r' || key == 'R')
            choice = CONFLICT_RELOAD;
        else if (key == 'o' || key == 'O')
            choice = CONFLICT_OVERWRITE;
        else if (key == 'k' || key == 'K' || key == '\x1b')
            choice = CONFLICT_KEEP;
        if (choice == CONFLICT_RELOAD) {
            if (note_conflict_reload())
                MODE_POP();
        } else if (choice == CONFLICT_OVERWRITE) {
            // A file changed yet again meanwhile is a new conflict: the dialog stays, asking anew.
            if (note_conflict_overwrite() || !app.save_paused)
                MODE_POP();
            app.conflict_prompt = false;
        } else if (choice == CONFLICT_KEEP) {
            // Saving stays paused ("saving paused" on the status bar). Autosaves keep the copy of
            // the writer's text current without asking; esc asks again before leaving.
            MODE_POP();
        }
        break;
    }

    case MODE_HELP: {
        // A second page (Tab or -> to get there, <- back) lists notices newest-first, so a
        // failed save or an AI edit can be found again without adding anything to the page itself.
        // A third, where the meaning index is compiled in, shows what it is doing and has its
        // switch and its rebuild. Any other key closes help.
        int32_t last_page = DAWN_EMBED_LIVE ? 2 : 1;
        if (key == '\t' || key == DAWN_KEY_RIGHT) {
            if (app.help_page < last_page)
                app.help_page++;
        } else if (key == DAWN_KEY_LEFT) {
            if (app.help_page > 0)
                app.help_page--;
        } else if (app.help_page != 2 || !help_meaning_key(key)) {
            MODE_POP();
        }
        if (app.help_page != 2)
            app.help_rebuild_armed = 0;
        break;
    }

    case MODE_TOC: {
        TocState* toc = (TocState*)app.toc_state;
        if (!toc) {
            MODE_POP();
            break;
        }

        switch (key) {
        case '\x1b':
            MODE_POP();
            break;
        case '\r':
        case '\n': {
            const TocEntry* entry = toc_get_selected(toc);
            if (entry) {
                app.cursor = entry->pos;
                app.selecting = false;
            }
            clear_screen();
            MODE_POP();
        } break;
        case DAWN_KEY_UP:
        case 'k':
            if (toc->selected > 0)
                toc->selected--;
            break;
        case DAWN_KEY_DOWN:
        case 'j':
            if (toc->selected < toc->filtered_count - 1)
                toc->selected++;
            break;
        case DAWN_KEY_PGUP:
            toc->selected -= 10;
            if (toc->selected < 0)
                toc->selected = 0;
            break;
        case DAWN_KEY_PGDN:
            toc->selected += 10;
            if (toc->selected >= toc->filtered_count)
                toc->selected = toc->filtered_count - 1;
            if (toc->selected < 0)
                toc->selected = 0;
            break;
        case 127:
        case '\b':
            if (toc->filter_len > 0) {
                toc->filter_len = str_utf8_prev(toc->filter, toc->filter_len);
                toc->filter[toc->filter_len] = '\0';
                toc_filter(toc);
            }
            break;
        default:
            if (IS_PRINTABLE(key)) {
                size_t len = (size_t)toc->filter_len;
                if (str_append_codepoint(toc->filter, sizeof(toc->filter), &len, key)) {
                    toc->filter_len = (int32_t)len;
                    toc_filter(toc);
                }
            }
            break;
        }
    } break;

    case MODE_SEARCH: {
        SearchState* search = (SearchState*)app.search_state;
        if (!search) {
            MODE_POP();
            break;
        }

        // A tap on a result row picks it as Enter would; anywhere else does nothing.
        if (key == DAWN_KEY_MOUSE_CLICK) {
            int32_t hit = render_search_hit(input_last_mouse_row(), input_last_mouse_col());
            if (hit < 0)
                break;
            search->selected = hit;
            key = '\r';
        }

        switch (key) {
        case '\x1b':
            MODE_POP();
            break;
        case '\r':
        case '\n': {
            const SearchResult* r = search_get_selected(search);
            int32_t m = search->selected - search->count;
            if (r) {
                app.cursor = r->pos;
                app.selecting = false;
            } else if (m >= search->meaning_here && m < search->meaning_count) {
                // Another note's row: that note opens at the piece.
                open_note_at(search->meaning[m].path, search->meaning[m].start);
                break;
            } else if (m >= 0 && m < search->meaning_count) {
                // A "by meaning" row: the piece's start, checked still current when ranked.
                size_t at = search->meaning[m].start;
                app.cursor = at <= gap_len(&app.text) ? at : gap_len(&app.text);
                app.selecting = false;
            }
            clear_screen();
            MODE_POP();
        } break;
        case DAWN_KEY_UP:
        case 16: // Up or Ctrl+P
            if (search->selected > 0)
                search->selected--;
            break;
        case DAWN_KEY_DOWN:
        case 14: // Down or Ctrl+N
            if (search->selected < search->count + search->meaning_count - 1)
                search->selected++;
            break;
        case DAWN_KEY_PGUP:
            search->selected -= 10;
            if (search->selected < 0)
                search->selected = 0;
            break;
        case DAWN_KEY_PGDN:
            search->selected += 10;
            if (search->selected >= search->count + search->meaning_count)
                search->selected = search->count + search->meaning_count - 1;
            if (search->selected < 0)
                search->selected = 0;
            break;
        case 127:
        case '\b':
            if (search->query_len > 0) {
                search->query_len = str_utf8_prev(search->query, search->query_len);
                search->query[search->query_len] = '\0';
                search_mark_dirty(search, DAWN_BACKEND(app)->clock(DAWN_CLOCK_MS));
            }
            break;
        default:
            if (IS_PRINTABLE(key)) {
                size_t len = (size_t)search->query_len;
                if (str_append_codepoint(search->query, SEARCH_MAX_QUERY, &len, key)) {
                    search->query_len = (int32_t)len;
                    search_mark_dirty(search, DAWN_BACKEND(app)->clock(DAWN_CLOCK_MS));
                }
            }
            break;
        }
    } break;
    }
}

// #endregion

// #region Engine API

bool dawn_engine_init(int8_t theme_override, int32_t timer_override)
{
    // Header rows in the block cache follow the terminal: scaled only with OSC 66 text sizing.
    block_set_text_scaling(HAS_CAP(DAWN_CAP_TEXT_SIZING));
    app.timer_mins = DEFAULT_TIMER_MINUTES;
    app.mode = MODE_WELCOME;
    app.theme = THEME_DARK;
    app.style = STYLE_MINIMAL;

    for (size_t i = 0; i < NUM_PRESETS; i++) {
        if (TIMER_PRESETS[i] == DEFAULT_TIMER_MINUTES) {
            app.preset_idx = (int32_t)i;
            break;
        }
    }

    // Apply persisted preferences. CLI overrides win over what's on disk
    // so user-supplied flags are honored even if settings exist.
    {
        // Nerd Font icons by default only where the font is known to be installed: the launcher
        // sets TERM_PROGRAM=termux-launcher. settings.json's "nerd_font" overrides either way.
        const char* prog = getenv("TERM_PROGRAM");
        app.nerd_font = prog && strcmp(prog, "termux-launcher") == 0;
    }
    app.meaning_index = true; // on unless settings.json says "meaning_index": false
    settings_load();
    embed_set_enabled(app.meaning_index); // before embed_start(), which then stays quiet when off
    if (theme_override >= 0) {
        app.theme = (Theme)theme_override;
    }
    if (timer_override >= 0) {
        app.timer_mins = timer_override;
        app.preset_idx = 0;
        for (size_t i = 0; i < NUM_PRESETS; i++) {
            if (TIMER_PRESETS[i] == timer_override) {
                app.preset_idx = (int32_t)i;
                break;
            }
        }
    }

    gap_init(&app.text, 4096);
    hist_load();

    app.block_cache = malloc(sizeof(BlockCache));
    if (app.block_cache)
        block_cache_init((BlockCache*)app.block_cache);

    app.hl_ctx = highlight_init(app.theme == THEME_DARK);
    dawn_update_size();

#if HAS_LIBAI
    search_tool_init();
    ai_result_t init_result = ai_init();
    if (init_result == AI_SUCCESS) {
        ai_availability_t status = ai_check_availability();
        if (status == AI_AVAILABLE) {
            app.ai_ctx = ai_context_create();
            if (app.ai_ctx)
                app.ai_ready = true;
        }
    }
#endif

    {
        // Every note in the notes directory, plus notes opened from elsewhere (the history).
        const char* extra[256];
        int32_t n = 0;
        for (int32_t i = 0; i < app.hist_count && n < 256; i++)
            extra[n++] = app.history[i].path;
        embed_start(history_dir(), extra, n);
    }

    return true;
}

//! What dawn_engine_shutdown() has to tell the terminal, printed at exit: written now it would go
//! to the alternate screen, which the frontend's dawn_ctx_shutdown() is yet to leave.
static char g_exit_msg[2 * PATH_MAX + 128];

static void print_exit_message(void)
{
    if (g_exit_msg[0])
        fputs(g_exit_msg, stderr);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
static void exit_message(const char* fmt, ...)
{
    static bool registered;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_exit_msg, sizeof(g_exit_msg), fmt, ap);
    va_end(ap);
    if (!registered && atexit(print_exit_message) == 0)
        registered = true;
    else if (!registered)
        print_exit_message(); // no handler: late is better than never
}

//! Whether the note is on screen: being written, or under one of the dialogs opened over it.
static bool on_note_screen(void)
{
    switch (app.mode) {
    case MODE_WRITING:
        return true;
    case MODE_FM_EDIT:
    case MODE_HELP:
    case MODE_BLOCK_EDIT:
    case MODE_TOC:
    case MODE_SEARCH:
    case MODE_CONFLICT:
        return app.prev_mode == MODE_WRITING;
    default:
        return false;
    }
}

void dawn_engine_shutdown(void)
{
    // The save comes first: closing a Termux session sends SIGHUP and then SIGKILL 150 ms later
    // (the launcher's ShellTerminator), and stopping the meaning index can take up to a second.
    // save_session writes only what changed; an empty note that was emptied on purpose counts.
    // A note changed elsewhere is not written over: no one is left to ask, so the writer's text
    // goes to (or refreshes) the conflict copy beside it and the terminal is told where.
    if (app.session_path && on_note_screen() && !app.preview_mode) {
        if (save_session()) {
        } else if (app.save_paused && app.conflict_path) {
            exit_message("dawn: %s changed elsewhere and was not overwritten; your text is in %s\n",
                app.session_path, app.conflict_path);
        } else if (app.save_paused) {
            exit_message("dawn: %s changed elsewhere and was not overwritten; your text could not be copied\n",
                app.session_path);
        } else {
            exit_message("dawn: could not save %s\n", app.session_path);
        }
    }
    embed_shutdown(); // anything pending is lost: the note is saved, and the next scan catches up
    DAWN_BACKEND(app)->set_title(NULL);
    scrollind_shutdown();

    gap_free(&app.text);
    free(app.session_path);
    app.session_path = NULL;
    free(app.conflict_path);
    app.conflict_path = NULL;
    fm_free(app.frontmatter);
    app.frontmatter = NULL;
    chat_clear();

    if (app.history) {
        for (int32_t i = 0; i < app.hist_count; i++) {
            free(app.history[i].path);
            free(app.history[i].title);
            free(app.history[i].date_str);
        }
        free(app.history);
        app.history = NULL;
    }

    for (int32_t i = 0; i < app.undo_count; i++)
        free(app.undo_stack[i].text);
    app.undo_count = 0;
    app.undo_pos = 0;

    if (app.block_cache) {
        block_cache_free((BlockCache*)app.block_cache);
        free(app.block_cache);
        app.block_cache = NULL;
    }

#if HAS_LIBAI
    if (app.ai_ctx && app.ai_session)
        ai_destroy_session(app.ai_ctx, app.ai_session);
    if (app.ai_ctx)
        ai_context_free(app.ai_ctx);
    ai_cleanup();
    search_tool_cleanup();
#endif
}

bool dawn_frame(void)
{
    if (app.quit)
        return false;
    if (DAWN_BACKEND(app)->check_quit())
        return false;
    if (DAWN_BACKEND(app)->check_resize())
        dawn_update_size();
    if (app.timer_on)
        timer_check();

    // Autosave touches the file only once something changed; a note merely opened stays as it was.
    // While a conflict pauses saving it looks again only once the text differs from the copy
    // already kept (save_session then refreshes the copy without asking, or saves when the file
    // is back to what dawn expected).
    if (app.mode == MODE_WRITING && app.dirty && !app.preview_mode) {
        int64_t now = DAWN_BACKEND(app)->clock(DAWN_CLOCK_SEC);
        if (app.last_save_time == 0)
            app.last_save_time = now;
        else if (now - app.last_save_time >= 5) {
            if (!app.save_paused || note_conflict_text_changed())
                save_session();
            app.last_save_time = now;
        }
    }

    // A change made elsewhere to a note with no unsaved edits is reloaded (dawn_file.c).
    if (app.mode == MODE_WRITING && app.session_path)
        note_watch();

    // A save found the note changed elsewhere: ask as soon as the note is on screen. Left (esc,
    // the timer) while saving is paused: say once where the writer's text is.
    if (app.conflict_prompt && app.mode == MODE_WRITING && !app.preview_mode) {
        app.conflict_prompt = false;
        MODE_PUSH(MODE_CONFLICT);
    }
    if (app.save_paused) {
        if (on_note_screen())
            app.conflict_left_told = false;
        else if (!app.conflict_left_told) {
            app.conflict_left_told = true;
            note_conflict_left();
        }
    }
#if HAS_LIBAI
    ai_pump();
    ai_tick();
#endif
    voice_tick();
    if (embed_poll()) {
        embed_news = true;
        embed_tell_failure();
    }
    handle_input();
    render();

    return true;
}

void dawn_request_quit(void) { app.quit = true; }
bool dawn_should_quit(void) { return app.quit; }

bool dawn_load_document(const char* path)
{
    if (DAWN_BACKEND(app)->file_exists(path))
        return load_file_for_editing(path);

    // A file that does not exist yet: an empty note bound to that path, plain like any file the
    // user names, and created by its first save. Its directory has to be there already.
    char* abs = note_path_for(path);
    if (!abs)
        return false;
    begin_session(abs);
    return true;
}

bool dawn_preview_document(const char* path)
{
    if (!load_file_for_editing(path))
        return false;
    app.preview_mode = true;
    app.mode = MODE_WRITING;
    app.timer_on = false;
    app.timer_mins = 0;
    return true;
}

bool dawn_print_document(const char* path)
{
    if (!load_file_for_editing(path))
        return false;
    app.preview_mode = true;
    app.mode = MODE_WRITING;
    app.timer_on = false;
    app.timer_mins = 0;
    app.ai_open = false;

    render_writing();
    return true;
}

bool dawn_print_buffer(const char* content, size_t size)
{
    if (!content || size == 0)
        return false;

    load_buffer_for_editing(content, size);
    app.preview_mode = true;
    app.mode = MODE_WRITING;
    app.timer_on = false;
    app.timer_mins = 0;
    app.ai_open = false;

    render_writing();
    return true;
}

bool dawn_preview_buffer(const char* content, size_t size)
{
    if (!content || size == 0)
        return false;

    load_buffer_for_editing(content, size);
    app.preview_mode = true;
    app.mode = MODE_WRITING;
    app.timer_on = false;
    app.timer_mins = 0;
    return true;
}

void dawn_new_document(void) { (void)new_session(); }

void dawn_save_document(void) { save_session(); }

void dawn_update_size(void)
{
    DAWN_BACKEND(app)->get_size(&app.cols, &app.rows);
}

void dawn_render(void) { render(); }

// #endregion

// #endregion

// #region Main Writing Renderer

// Forward declarations for block rendering
static void render_block(const RenderCtx* ctx, RenderState* rs, const Block* block);

// #region Inline Run Rendering Helpers

//! Render a delimiter run (**, *, `, etc.)
static void render_run_delim(const RenderCtx* ctx, RenderState* rs, const InlineRun* run)
{
    int32_t screen_row = VROW_TO_SCREEN(&ctx->L, rs->virtual_row, app.scroll_y);
    bool cursor_in_delim = CURSOR_IN_RANGE(app.cursor, run->byte_start, run->byte_end, app.hide_cursor_syntax);
    size_t dlen = run->data.delim.dlen;

    if (cursor_in_delim) {
        set_fg(get_dim());
        for (size_t i = 0; i < dlen && rs->pos < ctx->len; i++) {
            track_cursor(ctx, rs);
            if (IS_ROW_VISIBLE(&ctx->L, screen_row, ctx->max_row))
                rs->col_width += output_grapheme_advance(&app.text, &rs->pos, MD_CODE);
            else {
                size_t next;
                rs->col_width += grapheme_width_next(&app.text, rs->pos, &next);
                rs->pos = next;
            }
        }
        set_fg(get_fg());
    } else {
        rs->pos += dlen;
    }

    // Update active_style based on delimiter
    if (run->flags & INLINE_FLAG_IS_OPEN) {
        rs->active_style |= run->data.delim.delim_style;
    } else {
        rs->active_style &= ~run->data.delim.delim_style;
    }
}

//! Render an autolink run (<url>)
static void render_run_autolink(const RenderCtx* ctx, RenderState* rs, const InlineRun* run)
{
    int32_t screen_row = VROW_TO_SCREEN(&ctx->L, rs->virtual_row, app.scroll_y);
    size_t auto_total = run->byte_end - run->byte_start;
    bool cursor_in_auto = CURSOR_IN_RANGE(app.cursor, rs->pos, rs->pos + auto_total, app.hide_cursor_syntax);

    if (cursor_in_auto) {
        set_fg(get_dim());
        for (size_t i = 0; i < auto_total && rs->pos < ctx->len; i++) {
            track_cursor(ctx, rs);
            if (IS_ROW_VISIBLE(&ctx->L, screen_row, ctx->max_row))
                rs->col_width += output_grapheme_advance(&app.text, &rs->pos, MD_CODE);
            else {
                size_t next;
                rs->col_width += grapheme_width_next(&app.text, rs->pos, &next);
                rs->pos = next;
            }
        }
        set_fg(get_fg());
    } else {
        // Build the OSC 8 target: an email autolink's span is just "local@domain" (md_check_autolink
        // in dawn_md.c), so it needs a "mailto:" prefix to be a real link; a URI autolink's span
        // already is the full "scheme://..." string.
        char url[1024];
        bool is_email = (run->flags & INLINE_FLAG_IS_EMAIL) != 0;
        size_t prefix_len = is_email ? 7 : 0; // strlen("mailto:")
        size_t ulen = run->data.autolink.url_len < sizeof(url) - 1 - prefix_len
            ? run->data.autolink.url_len
            : sizeof(url) - 1 - prefix_len;
        if (is_email)
            memcpy(url, "mailto:", 7);
        gap_copy_to(&app.text, run->data.autolink.url_start, ulen, url + prefix_len);
        url[prefix_len + ulen] = '\0';
        bool use_osc8 = dawn_ctx_has(&app.ctx, DAWN_CAP_HYPERLINKS) && url_scheme_is_linkable(url);

        set_fg(get_link_color());
        set_underline(UNDERLINE_STYLE_SINGLE);
        set_underline_color(get_underline_color_token());
        if (use_osc8)
            DAWN_BACKEND(app)->link_begin(url);
        rs->pos++; // skip <
        size_t url_end = rs->pos + run->data.autolink.url_len;
        while (rs->pos < url_end && rs->pos < ctx->len) {
            if (IS_ROW_VISIBLE(&ctx->L, screen_row, ctx->max_row))
                rs->col_width += output_grapheme_advance(&app.text, &rs->pos, MD_CODE);
            else {
                size_t next;
                rs->col_width += grapheme_width_next(&app.text, rs->pos, &next);
                rs->pos = next;
            }
        }
        rs->pos++; // skip >
        if (use_osc8)
            DAWN_BACKEND(app)->link_end();
        set_underline(0);
        set_fg(get_fg());
    }
}

//! Render an HTML entity run (&amp; etc.)
static void render_run_entity(const RenderCtx* ctx, RenderState* rs, const InlineRun* run)
{
    int32_t screen_row = VROW_TO_SCREEN(&ctx->L, rs->virtual_row, app.scroll_y);
    size_t entity_total = run->byte_end - run->byte_start;
    bool cursor_in_entity = CURSOR_IN_RANGE(app.cursor, rs->pos, rs->pos + entity_total, app.hide_cursor_syntax);

    if (cursor_in_entity) {
        set_fg(get_dim());
        for (size_t i = 0; i < entity_total && rs->pos < ctx->len; i++) {
            track_cursor(ctx, rs);
            if (IS_ROW_VISIBLE(&ctx->L, screen_row, ctx->max_row))
                rs->col_width += output_grapheme_advance(&app.text, &rs->pos, MD_CODE);
            else {
                size_t next;
                rs->col_width += grapheme_width_next(&app.text, rs->pos, &next);
                rs->pos = next;
            }
        }
        set_fg(get_fg());
    } else {
        if (IS_ROW_VISIBLE(&ctx->L, screen_row, ctx->max_row)) {
            out_str_n(run->data.entity.utf8, run->data.entity.utf8_len);
            rs->col_width += utf8_display_width(run->data.entity.utf8, run->data.entity.utf8_len);
        }
        rs->pos += entity_total;
    }
}

//! Render an escape sequence run (\x)
//! Returns true if the escaped char should be skipped (hard line break)
static bool render_run_escape(const RenderCtx* ctx, RenderState* rs, const InlineRun* run)
{
    int32_t screen_row = VROW_TO_SCREEN(&ctx->L, rs->virtual_row, app.scroll_y);
    bool cursor_on_backslash = CURSOR_IN_RANGE(app.cursor, rs->pos, rs->pos + 1, app.hide_cursor_syntax);

    if (cursor_on_backslash) {
        set_fg(get_dim());
        if (IS_ROW_VISIBLE(&ctx->L, screen_row, ctx->max_row))
            rs->col_width += output_grapheme_advance(&app.text, &rs->pos, MD_CODE);
        else {
            size_t next;
            rs->col_width += grapheme_width_next(&app.text, rs->pos, &next);
            rs->pos = next;
        }
        set_fg(get_fg());
    } else {
        rs->pos++; // Skip backslash
        if (run->data.escape.escaped_char == '\n') {
            return true; // Hard line break - skip
        }
    }
    return false;
}

// #endregion

static void render_writing(void)
{
    if (app.plain_mode) {
        render_writing_plain();
        return;
    }

    bool print_mode = IS_PRINT_MODE();

    Layout L = calc_layout();

    // On a screen too short for a sheet the chat is all there is while it is open: the note, its
    // images and its status bar wait underneath. Every key goes to the chat, so it holds the focus.
    if (!print_mode && L.ai_overlay) {
        app.ai_focused = true;
        image_frame_start();
        render_ai_panel(&L);
        image_frame_end();
        out_flush();
        return;
    }

    // In print mode: capture theme bg for margin fills
    if (print_mode) {
        set_bg(get_bg());
    }

    // In print mode: no screen clearing, no image frames, no AI panel
    if (!print_mode) {
        image_frame_start();
        set_bg(get_bg());
        cursor_home();

        // Clear screen
        for (int32_t r = 0; r < app.rows; r++) {
            move_to(r + 1, 1);
            set_bg(get_bg());
            for (int32_t c = 0; c < L.text_area_cols; c++)
                out_char(' ');
            if (app.ai_open && !L.ai_sheet) {
                set_bg(get_bg());
                set_fg(get_border());
                out_char(' ');
                set_bg(get_ai_bg());
                for (int32_t c = 0; c < L.ai_cols; c++)
                    out_char(' ');
            }
        }

        // Reset background to editor bg after clearing AI panel area
        set_bg(get_bg());

        if (app.style == STYLE_ELEGANT)
            set_italic(true);
        if (app.ai_open && app.ai_focused)
            set_dim(true);
    }

    size_t len = gap_len(&app.text);

    // In print mode: render all rows (no scroll bounds)
    int32_t max_row = print_mode ? INT_MAX : (L.top_margin + L.text_height);
    int32_t scroll_y = print_mode ? 0 : app.scroll_y;

    // Ensure block cache is valid
    BlockCache* bc = (BlockCache*)app.block_cache;
    if (!bc) {
        // Fallback: allocate if not present
        app.block_cache = malloc(sizeof(BlockCache));
        bc = (BlockCache*)app.block_cache;
        if (bc)
            block_cache_init(bc);
    }

    if (bc && (!bc->valid || bc->text_len != len || bc->wrap_width != L.text_width || bc->text_height != L.text_height)) {
        block_cache_parse(bc, &app.text, L.text_width, L.text_height);
    }

    // Calculate cursor virtual row using block cache (not needed in print mode but harmless)
    int32_t cursor_vrow = 0;
    if (!print_mode && bc && bc->valid && bc->count > 0) {
        if (app.cursor >= len) {
            // Cursor at end of document - position after last block
            Block* last_block = &bc->blocks[bc->count - 1];
            cursor_vrow = last_block->vrow_start + last_block->vrow_count;
        } else {
            int32_t cursor_block_idx = block_index_at_pos(bc, app.cursor);
            if (cursor_block_idx >= 0) {
                Block* cursor_block = &bc->blocks[cursor_block_idx];
                cursor_vrow = cursor_block->vrow_start + calc_cursor_vrow_in_block(cursor_block, &app.text, app.cursor, L.text_width);
            }
        }
    }

    // Adjust scroll with margin to keep cursor away from edges (skip in print mode). While the
    // view is touch-detached (app.view_detached, set by handle_mouse_scroll - dawn_touch), stay
    // put instead of snapping back to the cursor; just clamp to the document's own bounds so a
    // swipe can't scroll past the end into empty space (scroll_margin rows of allowance so the
    // last line isn't glued to the very bottom edge).
    int32_t scroll_margin = L.text_height > 10 ? 3 : 1;
    if (!print_mode) {
        if (app.view_detached) {
            int32_t total_vrows = (bc && bc->valid) ? bc->total_vrows : 0;
            int32_t max_scroll = total_vrows - L.text_height + scroll_margin;
            if (max_scroll < 0)
                max_scroll = 0;
            if (app.scroll_y > max_scroll)
                app.scroll_y = max_scroll;
        } else if (cursor_vrow < app.scroll_y + scroll_margin) {
            app.scroll_y = cursor_vrow - scroll_margin;
        } else if (cursor_vrow >= app.scroll_y + L.text_height - scroll_margin) {
            app.scroll_y = cursor_vrow - L.text_height + scroll_margin + 1;
        }
        if (app.scroll_y < 0)
            app.scroll_y = 0;
        scroll_y = app.scroll_y;
    }

    // Initialize render state
    RenderState rs = { 0 };
    rs.cursor_virtual_row = cursor_vrow;
    rs.cursor_col = L.margin + 1;

    RenderCtx ctx = { .L = L, .max_row = max_row, .len = len, .cursor_virtual_row = &rs.cursor_virtual_row, .cursor_col = &rs.cursor_col, .is_print_mode = print_mode };

    // Find first visible block using binary search (in print mode, start from 0)
    uint32_t start_block_idx = 0;
    if (!print_mode && bc && bc->valid && bc->count > 0 && scroll_y > 0) {
        Block* start_block = block_at_vrow(bc, scroll_y);
        if (start_block) {
            start_block_idx = (uint32_t)(start_block - bc->blocks);
        }
    }

    // Render blocks
    // Track running vrow to handle cursor-in-block expanding beyond calculated vrows
    int32_t running_vrow = 0;
    if (bc && bc->valid && start_block_idx > 0) {
        // Start from where previous blocks would have ended (before their blank lines)
        Block* b = &bc->blocks[start_block_idx];
        running_vrow = b->vrow_start - b->leading_blank_lines;
    }

    if (bc && bc->valid) {
        for (uint32_t bi = start_block_idx; bi < bc->count; bi++) {
            Block* block = &bc->blocks[bi];

            // Use running_vrow instead of block->vrow_start to handle expansion
            int32_t block_screen_start = VROW_TO_SCREEN(&L, running_vrow, scroll_y);

            // In print mode, don't break early - render all blocks
            if (!print_mode && block_screen_start > max_row)
                break;

            // Render leading blank lines before the block
            // Track byte positions in blank region for cursor
            size_t blank_pos = block->blank_start;
            for (int32_t bl = 0; bl < block->leading_blank_lines; bl++) {
                // Check if cursor is on this blank line
                if (app.cursor >= blank_pos && app.cursor < block->start) {
                    // Count newlines from blank_pos to cursor to see if cursor is on THIS line
                    int32_t newlines_to_cursor = 0;
                    for (size_t p = block->blank_start; p < app.cursor; p++) {
                        if (gap_at(&app.text, p) == '\n')
                            newlines_to_cursor++;
                    }
                    if (newlines_to_cursor == bl) {
                        rs.cursor_virtual_row = running_vrow;
                        // Calculate cursor column accounting for spaces/tabs
                        int32_t col_offset = 0;
                        for (size_t cp = blank_pos; cp < app.cursor; cp++) {
                            char c = gap_at(&app.text, cp);
                            if (c == '\t')
                                col_offset += 4 - (col_offset % 4);
                            else if (c == ' ')
                                col_offset++;
                        }
                        rs.cursor_col = L.margin + 1 + col_offset;
                    }
                }

                int32_t screen_row = VROW_TO_SCREEN(&L, running_vrow, scroll_y);
                if (screen_row >= L.top_margin && screen_row <= max_row) {
                    move_to(screen_row, L.margin + 1);
                    // Check if this blank line is in selection - show selection background
                    size_t sel_s, sel_e;
                    get_selection(&sel_s, &sel_e);
                    bool in_sel = has_selection() && blank_pos >= sel_s && blank_pos < sel_e;
                    if (in_sel) {
                        set_bg(get_select());
                        out_spaces(L.text_width);
                        set_bg(get_bg());
                    } else {
                        clear_line();
                    }
                }

                // Move to next blank line position
                while (blank_pos < block->start && gap_at(&app.text, blank_pos) != '\n')
                    blank_pos++;
                if (blank_pos < block->start)
                    blank_pos++; // Skip newline

                running_vrow++;
            }

            // Set render state for this block
            rs.pos = block->start;
            rs.virtual_row = running_vrow;
            rs.col_width = 0;
            rs.line_style = 0;
            rs.style_depth = 0;
            rs.active_style = 0;
            rs.in_block_math = false;

            // Render the block
            render_block(&ctx, &rs, block);

            // Update running_vrow from actual rendered rows
            running_vrow = rs.virtual_row;
        }
    }

    // Handle trailing blank lines (newlines after last block or entire document if no blocks)
    if (!print_mode) {
        size_t trailing_start = (bc && bc->valid && bc->count > 0) ? bc->blocks[bc->count - 1].end : 0;

        // Track cursor if it's in the trailing blank region (not at EOF)
        if (app.cursor >= trailing_start && app.cursor < len) {
            int32_t newlines_before_cursor = 0;
            size_t line_start = trailing_start;
            for (size_t p = trailing_start; p < app.cursor; p++) {
                if (gap_at(&app.text, p) == '\n') {
                    newlines_before_cursor++;
                    line_start = p + 1;
                }
            }
            rs.cursor_virtual_row = running_vrow + newlines_before_cursor;
            // Calculate cursor column accounting for spaces/tabs
            int32_t col_offset = 0;
            for (size_t cp = line_start; cp < app.cursor; cp++) {
                char c = gap_at(&app.text, cp);
                if (c == '\t')
                    col_offset += 4 - (col_offset % 4);
                else if (c == ' ')
                    col_offset++;
            }
            rs.cursor_col = L.margin + 1 + col_offset;
        }

        // Render trailing blank lines and increment running_vrow
        size_t sel_s, sel_e;
        get_selection(&sel_s, &sel_e);
        bool selecting = has_selection();
        for (size_t p = trailing_start; p < len; p++) {
            if (gap_at(&app.text, p) == '\n') {
                int32_t screen_row = VROW_TO_SCREEN(&L, running_vrow, scroll_y);
                if (screen_row >= L.top_margin && screen_row <= max_row) {
                    move_to(screen_row, L.margin + 1);
                    // Check if this blank line is in selection - show selection background
                    bool in_sel = selecting && p >= sel_s && p < sel_e;
                    if (in_sel) {
                        set_bg(get_select());
                        out_spaces(L.text_width);
                        set_bg(get_bg());
                    } else {
                        clear_line();
                    }
                }
                running_vrow++;
            }
        }
    }

    // Handle cursor at end of document
    if (!print_mode && app.cursor >= len) {
        Block* last = (bc && bc->valid && bc->count > 0) ? &bc->blocks[bc->count - 1] : NULL;
        // Skip override for blocks that already track cursor at their end:
        // - Headers at EOF with NO trailing newline when using scaled header renderer
        // - HR blocks (they track cursor position in raw mode)
        bool has_newline = last && last->end > 0 && gap_at(&app.text, last->end - 1) == '\n';
        bool skip = last && app.cursor == last->end && !has_newline && (last->type == BLOCK_HR || (last->type == BLOCK_HEADER && HAS_CAP(DAWN_CAP_TEXT_SIZING)));
        if (!skip) {
            rs.cursor_virtual_row = running_vrow;
            // Calculate cursor column - find start of last line and measure spaces/tabs
            size_t trailing_start = last ? last->end : 0;
            size_t line_start = trailing_start;
            for (size_t p = trailing_start; p < len; p++) {
                if (gap_at(&app.text, p) == '\n')
                    line_start = p + 1;
            }
            int32_t col_offset = 0;
            for (size_t cp = line_start; cp < len; cp++) {
                char c = gap_at(&app.text, cp);
                if (c == '\t')
                    col_offset += 4 - (col_offset % 4);
                else if (c == ' ')
                    col_offset++;
            }
            rs.cursor_col = L.margin + 1 + (col_offset > 0 ? col_offset : rs.col_width);
        }
    }

    // Re-adjust scroll based on actual rendered cursor position (skip in print mode)
    // This handles cases where cursor-in-block expands beyond cached vrows
    // Skipped while touch-detached, for the same reason as the first adjustment above.
    if (!print_mode && !app.view_detached) {
        if (rs.cursor_virtual_row < app.scroll_y + scroll_margin) {
            app.scroll_y = rs.cursor_virtual_row - scroll_margin;
            if (app.scroll_y < 0)
                app.scroll_y = 0;
        } else if (rs.cursor_virtual_row >= app.scroll_y + L.text_height - scroll_margin) {
            app.scroll_y = rs.cursor_virtual_row - L.text_height + scroll_margin + 1;
        }
    }

    reset_attrs();

    // In print mode: output final newline and restore state
    if (print_mode) {
        out_char('\n');
        out_flush();
        return;
    }

    set_bg(get_bg());

    // The note's pill, in its right margin over the text rows (none when there is no margin)
    if (L.margin > 0 && bc && bc->valid)
        scrollind_show(SCROLLIND_NOTE, L.text_area_cols, L.top_margin, L.text_height, bc->total_vrows,
            L.text_height, app.scroll_y);

    // Under a sheet the note has no status line: the chat's input line is the bottom edge.
    if (!L.ai_sheet)
        render_status_bar(&L);

    // The status panel's corner (dawn_status): the two blank rows between the text and the status
    // line, or the one between the text and a chat sheet.
    if (L.ai_sheet)
        status_area(L.note_rows, L.note_rows, L.text_area_cols, status_panel_cols(&L), false);
    else
        status_area(app.rows - 2, app.rows - 1, L.text_area_cols, status_panel_cols(&L), false);

    if (app.ai_open) {
        render_ai_panel(&L);
        if (app.ai_focused) {
            image_frame_end();
            out_flush();
            return;
        }
        reset_attrs();
    }

    image_frame_end();
    int32_t cursor_screen_row = VROW_TO_SCREEN(&L, rs.cursor_virtual_row, app.scroll_y);
    if (cursor_screen_row < L.top_margin)
        cursor_screen_row = L.top_margin;
    if (cursor_screen_row > max_row)
        cursor_screen_row = max_row;
    if (rs.cursor_col < L.margin + 1)
        rs.cursor_col = L.margin + 1;
    if (!app.view_detached)
        voice_draw_overlay(cursor_screen_row, rs.cursor_col, L.margin + L.text_width - rs.cursor_col + 1);
    move_to(cursor_screen_row, rs.cursor_col);
    cursor_visible(true);
    status_cursor(cursor_screen_row, rs.cursor_col);
}

//! Render a single block - dispatches to type-specific renderer
static void render_block(const RenderCtx* ctx, RenderState* rs, const Block* block)
{
    switch (block->type) {
    case BLOCK_IMAGE:
        render_image_element(ctx, rs, block);
        break;

    case BLOCK_HR:
        render_hr_element(ctx, rs, block);
        break;

    case BLOCK_HEADER:
        if (HAS_CAP(DAWN_CAP_TEXT_SIZING)) {
            render_header_element(ctx, rs, block);
        } else {
            // Fall through to paragraph-style rendering for non-scalable platforms
            goto render_as_paragraph;
        }
        break;

    case BLOCK_CODE:
        render_code_block_element(ctx, rs, block);
        break;

    case BLOCK_MATH:
        render_block_math_element(ctx, rs, block);
        break;

    case BLOCK_TABLE:
        render_table_element(ctx, rs, block);
        break;

    case BLOCK_BLOCKQUOTE:
    case BLOCK_LIST_ITEM:
    case BLOCK_FOOTNOTE_DEF:
    case BLOCK_PARAGRAPH:
    render_as_paragraph: {
        // Render paragraph-like blocks with inline markdown
        // This handles blockquotes, lists, footnotes, and paragraphs
        size_t len = ctx->len;
        size_t sel_s, sel_e;
        get_selection(&sel_s, &sel_e);

        rs->pos = block->start;
        // Note: rs->virtual_row is set by caller (render_writing) to running_vrow
        rs->col_width = 0;

        // Initialize run-based rendering
        rs->runs = block->inline_runs;
        rs->run_count = block->inline_run_count;
        rs->current_run_idx = 0;

        // A block quote sits on its own rung (surface_container_low): each of its rows is filled
        // across the text column below, and every return to "the page" inside it returns here.
        bool quote_surface = block->type == BLOCK_BLOCKQUOTE && !ctx->is_print_mode;
        if (quote_surface) {
            theme_surface_begin(get_quote_bg());
            set_bg(get_bg());
        }

        while (rs->pos < block->end && rs->pos < len) {
            int32_t screen_row = VROW_TO_SCREEN(&ctx->L, rs->virtual_row, app.scroll_y);
            char c = gap_at(&app.text, rs->pos);

            track_cursor(ctx, rs);

            // Handle newline
            if (c == '\n') {
                FILL_SEL_TO_EOL(ctx, rs, rs->pos, sel_s, sel_e, screen_row);
                rs->pos++;
                int32_t newline_scale = GET_LINE_SCALE(rs->line_style);
                rs->virtual_row += newline_scale;
                rs->col_width = 0;
                rs->line_style = 0;
                rs->style_depth = 0;
                rs->active_style = 0;
                reset_attrs();
                set_bg(get_bg());
                current_text_scale = 1;
                current_frac_num = 0;
                current_frac_denom = 0;
                continue;
            }

            // Check line-level elements at line start
            bool at_line_start = (rs->pos == block->start || gap_at(&app.text, rs->pos - 1) == '\n');
            if (rs->col_width == 0 && at_line_start) {
                // Set line style for headers using block data
                if (!HAS_CAP(DAWN_CAP_TEXT_SIZING) && block->type == BLOCK_HEADER) {
                    rs->line_style = block_style_for_header_level(block->data.header.level);
                }
            }

            // Find end of logical line within block
            size_t line_end = rs->pos;
            while (line_end < block->end && line_end < len && gap_at(&app.text, line_end) != '\n') {
                line_end++;
            }

            // Calculate wrap segment
            int32_t text_scale = GET_LINE_SCALE(rs->line_style);
            int32_t seg_width;
            int32_t available_width = (ctx->L.text_width - rs->col_width) / text_scale;
            if (available_width < 1)
                available_width = 1;
            size_t seg_end = gap_find_wrap_point(&app.text, rs->pos, line_end, available_width, &seg_width);

            // Render line prefixes (blockquote bars, list bullets, etc.)
            if (IS_ROW_VISIBLE(&ctx->L, screen_row, ctx->max_row)) {
                if (rs->col_width == 0) {
                    if (quote_surface) {
                        move_to(screen_row, ctx->L.margin + 1);
                        set_bg(get_bg());
                        out_spaces(ctx->L.text_width);
                    }
                    move_to(screen_row, ctx->L.margin + 1);
                }
                render_line_prefixes(ctx, rs, block, line_end, &seg_end, &seg_width);
            }

            // Render segment content with inline markdown
            while (rs->pos < seg_end && rs->pos < len) {
                screen_row = VROW_TO_SCREEN(&ctx->L, rs->virtual_row, app.scroll_y);
                if (screen_row > ctx->max_row) {
                    rs->pos = seg_end;
                    break;
                }

                track_cursor(ctx, rs);

                bool in_sel = has_selection() && rs->pos >= sel_s && rs->pos < sel_e;

                // Run-based rendering: handle special runs at their start position
                const InlineRun* run = get_current_run(rs);
                if (run && AT_RUN_START(rs, run)) {
                    switch (run->type) {
                    case RUN_DELIM:
                        render_run_delim(ctx, rs, run);
                        continue;

                    case RUN_INLINE_MATH:
                        render_inline_math(ctx, rs, run);
                        continue;

                    case RUN_LINK: {
                        render_link(ctx, rs, run);
                        // Recalculate seg_end - link may have wrapped internally
                        int32_t avail = (ctx->L.text_width - rs->col_width) / text_scale;
                        if (avail < 1)
                            avail = 1;
                        seg_end = gap_find_wrap_point(&app.text, rs->pos, line_end, avail, &seg_width);
                        continue;
                    }

                    case RUN_FOOTNOTE_REF:
                        render_footnote_ref(ctx, rs, run);
                        continue;

                    case RUN_EMOJI:
                        render_emoji(ctx, rs, run);
                        continue;

                    case RUN_AUTOLINK:
                        render_run_autolink(ctx, rs, run);
                        continue;

                    case RUN_ENTITY:
                        render_run_entity(ctx, rs, run);
                        continue;

                    case RUN_ESCAPE:
                        if (render_run_escape(ctx, rs, run))
                            continue;
                        continue;

                    case RUN_HEADING_ID:
                        // Only treat as heading ID if we're on a heading line
                        if (rs->line_style & (MD_H1 | MD_H2 | MD_H3 | MD_H4 | MD_H5 | MD_H6)) {
                            render_heading_id(ctx, rs, run);
                            continue;
                        }
                        // Fall through to text rendering if not on heading line
                        break;

                    case RUN_TEXT:
                        // Fall through to character-by-character rendering
                        break;
                    }
                }

                // Apply style and render character
                if (rs->in_block_math) {
                    set_italic(true);
                    set_fg(get_accent());
                } else if (rs->active_style) {
                    block_apply_style(rs->active_style);
                } else if (rs->line_style) {
                    block_apply_style(rs->line_style);
                } else {
                    block_apply_style(0);
                }

                // Set background: selection overrides all, otherwise preserve mark/code bg
                if (in_sel) {
                    set_bg(get_select());
                } else if (!(rs->active_style & (MD_MARK | MD_CODE))) {
                    set_bg(get_bg());
                }
                // If MD_MARK or MD_CODE, background was already set by block_apply_style

                // Read-aloud highlight and dictation glow (dawn_voice): over the markdown style,
                // under the selection.
                VoiceStyle voice;
                if (voice_style_at(rs->pos, &voice)) {
                    if (voice.has_fg)
                        set_fg(voice.fg);
                    if (voice.has_bg && !in_sel)
                        set_bg(voice.bg);
                }

                if (IS_ROW_VISIBLE(&ctx->L, screen_row, ctx->max_row)) {
                    // Use active_style to skip replacements inside inline code
                    rs->col_width += output_grapheme_advance(&app.text, &rs->pos, rs->active_style);
                } else {
                    size_t next;
                    rs->col_width += grapheme_width_next(&app.text, rs->pos, &next);
                    rs->pos = next;
                }
            }

            // End of segment - wrap to next line if needed
            if (rs->pos >= seg_end && rs->pos < line_end) {
                FILL_SEL_TO_EOL(ctx, rs, rs->pos, sel_s, sel_e, screen_row);
                // In print mode, reset background before line wrap to prevent code bg overflow
                if (ctx->is_print_mode && IS_ROW_VISIBLE(&ctx->L, screen_row, ctx->max_row)) {
                    reset_attrs();
                    set_bg(get_bg());
                }
                rs->virtual_row += text_scale;
                rs->col_width = 0;
                rs->pos = skip_leading_space(&app.text, rs->pos, line_end);
            }
        }
        if (quote_surface) {
            theme_surface_end();
            set_bg(get_bg());
        }
        break;
    }
    }
}
