// test_hit.c - host tests for the hit-region registry (dawn_hit.c): add and find, overlap
// precedence, frame reset, table overflow and the hint-line parser.

#include "dawn_backend.h"
#include "dawn_hit.h"

#include <stdio.h>

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond)                                                         \
    do {                                                                    \
        tests_run++;                                                        \
        if (!(cond)) {                                                      \
            tests_failed++;                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                   \
    } while (0)

//! The key a tap on (col, row) stands for, 0 for no HIT_KEY region there.
static int32_t key_at(int32_t col, int32_t row)
{
    HitTarget t;
    if (!hit_find(col, row, &t) || t.kind != HIT_KEY)
        return 0;
    return t.key;
}

static void test_add_find(void)
{
    hit_frame_begin();
    CHECK(hit_count() == 0);
    CHECK(!hit_find(1, 1, NULL));

    hit_add(10, 5, 4, 2, HIT_KEY, 'h', 0);
    HitTarget t = { 0 };
    CHECK(hit_find(10, 5, &t));
    CHECK(t.kind == HIT_KEY && t.key == 'h');
    CHECK(hit_find(13, 6, &t)); // bottom-right cell
    CHECK(!hit_find(14, 5, NULL)); // one past the width
    CHECK(!hit_find(10, 7, NULL)); // one past the height
    CHECK(!hit_find(9, 5, NULL));
    CHECK(!hit_find(10, 4, NULL));

    hit_add(1, 1, 3, 1, HIT_ROW, '\r', 7);
    CHECK(hit_find(2, 1, &t));
    CHECK(t.kind == HIT_ROW && t.key == '\r' && t.arg == 7);

    // Empty rectangles are not regions.
    hit_add(30, 30, 0, 1, HIT_KEY, 'x', 0);
    hit_add(30, 30, 1, 0, HIT_KEY, 'x', 0);
    CHECK(!hit_find(30, 30, NULL));
    CHECK(hit_count() == 2);

    // A new frame forgets everything.
    hit_frame_begin();
    CHECK(hit_count() == 0);
    CHECK(!hit_find(10, 5, NULL));
}

static void test_overlap(void)
{
    hit_frame_begin();
    hit_add(1, 1, 80, 24, HIT_INERT, 0, 0); // a box
    hit_add(5, 10, 20, 1, HIT_KEY, 'r', 0); // a row in it
    hit_add(20, 10, 2, 5, HIT_PILL, 0, 1); // a pill over the row's end
    HitTarget t;
    CHECK(hit_find(1, 1, &t) && t.kind == HIT_INERT);
    CHECK(hit_find(5, 10, &t) && t.kind == HIT_KEY && t.key == 'r');
    CHECK(hit_find(20, 10, &t) && t.kind == HIT_PILL && t.arg == 1); // the later one wins
    CHECK(hit_find(21, 14, &t) && t.kind == HIT_PILL);
    CHECK(hit_find(19, 10, &t) && t.kind == HIT_KEY);
    CHECK(hit_find(19, 11, &t) && t.kind == HIT_INERT);
}

static void test_overflow(void)
{
    hit_frame_begin();
    for (int32_t i = 0; i < HIT_MAX; i++)
        hit_add(1, i + 1, 1, 1, HIT_ROW, 0, i);
    CHECK(hit_count() == HIT_MAX);
    hit_add(1, 1, 1, 1, HIT_KEY, 'q', 0); // dropped: would have won row 1
    hit_add(2, 1, 1, 1, HIT_KEY, 'q', 0);
    CHECK(hit_count() == HIT_MAX);
    HitTarget t;
    CHECK(hit_find(1, 1, &t) && t.kind == HIT_ROW && t.arg == 0);
    CHECK(hit_find(1, HIT_MAX, &t) && t.arg == HIT_MAX - 1);
    CHECK(!hit_find(2, 1, NULL));
}

static void test_hints(void)
{
    // Bracket form: "[key] label" apart by three spaces.
    hit_frame_begin();
    const char* finished = "[c] continue   [enter] new   [esc] menu";
    hit_add_hints(11, 3, finished, 100, true);
    CHECK(hit_count() == 3);
    CHECK(key_at(11, 3) == 'c');
    CHECK(key_at(11 + 11, 3) == 'c'); // the 'e' closing "continue"
    CHECK(key_at(11 + 12, 3) == 0); // the gap
    CHECK(key_at(11 + 15, 3) == '\r');
    CHECK(key_at(11 + 25, 3) == '\r'); // "new"'s 'w'
    CHECK(key_at(11 + 29, 3) == '\x1b');
    CHECK(key_at(11 + 38, 3) == '\x1b');
    CHECK(key_at(11 + 39, 3) == 0);
    CHECK(key_at(11, 4) == 0);

    // Brackets one space apart, arrows, and keys that name several (skipped).
    hit_frame_begin();
    hit_add_hints(1, 1, "[<-] keys [tab] meaning [esc] close", 100, true);
    CHECK(key_at(1, 1) == DAWN_KEY_LEFT);
    CHECK(key_at(9, 1) == DAWN_KEY_LEFT); // "keys"'s 's'
    CHECK(key_at(10, 1) == 0);
    CHECK(key_at(11, 1) == '\t');
    CHECK(key_at(25, 1) == '\x1b');
    hit_frame_begin();
    hit_add_hints(1, 1, "[j/k] select   [o] open   [/] reflect with ai", 100, true);
    CHECK(hit_count() == 2);
    CHECK(key_at(1, 1) == 0);
    CHECK(key_at(16, 1) == 'o');
    CHECK(key_at(27, 1) == '/');
    CHECK(key_at(45, 1) == '/'); // "ai"'s 'i'

    // Colon form, with UTF-8 names counted one column each.
    hit_frame_begin();
    hit_add_hints(5, 2, "↑↓:nav  enter:jump  esc:close", 100, true);
    CHECK(hit_count() == 2);
    CHECK(key_at(5, 2) == 0);
    CHECK(key_at(5 + 8, 2) == '\r');
    CHECK(key_at(5 + 17, 2) == '\r');
    CHECK(key_at(5 + 18, 2) == 0);
    CHECK(key_at(5 + 20, 2) == '\x1b');
    hit_frame_begin();
    hit_add_hints(1, 1, "tab:next  </>:part  +/-:adj  ^S:save  +:add field  space:toggle", 100, true);
    CHECK(key_at(1, 1) == '\t');
    CHECK(key_at(11, 1) == 0);
    CHECK(key_at(21, 1) == 0);
    CHECK(key_at(30, 1) == 19);
    CHECK(key_at(39, 1) == '+');
    CHECK(key_at(49, 1) == '+'); // "field"'s 'd'
    CHECK(key_at(52, 1) == ' ');

    // Letters left out where they would be typed; the line cut at max_cols.
    hit_frame_begin();
    hit_add_hints(1, 1, "tab:field  p:%/px  enter:save  esc:cancel", 100, false);
    CHECK(key_at(1, 1) == '\t');
    CHECK(key_at(12, 1) == 0);
    CHECK(key_at(20, 1) == '\r');
    hit_frame_begin();
    hit_add_hints(1, 1, "[o] open   [esc] back", 14, true);
    CHECK(key_at(8, 1) == 'o');
    CHECK(key_at(12, 1) == '\x1b');
    CHECK(key_at(14, 1) == '\x1b');
    CHECK(key_at(15, 1) == 0);
    hit_frame_begin();
    hit_add_hints(1, 1, "[o] open   [esc] back", 9, true);
    CHECK(hit_count() == 1);
}

int main(void)
{
    test_add_find();
    test_overlap();
    test_overflow();
    test_hints();
    printf("%d/%d checks passed\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}
