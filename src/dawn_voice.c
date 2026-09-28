// dawn_voice.c - The voice helpers' face toward dawn.c: key routing, the frame tick, the
// per-byte style, the overlay and the status word. The work is in dawn_speak.c (read-aloud)
// and dawn_dictate.c (dictation marks).

#include "dawn_voice.h"
#include "dawn_dictate.h"
#include "dawn_speak.h"

#define KEY_CTRL_Q 17 //!< Unused in the note before this; the in-app keyboard's Ctrl row sends it

static struct {
    bool motion_known; //!< reduced_motion has been read from the environment
    bool reduced_motion;
    bool styling; //!< Something may want a style this frame (fast path for voice_style_at)
} g_voice;

void voice_set_reduced_motion(bool on)
{
    g_voice.reduced_motion = on;
    g_voice.motion_known = true;
}

bool voice_reduced_motion(void)
{
    if (!g_voice.motion_known) {
        const char* env = getenv("DAWN_REDUCED_MOTION");
        g_voice.reduced_motion = env && env[0] && strcmp(env, "0") != 0;
        g_voice.motion_known = true;
    }
    return g_voice.reduced_motion;
}

bool voice_handle_key(int32_t key)
{
    if (key == DAWN_KEY_DICTATION) {
        speak_stop(); // new words on the page: whatever was being read is out of date
        dictate_drain();
        return true;
    }

    if (speak_active()) {
        switch (key) {
        case DAWN_KEY_MOUSE_SCROLL_UP:
        case DAWN_KEY_MOUSE_SCROLL_DOWN:
        case DAWN_KEY_MOUSE_DRAG:
        case DAWN_KEY_MOUSE_RELEASE:
            return false; // looking around while listening is fine
        default:
            speak_stop();
            return key == '\x1b' || key == KEY_CTRL_Q; // those only stop; others go on to dawn
        }
    }

    if (key == KEY_CTRL_Q && app.mode == MODE_WRITING && !(app.ai_open && app.ai_focused)) {
        speak_start();
        return true;
    }
    return false;
}

bool voice_tick(void)
{
    int64_t now = DAWN_BACKEND(app)->clock(DAWN_CLOCK_MS);
    bool speaking = speak_tick(now);
    bool dictating = dictate_tick(now);
    g_voice.styling = speak_active() || dictating;
    return speaking || dictating;
}

bool voice_style_at(size_t pos, VoiceStyle* out)
{
    if (!g_voice.styling || !out)
        return false;
    if (speak_style_at(pos, out))
        return true;
    return dictate_style_at(pos, out);
}

void voice_draw_overlay(int32_t row, int32_t col, int32_t cols_free)
{
    if (speak_active())
        return;
    dictate_draw_overlay(row, col, cols_free);
}

const char* voice_status_text(void)
{
    if (app.focus_mode)
        return NULL;
    if (speak_active())
        return "reading aloud";
    if (dictate_listening())
        return "listening";
    return NULL;
}
