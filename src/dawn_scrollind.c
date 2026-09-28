// dawn_scrollind.c - a scroll indicator that is there while the page moves and gone once it rests.
//
// Each scrollable pane (the note, the chat) gets a 3 px pill in its right margin, sized to the
// viewport's share of the content and placed pixel-exactly with kitty graphics: the image is put
// at the cell holding the pill's top pixel, offset by X/Y inside that cell. No toolbar, no track;
// without kitty graphics (or without a cell size in pixels) nothing is drawn at all.
//
// The fade is the terminal's, not ours: the pill is transmitted as a short kitty animation, frame
// 1 opaque and PILL_FADE_FRAMES more stepping its alpha down to 0. A scroll stops the animation on
// frame 1 (a=a,s=1,c=1); PILL_REST_MS after the last one it is played once (a=a,s=3,v=2) and comes
// to rest, invisible, on the last frame - so an idle page sends nothing at all.
//
// Two things about the terminal shape the mechanics. Deleting an image's last placement makes the
// launcher drop that image's animation frames (kitty does the same for unreachable images), so a
// hidden pill is freed outright (d=I) and sent again when it is next seen, and dawn's own per-frame
// clear of document images is narrowed to their z-indexes (img_keep_overlays) instead of deleting
// every placement. And animation frames decode asynchronously, so a control that names a frame
// right after transmitting it can arrive first and be ignored: the pill is therefore only ever
// (re)transmitted while it is meant to be visible, where frame 1 - the root image - is correct
// without any control at all. A pill whose length changed while it was invisible stays stale
// until the next scroll, which nobody can see.

#include "dawn_scrollind.h"
#include "dawn_theme.h"
#include "dawn_types.h"

#include <math.h>

#define PILL_WIDTH_PX 3 //!< The pill's thickness
#define PILL_FADE_FRAMES 4 //!< Frames after the opaque one, alpha stepping down to 0
#define PILL_FRAME_GAP_MS 70 //!< How long each fade frame shows
#define PILL_REST_MS 900 //!< Quiet time after the last scroll before the pill fades

//! Fixed kitty image ids, one per pane. The POSIX backend numbers document images from 1 upward,
//! so these sit far above anything it will reach.
static const uint32_t PILL_IDS[SCROLLIND_PANE_COUNT] = { 0x64A70001u, 0x64A70002u };

//! Alpha of each frame, frame 1 first
static const float PILL_ALPHA[1 + PILL_FADE_FRAMES] = { 1.0f, 0.72f, 0.45f, 0.2f, 0.0f };

typedef struct {
    bool shown; //!< scrollind_show() reached this pane in the current frame
    bool transmitted; //!< the terminal holds this pill's image and frames
    bool visible; //!< on frame 1 (a scroll happened recently); false once the fade was played
    bool have_offset; //!< offset below is from a previous frame of the same showing
    int32_t offset; //!< last frame's content offset, to notice a scroll
    int64_t last_scroll_ms; //!< when the offset last changed
    int32_t len_px; //!< the transmitted pill's length
    int32_t cell_w, cell_h; //!< the cell size it was made for
    DawnColor color; //!< the colour it was made in
} Pill;

static struct {
    Pill pills[SCROLLIND_PANE_COUNT];
    bool enabled; //!< kitty graphics and a pixel cell size, in interactive mode, this frame
    bool overlays_kept; //!< the backend was told to leave z>0 placements alone
    int32_t cell_w, cell_h; //!< this frame's cell size in pixels
} ind = { 0 };

// #region Output

static void emit(const char* s)
{
    DAWN_BACKEND(app)->write_str(s, strlen(s));
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

//! Base64 of n bytes (n a multiple of 3 except for the last chunk) into out; returns its length.
static size_t b64_encode(const uint8_t* in, size_t n, char* out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n)
            v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n)
            v |= in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = i + 1 < n ? B64[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? B64[v & 63] : '=';
    }
    return o;
}

//! One kitty graphics command carrying a payload, split into the protocol's 4096-byte chunks: the
//! first chunk has the control keys, the rest only m= (1 while more follow).
static void emit_payload(const char* keys, const uint8_t* data, size_t n)
{
    enum { RAW_CHUNK = 3072 }; // encodes to exactly 4096 bytes
    char b64[4096 + 1];
    char head[160];
    size_t pos = 0;
    bool first = true;
    do {
        size_t take = n - pos < RAW_CHUNK ? n - pos : RAW_CHUNK;
        bool more = pos + take < n;
        size_t blen = b64_encode(data + pos, take, b64);
        b64[blen] = '\0';
        if (first)
            snprintf(head, sizeof(head), "\x1b_G%s,m=%d;", keys, more ? 1 : 0);
        else
            snprintf(head, sizeof(head), "\x1b_Gm=%d;", more ? 1 : 0);
        emit(head);
        DAWN_BACKEND(app)->write_str(b64, blen);
        emit("\x1b\\");
        pos += take;
        first = false;
    } while (pos < n);
}

