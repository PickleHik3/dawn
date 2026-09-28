// dawn_input.c

#include "dawn_input.h"

// #region Input Reading

int32_t input_read_key(void)
{
    return DAWN_BACKEND(app)->read_key();
}

int32_t input_last_mouse_col(void)
{
    return DAWN_BACKEND(app)->mouse_col();
}

int32_t input_last_mouse_row(void)
{
    return DAWN_BACKEND(app)->mouse_row();
}

bool input_take_dictation(DawnDictEvent* out)
{
    const DawnBackend* b = DAWN_BACKEND(app);
    return b->take_dictation && b->take_dictation(out);
}

// #endregion
