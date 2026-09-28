// dawn_notice.c - ring buffer behind the status-line notice and the Ctrl+H activity list.
//
// One "current" notice (an index into the ring) drives the status line: notice_current() derives
// its fade purely from clock time, so render_status_bar() can call it every frame without this
// module tracking any redraw state of its own - dawn.c's frame-skip logic is what decides to keep
// asking (see the timed-redraw note added there alongside notice_post()'s call sites).

#include "dawn_notice.h"
#include "dawn_types.h"

#define NOTICE_MAX 50 //!< Ring capacity for the activity list (spec: "~50 entries")
#define NOTICE_TEXT_MAX 96 //!< Notices are meant to be short ("renamed · Eid plans")

// Full dim for NOTICE_FULL_DIM_MS, then a lower-contrast dim over the next NOTICE_FADE_MS -
// NOTICE_ERROR ignores both and just stays until notice_ack().
#define NOTICE_FULL_DIM_MS 3000
#define NOTICE_FADE_MS 1000
#define NOTICE_VISIBLE_MS (NOTICE_FULL_DIM_MS + NOTICE_FADE_MS)

typedef struct {
    char text[NOTICE_TEXT_MAX];
    NoticeKind kind;
    int64_t posted_sec; //!< Wall clock at post time, for the activity list's "2 min ago"
} NoticeEntry;

static struct {
    NoticeEntry ring[NOTICE_MAX];
    int32_t count; //!< Valid entries, saturates at NOTICE_MAX
    int32_t next; //!< Ring write cursor: next slot notice_post() will use

    int32_t current; //!< Ring index the status line is showing, or -1 for none
    int64_t current_posted_ms; //!< Monotonic post time of `current`, for the fade curve
    bool acked; //!< A NOTICE_ERROR at `current` was acknowledged (any key or tap)
} state = { .current = -1 };

void notice_post(NoticeKind kind, const char* text)
{
    if (!text)
        text = "";

    NoticeEntry* e = &state.ring[state.next];
    size_t n = 0;
    while (text[n] != '\0' && n < sizeof(e->text) - 1)
        n++;
    for (size_t i = 0; i < n; i++)
        e->text[i] = text[i];
    e->text[n] = '\0';
    e->kind = kind;
    e->posted_sec = DAWN_BACKEND(app)->clock(DAWN_CLOCK_SEC);

    state.current = state.next;
    state.current_posted_ms = DAWN_BACKEND(app)->clock(DAWN_CLOCK_MS);
    state.acked = false;

    state.next = (state.next + 1) % NOTICE_MAX;
    if (state.count < NOTICE_MAX)
        state.count++;
}

void notice_ack(void)
{
    state.acked = true;
}

bool notice_current(const char** text, NoticeKind* kind, float* fresh)
{
    if (state.current < 0)
        return false;
    NoticeEntry* e = &state.ring[state.current];

    if (e->kind == NOTICE_ERROR) {
        if (state.acked)
            return false;
        if (text)
            *text = e->text;
        if (kind)
            *kind = e->kind;
        if (fresh)
            *fresh = 1.0f; // full strength until acknowledged, not time-based
        return true;
    }

    int64_t now = DAWN_BACKEND(app)->clock(DAWN_CLOCK_MS);
    int64_t age = now - state.current_posted_ms;
    if (age < 0 || age >= NOTICE_VISIBLE_MS) {
        state.current = -1; // fully faded; stop asking for redraws over this
        return false;
    }

    float f = (age <= NOTICE_FULL_DIM_MS)
        ? 1.0f
        : 1.0f - (float)(age - NOTICE_FULL_DIM_MS) / (float)NOTICE_FADE_MS;

    if (text)
        *text = e->text;
    if (kind)
        *kind = e->kind;
    if (fresh)
        *fresh = f;
    return true;
}

int32_t notice_history(const char** texts, int64_t* times_sec, int32_t max)
{
    int32_t n = (state.count < max) ? state.count : max;
    for (int32_t i = 0; i < n; i++) {
        int32_t idx = state.next - 1 - i;
        while (idx < 0)
            idx += NOTICE_MAX;
        if (texts)
            texts[i] = state.ring[idx].text;
        if (times_sec)
            times_sec[i] = state.ring[idx].posted_sec;
    }
    return n;
}