// #endregion

// #region Pill Image

//! Fill an RGBA pill of PILL_WIDTH_PX x h: a capsule with round ends, edge coverage antialiased,
//! at the given overall alpha.
static void pill_pixels(uint8_t* px, int32_t h, DawnColor c, float alpha)
{
    const float r = PILL_WIDTH_PX / 2.0f;
    for (int32_t y = 0; y < h; y++) {
        for (int32_t x = 0; x < PILL_WIDTH_PX; x++) {
            // Distance from the pixel's centre to the capsule's spine (the segment between the
            // centres of its two end caps), then coverage from how far inside the radius it is.
            float cx = x + 0.5f, cy = y + 0.5f;
            float top = r, bottom = h - r;
            float sy = cy < top ? top : (cy > bottom ? bottom : cy);
            float dx = cx - r, dy = cy - sy;
            float d = sqrtf(dx * dx + dy * dy);
            float cover = r + 0.5f - d;
            if (cover < 0.0f)
                cover = 0.0f;
            if (cover > 1.0f)
                cover = 1.0f;
            uint8_t* p = px + ((size_t)y * PILL_WIDTH_PX + (size_t)x) * 4;
            p[0] = c.r;
            p[1] = c.g;
            p[2] = c.b;
            p[3] = (uint8_t)(cover * alpha * 255.0f + 0.5f);
        }
    }
}

static void pill_delete(ScrollIndPane pane)
{
    Pill* p = &ind.pills[pane];
    if (!p->transmitted)
        return;
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "\x1b_Ga=d,d=I,i=%u,q=2\x1b\\", PILL_IDS[pane]);
    emit(cmd);
    p->transmitted = false;
}

//! Send the pill as image PILL_IDS[pane]: the opaque root frame, then the fade frames. It shows
//! frame 1 when placed, and the animation stays stopped until the fade is asked for.
static bool pill_transmit(ScrollIndPane pane, int32_t len_px, DawnColor color)
{
    Pill* p = &ind.pills[pane];
    size_t bytes = (size_t)len_px * PILL_WIDTH_PX * 4;
    uint8_t* px = malloc(bytes);
    if (!px)
        return false;

    pill_delete(pane);

    if (!ind.overlays_kept && DAWN_BACKEND(app)->img_keep_overlays) {
        DAWN_BACKEND(app)->img_keep_overlays(true);
        ind.overlays_kept = true;
    }

    uint32_t id = PILL_IDS[pane];
    char keys[128];
    pill_pixels(px, len_px, color, PILL_ALPHA[0]);
    snprintf(keys, sizeof(keys), "a=t,f=32,s=%d,v=%d,i=%u,q=2", PILL_WIDTH_PX, len_px, id);
    emit_payload(keys, px, bytes);

    for (int32_t f = 1; f <= PILL_FADE_FRAMES; f++) {
        pill_pixels(px, len_px, color, PILL_ALPHA[f]);
        // X=1 replaces the (empty) canvas rather than blending onto it; z is this frame's gap.
        snprintf(keys, sizeof(keys), "a=f,f=32,s=%d,v=%d,i=%u,X=1,z=%d,q=2", PILL_WIDTH_PX, len_px, id,
            PILL_FRAME_GAP_MS);
        emit_payload(keys, px, bytes);
    }
    free(px);

    // Frame 1's own gap (how long it lingers once the fade starts), and stopped on frame 1.
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "\x1b_Ga=a,i=%u,r=1,z=%d,s=1,q=2\x1b\\", id, PILL_FRAME_GAP_MS);
    emit(cmd);

    p->transmitted = true;
    p->len_px = len_px;
    p->cell_w = ind.cell_w;
    p->cell_h = ind.cell_h;
    p->color = color;
    return true;
}

// #endregion

// #region Frame

void scrollind_frame_begin(void)
{
    ind.enabled = false;
    for (int32_t i = 0; i < SCROLLIND_PANE_COUNT; i++)
        ind.pills[i].shown = false;

    if (app.ctx.mode != DAWN_MODE_INTERACTIVE || !dawn_ctx_has(&app.ctx, DAWN_CAP_IMAGES))
        return;
    if (!DAWN_BACKEND(app)->img_cell_px)
        return;
    int32_t w = 0, h = 0;
    if (!DAWN_BACKEND(app)->img_cell_px(&w, &h) || w <= PILL_WIDTH_PX || h <= 1)
        return;
    ind.cell_w = w;
    ind.cell_h = h;
    ind.enabled = true;
}

