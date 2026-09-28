// dawn_theme.c

#include "dawn_theme.h"

// #region DawnColor Palettes

//! Light theme - warm paper aesthetic
static const DawnColor LIGHT_BG = { 252, 250, 245 }; //!< Cream paper background
static const DawnColor LIGHT_FG = { 45, 45, 45 }; //!< Dark ink text
static const DawnColor LIGHT_DIM = { 160, 155, 145 }; //!< Muted annotations
static const DawnColor LIGHT_ACCENT = { 120, 100, 80 }; //!< Sepia accent
static const DawnColor LIGHT_SELECT = { 255, 245, 200 }; //!< Warm highlight
static const DawnColor LIGHT_AI_BG = { 245, 243, 238 }; //!< Subtle AI panel
static const DawnColor LIGHT_BORDER = { 220, 215, 205 }; //!< Soft borders
static const DawnColor LIGHT_CODE_BG = { 240, 238, 233 }; //!< Code block background
static const DawnColor LIGHT_MODAL_BG = { 255, 253, 250 }; //!< Modal popup background
static const DawnColor LIGHT_INPUT_BG = { 238, 235, 228 }; //!< Chat input line, one step past the panel
static const DawnColor LIGHT_ROW_SELECT = { 232, 228, 219 }; //!< Selected row in a list or modal

//! Dark theme - deep focus aesthetic
static const DawnColor DARK_BG = { 22, 22, 26 }; //!< Deep charcoal background
static const DawnColor DARK_FG = { 210, 205, 195 }; //!< Warm white text
static const DawnColor DARK_DIM = { 90, 85, 80 }; //!< Muted annotations
static const DawnColor DARK_ACCENT = { 200, 175, 130 }; //!< Golden accent
static const DawnColor DARK_SELECT = { 60, 55, 45 }; //!< Subtle highlight
static const DawnColor DARK_AI_BG = { 28, 28, 32 }; //!< Slightly lighter panel
static const DawnColor DARK_BORDER = { 50, 48, 45 }; //!< Soft borders
static const DawnColor DARK_CODE_BG = { 30, 30, 34 }; //!< Code block background
static const DawnColor DARK_MODAL_BG = { 35, 35, 40 }; //!< Modal popup background
static const DawnColor DARK_INPUT_BG = { 35, 35, 40 }; //!< Chat input line, one step past the panel
static const DawnColor DARK_ROW_SELECT = { 46, 45, 52 }; //!< Selected row in a list or modal

//! Emphasis tokens with no material palette loaded: sensible values derived from the built-in
//! cream/charcoal look rather than reusing get_accent() for everything.
static const DawnColor LIGHT_ITALIC = { 120, 90, 110 }; //!< Muted plum
static const DawnColor LIGHT_LINK = { 70, 95, 150 }; //!< Ink blue
static const DawnColor LIGHT_HIGHLIGHT_BG = { 255, 235, 180 };
static const DawnColor LIGHT_HIGHLIGHT_FG = { 60, 50, 20 };

static const DawnColor DARK_ITALIC = { 175, 140, 165 }; //!< Muted plum
static const DawnColor DARK_LINK = { 140, 170, 220 }; //!< Ink blue
static const DawnColor DARK_HIGHLIGHT_BG = { 70, 60, 25 };
static const DawnColor DARK_HIGHLIGHT_FG = { 235, 220, 170 };

// #endregion

// #region Material You Palette (~/.termux/material-colors-{dark,light}.properties)
//
// Termux:Styling writes these when the phone's wallpaper-derived Material You palette changes.
// Loaded roles map onto dawn's own tokens (see dawn-first-party-spec.html #colors); anything the
// file doesn't define falls back to the built-in constants above, and if neither properties file
// exists at all, dawn keeps its own cream/charcoal look exactly as before.
//
// The surfaces follow Material's tonal ladder, one rung per layer, the same in light and dark:
//   page                      surface
//   code blocks, block quotes surface_container_low
//   chat pane                 surface_container
//   chat input, help, modals  surface_container_high
//   selected row              surface_container_highest
// The page is deliberately not terminal_background (surface_container_lowest): the launcher never
// paints a cell whose colour equals its default background, so a page in that colour would let
// the wallpaper through.

