// dawn_image.c

#include "dawn_image.h"
#include "dawn_scrollind.h"
#include <stdlib.h>
#include <string.h>

bool image_is_supported(const char* path)
{
    return DAWN_BACKEND(app)->img_supported(path);
}

int32_t image_display_at(const char* path, int32_t row, int32_t col, int32_t max_cols, int32_t max_rows)
{
    return DAWN_BACKEND(app)->img_display(path, row, col, max_cols, max_rows);
}

int32_t image_display_at_cropped(const char* path, int32_t row, int32_t col, int32_t max_cols,
    int32_t crop_top_rows, int32_t visible_rows)
{
    return DAWN_BACKEND(app)->img_display_cropped(path, row, col, max_cols, crop_top_rows, visible_rows);
}

int32_t image_display(const char* path, int32_t max_cols, int32_t max_rows)
{
    return image_display_at(path, 0, 0, max_cols, max_rows);
}

void image_frame_start(void)
{
    DAWN_BACKEND(app)->img_frame_start();
}

void image_frame_end(void)
{
    DAWN_BACKEND(app)->img_frame_end();
}

void image_mask_region(int32_t col, int32_t row, int32_t cols, int32_t rows, DawnColor bg)
{
    DAWN_BACKEND(app)->img_mask(col, row, cols, rows, bg);
}

void image_clear_all(void)
{
    DAWN_BACKEND(app)->img_clear_all();
    // a=d,d=A took the scroll indicator's pills with it
    scrollind_forget();
}

void image_cache_invalidate(const char* path)
{
    DAWN_BACKEND(app)->img_invalidate(path);
}

bool image_get_size(const char* path, int32_t* width, int32_t* height)
{
    return DAWN_BACKEND(app)->img_size(path, width, height);
}

int32_t image_calc_rows(int32_t pixel_width, int32_t pixel_height, int32_t max_cols, int32_t max_rows)
{
    return DAWN_BACKEND(app)->img_calc_rows(pixel_width, pixel_height, max_cols, max_rows);
}

static char* image_base_dir = NULL; //!< The open note's directory, or NULL

void image_set_base_dir(const char* dir)
{
    free(image_base_dir);
    image_base_dir = (dir && dir[0]) ? strdup(dir) : NULL;
}

void image_set_base_dir_for_note(const char* note_path)
{
    if (!note_path || !note_path[0]) {
        image_set_base_dir(NULL);
        return;
    }
    const char* slash = strrchr(note_path, '/');
    if (!slash) {
        image_set_base_dir(NULL); // a bare file name: the working directory is its directory
        return;
    }
    size_t n = (size_t)(slash - note_path);
    char* dir = strndup(note_path, n == 0 ? 1 : n); // "/note.md" -> "/"
    image_set_base_dir(dir);
    free(dir);
}

//! Whether a path is relative: not absolute, not ~, not a URL scheme
static bool image_path_is_relative(const char* p)
{
    if (!p || !p[0] || p[0] == '/' || p[0] == '~')
        return false;
    for (const char* c = p; *c; c++) {
        if (*c == ':')
            return c == p; // "scheme:..." is a URL
        if (*c == '/')
            break;
    }
    return true;
}

bool image_resolve_and_cache_to(const char* raw_path, const char* base_dir, char* out, size_t out_size)
{
    if (!base_dir && image_base_dir && image_path_is_relative(raw_path))
        base_dir = image_base_dir;
    return DAWN_BACKEND(app)->img_resolve(raw_path, base_dir, out, out_size);
}