bool scrollind_available(void)
{
    return ind.enabled;
}

void scrollind_show(ScrollIndPane pane, int32_t col, int32_t track_row, int32_t track_rows,
    int32_t content, int32_t viewport, int32_t offset)
{
    // Only the writing page itself: help, the activity list, search, the TOC and the editors all
    // cover the panes, and a pill left over them would float on top.
    if (!ind.enabled || app.mode != MODE_WRITING)
        return;
    if ((int32_t)pane >= SCROLLIND_PANE_COUNT || col < 1 || track_row < 1 || track_rows < 2)
        return;
    if (content <= viewport || viewport < 1)
        return; // nothing to scroll: not shown, so scrollind_frame_end() hides it

    Pill* p = &ind.pills[pane];
    p->shown = true;
    int64_t now = DAWN_BACKEND(app)->clock(DAWN_CLOCK_MS);

    // Length: the viewport's share of the track, never shorter than a cell and a half.
    int32_t cell_h = ind.cell_h;
    int32_t track_px = track_rows * cell_h;
    int32_t len_px = (int32_t)((int64_t)track_px * viewport / content);
    int32_t min_px = cell_h * 3 / 2;
    if (len_px < min_px)
        len_px = min_px;
    if (len_px > track_px)
        len_px = track_px;

    // Position: offset's share of the travel, in pixels from the top of the screen.
    int32_t max_offset = content - viewport;
    if (offset < 0)
        offset = 0;
    if (offset > max_offset)
        offset = max_offset;
    int32_t top_px = (track_row - 1) * cell_h + (int32_t)((int64_t)(track_px - len_px) * offset / max_offset);

    // A scroll brings the pill back (stopped on its opaque frame 1) and restarts the rest timer.
    bool scrolled = p->have_offset && offset != p->offset;
    p->offset = offset;
    p->have_offset = true;
    if (scrolled) {
        p->last_scroll_ms = now;
        if (!p->visible && p->transmitted) {
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "\x1b_Ga=a,i=%u,s=1,c=1,q=2\x1b\\", PILL_IDS[pane]);
            emit(cmd);
        }
        p->visible = true;
    }

    DawnColor color = get_dim(); // on_surface_variant
    bool stale = !p->transmitted || p->len_px != len_px || p->cell_w != ind.cell_w || p->cell_h != cell_h
        || p->color.r != color.r || p->color.g != color.g || p->color.b != color.b;
    if (p->visible && stale) {
        if (!pill_transmit(pane, len_px, color))
            return;
        p->last_scroll_ms = now; // a freshly sent pill shows frame 1 for the full rest time
    }
    if (!p->transmitted)
        return; // faded and never (re)sent: nothing to show

    // Rested long enough: play the fade once. v=2 is one pass (kitty: v=N runs N-1 loops); it
    // stops on the last, transparent frame.
    if (p->visible && now - p->last_scroll_ms >= PILL_REST_MS) {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "\x1b_Ga=a,i=%u,s=3,v=2,q=2\x1b\\", PILL_IDS[pane]);
        emit(cmd);
        p->visible = false;
    }

    // Placed every frame, the same placement id each time, so it simply moves: a frame that
    // changes nothing else is byte-identical and the backend drops it whole. C=1 leaves the
    // cursor where dawn put it; z=1 is above the text, in a margin cell that holds none.
    int32_t row = top_px / cell_h + 1;
    int32_t y_off = top_px % cell_h;
    int32_t inset = ind.cell_w / 6 > 1 ? ind.cell_w / 6 : 1;
    int32_t x_off = ind.cell_w - PILL_WIDTH_PX - inset;
    if (x_off < 0)
        x_off = 0;
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "\x1b[%d;%dH\x1b_Ga=p,i=%u,p=1,X=%d,Y=%d,C=1,z=1,q=2\x1b\\", row, col,
        PILL_IDS[pane], x_off, y_off);
    emit(cmd);
}

void scrollind_frame_end(void)
{
    for (int32_t i = 0; i < SCROLLIND_PANE_COUNT; i++) {
        Pill* p = &ind.pills[i];
        if (p->shown)
            continue;
        // Covered, switched away from, or nothing to scroll: gone, and quiet when it returns.
        pill_delete((ScrollIndPane)i);
        p->have_offset = false;
        p->visible = false;
    }
}

void scrollind_forget(void)
{
    for (int32_t i = 0; i < SCROLLIND_PANE_COUNT; i++) {
        ind.pills[i].transmitted = false;
        ind.pills[i].visible = false;
    }
}

void scrollind_shutdown(void)
{
    for (int32_t i = 0; i < SCROLLIND_PANE_COUNT; i++)
        pill_delete((ScrollIndPane)i);
    DAWN_BACKEND(app)->flush();
}

// #endregion