typedef struct {
    bool loaded;
    DawnColor bg, fg, dim, accent, select, ai_bg, code_bg, modal_bg, border;
    DawnColor quote_bg, input_bg, row_select;
    DawnColor italic, link, underline, highlight_bg, highlight_fg, error;
} MaterialPalette;

static struct {
    MaterialPalette light, dark;
    int64_t light_mtime, dark_mtime; //!< 0 = file absent (or never checked)
    int64_t last_check_ms; //!< Throttles the mtime stat()s to at most once a second
    bool paths_ready;
    char light_path[1024], dark_path[1024];
} material = { 0 };

//! Parse "RRGGBB" (6 hex digits, upper or lower case) into a color. Returns false on anything else
//! (no leading '#' required from the caller - material_find_color() already stripped it).
static bool parse_hex6(const char* s, DawnColor* out)
{
    uint8_t v[3] = { 0, 0, 0 };
    for (int32_t i = 0; i < 6; i++) {
        char c = s[i];
        int32_t digit;
        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (c >= 'a' && c <= 'f')
            digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            digit = c - 'A' + 10;
        else
            return false;
        v[i / 2] = (uint8_t)((v[i / 2] << 4) | digit);
    }
    out->r = v[0];
    out->g = v[1];
    out->b = v[2];
    return true;
}

//! Find "key=#RRGGBB" (or "key=RRGGBB", or with a trailing alpha byte after the RGB) on its own
//! line in buf and parse its color. Lines are newline-separated key=value pairs, Java properties
//! style; a line starting with '#' or '!' is a comment (only meaningful if it isn't also "key=..."
//! - properties comments start the line with the marker itself, not inside a key).
static bool material_find_color(const char* buf, size_t len, const char* key, DawnColor* out)
{
    size_t key_len = strlen(key);
    size_t i = 0;
    while (i < len) {
        size_t line_start = i;
        while (i < len && buf[i] != '\n')
            i++;
        size_t line_len = i - line_start;
        i++; // skip the newline itself

        if (line_len == 0 || buf[line_start] == '#' || buf[line_start] == '!')
            continue;
        if (line_len <= key_len || strncmp(buf + line_start, key, key_len) != 0)
            continue;
        if (buf[line_start + key_len] != '=')
            continue;

        size_t val_start = line_start + key_len + 1;
        if (val_start < line_start + line_len && buf[val_start] == '#')
            val_start++;
        if (line_start + line_len - val_start < 6)
            continue;
        if (parse_hex6(buf + val_start, out))
            return true;
    }
    return false;
}

//! One RGB step lighter (dark palette) or darker (light palette), each channel held in 0..255.
//! A channel already at the limit steps the other way, so the result always differs.
static DawnColor nudge_one_step(DawnColor c, bool lighter)
{
    uint8_t* ch[3] = { &c.r, &c.g, &c.b };
    for (int32_t i = 0; i < 3; i++) {
        if (lighter)
            *ch[i] = *ch[i] < 255 ? (uint8_t)(*ch[i] + 1) : 254;
        else
            *ch[i] = *ch[i] > 0 ? (uint8_t)(*ch[i] - 1) : 1;
    }
    return c;
}

