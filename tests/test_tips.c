// test_tips.c - host tests for the one-shot tips' seen set (dawn_tips.c): marking, names, and the
// "tips_seen" array's round trip through settings.json text, keeping names a newer dawn wrote.

#include "cJSON.h"
#include "dawn_tips.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

//! The array printed without spaces, freed by the caller
static char* print(const cJSON* array)
{
    return array ? cJSON_PrintUnformatted(array) : NULL;
}

static void test_seen_set(void)
{
    tips_reset();
    for (int32_t i = 0; i < TIP_COUNT; i++)
        CHECK(!tips_seen((TipId)i));
    tips_mark(TIP_FIRST_SAVE);
    CHECK(tips_seen(TIP_FIRST_SAVE));
    CHECK(!tips_seen(TIP_FIRST_SELECT));
    tips_mark(TIP_FIRST_SAVE); // twice is once
    CHECK(tips_seen(TIP_FIRST_SAVE));
    tips_mark(TIP_COUNT); // out of range: ignored
    tips_mark((TipId)-1);
    CHECK(!tips_seen(TIP_COUNT));
    CHECK(!tips_seen((TipId)-1));
    tips_reset();
    CHECK(!tips_seen(TIP_FIRST_SAVE));
}

static void test_names(void)
{
    // The names are settings.json's: they never change once shipped.
    CHECK(strcmp(tips_name(TIP_FIRST_SELECT), "first_select") == 0);
    CHECK(strcmp(tips_name(TIP_FIRST_SAVE), "first_save") == 0);
    CHECK(tips_name(TIP_COUNT) == NULL);
    for (int32_t i = 0; i < TIP_COUNT; i++) {
        CHECK(tips_name((TipId)i) != NULL);
        CHECK(tips_from_name(tips_name((TipId)i)) == (TipId)i);
    }
    CHECK(tips_from_name("first_unknown") == TIP_COUNT);
    CHECK(tips_from_name("") == TIP_COUNT);
    CHECK(tips_from_name(NULL) == TIP_COUNT);
}

static void test_json_round_trip(void)
{
    // Nothing seen: an empty array.
    tips_reset();
    cJSON* a = tips_save_json(NULL);
    char* s = print(a);
    CHECK(s && strcmp(s, "[]") == 0);
    free(s);
    cJSON_Delete(a);

    // Seen tips go out by name in TipId order, whatever order they were marked in.
    tips_mark(TIP_FIRST_SAVE);
    tips_mark(TIP_FIRST_SELECT);
    a = tips_save_json(NULL);
    s = print(a);
    CHECK(s && strcmp(s, "[\"first_select\",\"first_save\"]") == 0);

    // And come back from the file's text.
    tips_reset();
    cJSON* back = cJSON_Parse(s);
    tips_load_json(back);
    CHECK(tips_seen(TIP_FIRST_SELECT));
    CHECK(tips_seen(TIP_FIRST_SAVE));
    cJSON_Delete(back);
    free(s);
    cJSON_Delete(a);

    // One seen, one not: only that one comes back.
    tips_reset();
    tips_mark(TIP_FIRST_SAVE);
    a = tips_save_json(NULL);
    s = print(a);
    CHECK(s && strcmp(s, "[\"first_save\"]") == 0);
    tips_reset();
    back = cJSON_Parse(s);
    tips_load_json(back);
    CHECK(tips_seen(TIP_FIRST_SAVE));
    CHECK(!tips_seen(TIP_FIRST_SELECT));
    cJSON_Delete(back);
    free(s);
    cJSON_Delete(a);
}

static void test_json_odd_input(void)
{
    // Unknown names, non-strings and duplicates mark only what is known.
    tips_reset();
    cJSON* in = cJSON_Parse("[\"first_save\", 7, null, \"first_later\", \"first_save\", {\"a\": 1}]");
    tips_load_json(in);
    CHECK(tips_seen(TIP_FIRST_SAVE));
    CHECK(!tips_seen(TIP_FIRST_SELECT));

    // A newer dawn's names survive this one's save, once each, after the known ones.
    tips_mark(TIP_FIRST_SELECT);
    cJSON* out = tips_save_json(in);
    char* s = print(out);
    CHECK(s && strcmp(s, "[\"first_select\",\"first_save\",\"first_later\"]") == 0);
    free(s);
    cJSON_Delete(out);
    cJSON_Delete(in);

    // Anything but an array marks nothing and keeps nothing.
    tips_reset();
    cJSON* obj = cJSON_Parse("{\"first_save\": true}");
    tips_load_json(obj);
    tips_load_json(NULL);
    CHECK(!tips_seen(TIP_FIRST_SAVE));
    out = tips_save_json(obj);
    s = print(out);
    CHECK(s && strcmp(s, "[]") == 0);
    free(s);
    cJSON_Delete(out);
    cJSON_Delete(obj);

    // A string that is a tip's name only by prefix is not that tip.
    cJSON* near = cJSON_Parse("[\"first_sav\", \"first_save_x\", \"FIRST_SAVE\"]");
    tips_load_json(near);
    CHECK(!tips_seen(TIP_FIRST_SAVE));
    cJSON_Delete(near);
}

int main(void)
{
    test_seen_set();
    test_names();
    test_json_round_trip();
    test_json_odd_input();
    printf("%d/%d checks passed\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}