static void material_load_one(const char* path, MaterialPalette* mp, const MaterialPalette* fallback, bool dark)
{
    size_t len = 0;
    char* buf = DAWN_BACKEND(app)->read_file(path, &len);
    if (!buf) {
        mp->loaded = false;
        return;
    }

    // Each role falls back to the built-in palette passed in, one key at a time, so a partial
    // file (a role Termux:Styling hasn't started writing yet) still gets a coherent theme.
    DawnColor term_bg = fallback->bg;
    bool have_term_bg = material_find_color(buf, len, "terminal_background", &term_bg);

    // Older files carry no "surface": the terminal background is the next best page, nudged below.
    mp->bg = term_bg;
    material_find_color(buf, len, "surface", &mp->bg);
    if (have_term_bg && mp->bg.r == term_bg.r && mp->bg.g == term_bg.g && mp->bg.b == term_bg.b) {
        // One RGB step off the terminal's own background, away from the ink, is invisible to the
        // eye but enough for the launcher to paint the cell.
        mp->bg = nudge_one_step(mp->bg, dark);
    }
    mp->fg = fallback->fg;
    material_find_color(buf, len, "terminal_foreground", &mp->fg);
    mp->dim = fallback->dim;
    material_find_color(buf, len, "on_surface_variant", &mp->dim);
    mp->accent = fallback->accent;
    material_find_color(buf, len, "primary", &mp->accent);
    mp->select = fallback->select;
    material_find_color(buf, len, "terminal_selection_bg", &mp->select);
    mp->code_bg = fallback->code_bg;
    material_find_color(buf, len, "surface_container_low", &mp->code_bg);
    mp->quote_bg = mp->code_bg; // same rung, "surface_container_low"
    mp->ai_bg = fallback->ai_bg;
    material_find_color(buf, len, "surface_container", &mp->ai_bg);
    mp->modal_bg = fallback->modal_bg;
    material_find_color(buf, len, "surface_container_high", &mp->modal_bg);
    mp->input_bg = fallback->input_bg;
    material_find_color(buf, len, "surface_container_high", &mp->input_bg);
    mp->row_select = fallback->row_select;
    material_find_color(buf, len, "surface_container_highest", &mp->row_select);
    mp->border = fallback->border;
    material_find_color(buf, len, "outline_variant", &mp->border);
    mp->italic = fallback->italic;
    material_find_color(buf, len, "tertiary", &mp->italic);
    mp->link = fallback->link;
    material_find_color(buf, len, "secondary", &mp->link);
    mp->underline = mp->accent; // "primary", same key as accent
    mp->highlight_bg = fallback->highlight_bg;
    material_find_color(buf, len, "tertiary_container", &mp->highlight_bg);
    mp->highlight_fg = fallback->highlight_fg;
    material_find_color(buf, len, "on_tertiary_container", &mp->highlight_fg);
    mp->error = fallback->accent; // sensible fallback until a proof-mark feature needs it
    material_find_color(buf, len, "error", &mp->error);

    free(buf);
    mp->loaded = true;
}

static void material_init_paths(void)
{
    const char* home = DAWN_BACKEND(app)->home_dir();
    if (!home)
        home = "";
    snprintf(material.dark_path, sizeof(material.dark_path), "%s/.termux/material-colors-dark.properties", home);
    snprintf(material.light_path, sizeof(material.light_path), "%s/.termux/material-colors-light.properties", home);
    material.paths_ready = true;
}

//! Re-stat and, if changed, reload both palette files - throttled to at most once a second so the
//! frequent get_bg()/get_fg()/... calls each render don't turn into a stat() storm.
static void material_refresh(void)
{
    if (!material.paths_ready)
        material_init_paths();

    int64_t now = DAWN_BACKEND(app)->clock(DAWN_CLOCK_MS);
    if (material.last_check_ms != 0 && now - material.last_check_ms < 1000)
        return;
    material.last_check_ms = now;

    int64_t dm = DAWN_BACKEND(app)->mtime(material.dark_path);
    if (dm != material.dark_mtime) {
        material.dark_mtime = dm;
        if (dm == 0) {
            material.dark.loaded = false;
        } else {
            MaterialPalette fallback = { .bg = DARK_BG, .fg = DARK_FG, .dim = DARK_DIM, .accent = DARK_ACCENT,
                .select = DARK_SELECT, .ai_bg = DARK_AI_BG, .code_bg = DARK_CODE_BG, .modal_bg = DARK_MODAL_BG,
                .border = DARK_BORDER, .quote_bg = DARK_CODE_BG, .input_bg = DARK_INPUT_BG,
                .row_select = DARK_ROW_SELECT, .italic = DARK_ITALIC, .link = DARK_LINK,
                .highlight_bg = DARK_HIGHLIGHT_BG, .highlight_fg = DARK_HIGHLIGHT_FG };
            material_load_one(material.dark_path, &material.dark, &fallback, true);
        }
    }

    int64_t lm = DAWN_BACKEND(app)->mtime(material.light_path);
    if (lm != material.light_mtime) {
        material.light_mtime = lm;
        if (lm == 0) {
            material.light.loaded = false;
        } else {
            MaterialPalette fallback = { .bg = LIGHT_BG, .fg = LIGHT_FG, .dim = LIGHT_DIM, .accent = LIGHT_ACCENT,
                .select = LIGHT_SELECT, .ai_bg = LIGHT_AI_BG, .code_bg = LIGHT_CODE_BG, .modal_bg = LIGHT_MODAL_BG,
                .border = LIGHT_BORDER, .quote_bg = LIGHT_CODE_BG, .input_bg = LIGHT_INPUT_BG,
                .row_select = LIGHT_ROW_SELECT, .italic = LIGHT_ITALIC, .link = LIGHT_LINK,
                .highlight_bg = LIGHT_HIGHLIGHT_BG, .highlight_fg = LIGHT_HIGHLIGHT_FG };
            material_load_one(material.light_path, &material.light, &fallback, false);
        }
    }
}

static inline const MaterialPalette* material_for(Theme t)
{
    return (t == THEME_DARK) ? &material.dark : &material.light;
}

bool theme_material_active(void)
{
    material_refresh();
    return material.dark.loaded || material.light.loaded;
}

// #endregion

// #region Output Primitives

void set_fg(DawnColor c)
{
    DAWN_BACKEND(app)->set_fg(c);
}

void set_bg(DawnColor c)
{
    DAWN_BACKEND(app)->set_bg(c);
}

void move_to(int32_t r, int32_t c)
{
    DAWN_BACKEND(app)->set_cursor(c, r); // Note: backend uses (col, row) order
}

void out_str(const char* str)
{
    DAWN_BACKEND(app)->write_str(str, strlen(str));
}

void out_str_n(const char* str, size_t len)
{
    DAWN_BACKEND(app)->write_str(str, len);
}

void out_char(char c)
{
    DAWN_BACKEND(app)->write_char(c);
}

void out_spaces(int32_t n)
{
    DAWN_BACKEND(app)->repeat_char(' ', n);
}

void out_int(int32_t value)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    out_str(buf);
}

void out_flush(void)
{
    DAWN_BACKEND(app)->flush();
}

void clear_screen(void)
{
    DAWN_BACKEND(app)->clear_screen();
}

void clear_line(void)
{
    DAWN_BACKEND(app)->clear_line();
}

void clear_range(int32_t n)
{
    DAWN_BACKEND(app)->clear_range(n);
}

void cursor_visible(bool visible)
{
    DAWN_BACKEND(app)->set_cursor_visible(visible);
}

void cursor_home(void)
{
    move_to(1, 1);
}

void sync_begin(void)
{
    DAWN_BACKEND(app)->sync_begin();
}

void sync_end(void)
{
    DAWN_BACKEND(app)->sync_end();
}

void fill_line_end(DawnColor bg)
{
    // Only applies in print mode
    if (app.ctx.mode != DAWN_MODE_PRINT)
        return;

    int32_t cols = 0, rows = 0;
    DAWN_BACKEND(app)->get_size(&cols, &rows);
    if (cols <= 0)
        cols = 80;

    // Set background and fill to end of line
    set_bg(bg);
    // Use CSI K (erase to end of line) which uses current bg
    DAWN_BACKEND(app)->write_str("\x1b[K", 3);
}

// #endregion

// #region Theme Colors

//! The surface get_bg() hands out while a block quote renders (theme_surface_begin); every
//! "back to the page" set_bg(get_bg()) inside the quote then lands on the quote's rung instead.
static struct {
    bool active;
    DawnColor color;
} surface_override = { 0 };

void theme_surface_begin(DawnColor c)
{
    surface_override.active = true;
    surface_override.color = c;
}

void theme_surface_end(void)
{
    surface_override.active = false;
}

DawnColor get_bg(void)
{
    if (app.ctx.mode == DAWN_MODE_PRINT && app.ctx.host_bg) {
        return *app.ctx.host_bg;
    }
    if (surface_override.active)
        return surface_override.color;
    material_refresh();
    const MaterialPalette* m = material_for(app.theme);
    if (m->loaded)
        return m->bg;
    return app.theme == THEME_DARK ? DARK_BG : LIGHT_BG;
}
DawnColor get_fg(void)
{
    const MaterialPalette* m = material_for(app.theme);
    if (m->loaded)
        return m->fg;
    return app.theme == THEME_DARK ? DARK_FG : LIGHT_FG;
}
DawnColor get_dim(void)
{
    const MaterialPalette* m = material_for(app.theme);
    if (m->loaded)
        return m->dim;
    return app.theme == THEME_DARK ? DARK_DIM : LIGHT_DIM;
}
DawnColor get_accent(void)
{
    const MaterialPalette* m = material_for(app.theme);
    if (m->loaded)
        return m->accent;
    return app.theme == THEME_DARK ? DARK_ACCENT : LIGHT_ACCENT;
}
DawnColor get_select(void)
{
    const MaterialPalette* m = material_for(app.theme);
    if (m->loaded)
        return m->select;
    return app.theme == THEME_DARK ? DARK_SELECT : LIGHT_SELECT;
}
DawnColor get_ai_bg(void)
{
    const MaterialPalette* m = material_for(app.theme);
    if (m->loaded)
        return m->ai_bg;
    return app.theme == THEME_DARK ? DARK_AI_BG : LIGHT_AI_BG;
}
DawnColor get_border(void)
{
    const MaterialPalette* m = material_for(app.theme);
    if (m->loaded)
        return m->border;
    return app.theme == THEME_DARK ? DARK_BORDER : LIGHT_BORDER;
}
DawnColor get_code_bg(void)
{
    const MaterialPalette* m = material_for(app.theme);
    if (m->loaded)
        return m->code_bg;
    return app.theme == THEME_DARK ? DARK_CODE_BG : LIGHT_CODE_BG;
}
DawnColor get_modal_bg(void)
{
    const MaterialPalette* m = material_for(app.theme);
    if (m->loaded)
        return m->modal_bg;
    return app.theme == THEME_DARK ? DARK_MODAL_BG : LIGHT_MODAL_BG;
}
DawnColor get_quote_bg(void)
{
    const MaterialPalette* m = material_for(app.theme);
    if (m->loaded)
        return m->quote_bg;
    return app.theme == THEME_DARK ? DARK_CODE_BG : LIGHT_CODE_BG;
}
DawnColor get_input_bg(void)
{
    const MaterialPalette* m = material_for(app.theme);
    if (m->loaded)
        return m->input_bg;
    return app.theme == THEME_DARK ? DARK_INPUT_BG : LIGHT_INPUT_BG;
}
DawnColor get_row_select_bg(void)
{
    const MaterialPalette* m = material_for(app.theme);
    if (m->loaded)
        return m->row_select;
    return app.theme == THEME_DARK ? DARK_ROW_SELECT : LIGHT_ROW_SELECT;
}
DawnColor get_italic_color(void)
{
    const MaterialPalette* m = material_for(app.theme);
    if (m->loaded)
        return m->italic;
    return app.theme == THEME_DARK ? DARK_ITALIC : LIGHT_ITALIC;
}
DawnColor get_link_color(void)
{
    const MaterialPalette* m = material_for(app.theme);
    if (m->loaded)
        return m->link;
    return app.theme == THEME_DARK ? DARK_LINK : LIGHT_LINK;
}
DawnColor get_underline_color_token(void)
{
    const MaterialPalette* m = material_for(app.theme);
    if (m->loaded)
        return m->underline;
    return get_accent(); // built-in palette has no separate underline token
}
DawnColor get_highlight_bg(void)
{
    const MaterialPalette* m = material_for(app.theme);
    if (m->loaded)
        return m->highlight_bg;
    return app.theme == THEME_DARK ? DARK_HIGHLIGHT_BG : LIGHT_HIGHLIGHT_BG;
}
DawnColor get_highlight_fg(void)
{
    const MaterialPalette* m = material_for(app.theme);
    if (m->loaded)
        return m->highlight_fg;
    return app.theme == THEME_DARK ? DARK_HIGHLIGHT_FG : LIGHT_HIGHLIGHT_FG;
}
DawnColor get_error_color(void)
{
    const MaterialPalette* m = material_for(app.theme);
    if (m->loaded)
        return m->error;
    return get_accent(); // built-in palette has no separate error token (yet)
}

// #endregion

// #region DawnColor Utilities

DawnColor color_lerp(DawnColor a, DawnColor b, float t)
{
    return (DawnColor) {
        (uint8_t)(a.r + (b.r - a.r) * t),
        (uint8_t)(a.g + (b.g - a.g) * t),
        (uint8_t)(a.b + (b.b - a.b) * t)
    };
}

// #endregion

// #region Text Attributes

void set_bold(bool on)
{
    DAWN_BACKEND(app)->set_bold(on);
}

void set_italic(bool on)
{
    DAWN_BACKEND(app)->set_italic(on);
}

void set_dim(bool on)
{
    DAWN_BACKEND(app)->set_dim(on);
}

void set_strikethrough(bool on)
{
    DAWN_BACKEND(app)->set_strike(on);
}

void reset_attrs(void)
{
    DAWN_BACKEND(app)->reset_attrs();
}

// #endregion

// #region Styled Text

void set_underline(UnderlineStyle style)
{
    DAWN_BACKEND(app)->set_underline(style);
}

void set_underline_color(DawnColor c)
{
    DAWN_BACKEND(app)->set_underline_color(c);
}

void clear_underline(void)
{
    DAWN_BACKEND(app)->clear_underline();
}

// #endregion

// #region Text Sizing

void print_scaled_char(char c, int32_t scale)
{
    if (scale <= 1 || !dawn_ctx_has(&app.ctx, DAWN_CAP_TEXT_SIZING)) {
        DAWN_BACKEND(app)->write_char(c);
        return;
    }
    char str[2] = { c, '\0' };
    DAWN_BACKEND(app)->write_scaled(str, 1, scale);
}

void print_scaled_str(const char* str, size_t len, int32_t scale)
{
    if (scale <= 1 || !dawn_ctx_has(&app.ctx, DAWN_CAP_TEXT_SIZING)) {
        DAWN_BACKEND(app)->write_str(str, len);
        return;
    }
    DAWN_BACKEND(app)->write_scaled(str, len, scale);
}

void print_scaled_frac_char(char c, int32_t scale, int32_t num, int32_t denom)
{
    // No scaling needed if scale is 1 with no fractional part, or no text sizing support
    if ((scale <= 1 && (num == 0 || denom == 0)) || !dawn_ctx_has(&app.ctx, DAWN_CAP_TEXT_SIZING)) {
        DAWN_BACKEND(app)->write_char(c);
        return;
    }
    if (DAWN_BACKEND(app)->write_scaled_frac) {
        char str[2] = { c, '\0' };
        DAWN_BACKEND(app)->write_scaled_frac(str, 1, scale, num, denom);
    } else if (scale > 1) {
        // Fallback to integer scaling if fractional not supported
        char str[2] = { c, '\0' };
        DAWN_BACKEND(app)->write_scaled(str, 1, scale);
    } else {
        DAWN_BACKEND(app)->write_char(c);
    }
}

void print_scaled_frac_str(const char* str, size_t len, int32_t scale, int32_t num, int32_t denom)
{
    // No scaling needed if scale is 1 with no fractional part, or no text sizing support
    if ((scale <= 1 && (num == 0 || denom == 0)) || !dawn_ctx_has(&app.ctx, DAWN_CAP_TEXT_SIZING)) {
        DAWN_BACKEND(app)->write_str(str, len);
        return;
    }
    if (DAWN_BACKEND(app)->write_scaled_frac) {
        DAWN_BACKEND(app)->write_scaled_frac(str, len, scale, num, denom);
    } else if (scale > 1) {
        // Fallback to integer scaling if fractional not supported
        DAWN_BACKEND(app)->write_scaled(str, len, scale);
    } else {
        DAWN_BACKEND(app)->write_str(str, len);
    }
}

// #endregion
