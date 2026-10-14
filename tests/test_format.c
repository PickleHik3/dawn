// test_format.c - host tests for dawn_format.c, the formatting commands behind the ribbon: inline
// styles with and without a selection, line styles over one and several lines, the inserts, the
// cursor and selection after each command, and toggles that undo themselves. Linked with
// dawn_gap.c alone. Texts are written with '@' for the cursor and '$' for the selection anchor.

#include "dawn_format.h"
#include "dawn_gap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_failed = 0;

//! Fill g and c from text written with '@' (cursor) and '$' (anchor; a selection when present)
static void load(const char* in, GapBuffer* g, FormatCursor* c)
{
    gap_init(g, 8);
    size_t k = 0;
    bool anchor = false;
    c->cursor = c->anchor = 0;
    c->selecting = false;
    for (const char* p = in; *p; p++) {
        if (*p == '@') {
            c->cursor = k;
        } else if (*p == '$') {
            c->anchor = k;
            anchor = true;
        } else {
            gap_insert(g, k++, *p);
        }
    }
    if (anchor)
        c->selecting = true;
    else
        c->anchor = c->cursor;
}

//! The text of g with '@' and, for a selection, '$' put back in
static char* render(const GapBuffer* g, const FormatCursor* c)
{
    char* t = gap_to_str(g);
    size_t n = strlen(t), k = 0;
    char* out = malloc(n + 3);
    for (size_t i = 0; i <= n; i++) {
        if (c->selecting && c->anchor != c->cursor && i == c->anchor)
            out[k++] = '$';
        if (i == c->cursor)
            out[k++] = '@';
        if (i < n)
            out[k++] = t[i];
    }
    out[k] = '\0';
    free(t);
    return out;
}

static void show(const char* s)
{
    fputc('"', stderr);
    for (; *s; s++) {
        if (*s == '\n')
            fputs("\\n", stderr);
        else if (*s == '\r')
            fputs("\\r", stderr);
        else
            fputc(*s, stderr);
    }
    fputc('"', stderr);
}

static void check_apply(int line, const char* in, FormatOp op, const char* want, bool want_changed)
{
    GapBuffer g;
    FormatCursor c;
    load(in, &g, &c);
    bool changed = format_apply(&g, op, &c);
    char* got = render(&g, &c);
    tests_run++;
    if (strcmp(got, want) != 0 || changed != want_changed || c.cursor > gap_len(&g) || c.anchor > gap_len(&g)) {
        tests_failed++;
        fprintf(stderr, "FAIL %s:%d: op %d on ", __FILE__, line, (int)op);
        show(in);
        fputs("\n  want ", stderr);
        show(want);
        fprintf(stderr, " (changed %d)\n  got  ", want_changed);
        show(got);
        fprintf(stderr, " (changed %d)\n", changed);
    }
    free(got);
    gap_free(&g);
}

//! op on in gives want, and op again gives in back
static void check_toggle(int line, const char* in, FormatOp op, const char* want)
{
    check_apply(line, in, op, want, true);
    check_apply(line, want, op, in, true);
}

static void check_state(int line, const char* in, FormatOp op, bool want)
{
    GapBuffer g;
    FormatCursor c;
    load(in, &g, &c);
    bool got = format_state(&g, &c, op);
    tests_run++;
    if (got != want) {
        tests_failed++;
        fprintf(stderr, "FAIL %s:%d: state %d on ", __FILE__, line, (int)op);
        show(in);
        fprintf(stderr, " want %d got %d\n", want, got);
    }
    gap_free(&g);
}

#define APPLY(in, op, want) check_apply(__LINE__, in, op, want, true)
#define SAME(in, op) check_apply(__LINE__, in, op, in, false)
#define TOGGLE(in, op, want) check_toggle(__LINE__, in, op, want)
#define STATE(in, op, want) check_state(__LINE__, in, op, want)

static void test_inline_caret(void)
{
    // the word at or touching the cursor, cursor kept on the same letter
    TOGGLE("hello w@orld", FORMAT_BOLD, "hello **w@orld**");
    TOGGLE("@hello world", FORMAT_BOLD, "**@hello** world");
    TOGGLE("hello@ world", FORMAT_ITALIC, "*hello@* world");
    TOGGLE("a fi@ne day", FORMAT_STRIKE, "a ~~fi@ne~~ day");
    TOGGLE("call pr@intf now", FORMAT_CODE, "call `pr@intf` now");
    TOGGLE("it's do@n't", FORMAT_BOLD, "it's **do@n't**");
    TOGGLE("caf\xc3\xa9@ ok", FORMAT_BOLD, "**caf\xc3\xa9@** ok");
    // whitespace and empty lines get an empty pair, and a second press removes it
    TOGGLE("a @", FORMAT_BOLD, "a **@**");
    TOGGLE("@", FORMAT_ITALIC, "*@*");
    TOGGLE("x @ y", FORMAT_STRIKE, "x ~~@~~ y");
    TOGGLE("@", FORMAT_CODE, "`@`");
    // nesting
    TOGGLE("**h@i**", FORMAT_ITALIC, "***h@i***");
    TOGGLE("*h@i*", FORMAT_BOLD, "***h@i***");
    TOGGLE("**h@i**", FORMAT_STRIKE, "**~~h@i~~**");
    // emphasis goes outside a code span
    TOGGLE("`c@ode`", FORMAT_BOLD, "**`c@ode`**");
    // a word inside a longer span: the span is closed and reopened around it
    APPLY("**foo b@ar**", FORMAT_BOLD, "**foo** b@ar");
    APPLY("**foo b@ar baz**", FORMAT_BOLD, "**foo** b@ar **baz**");
    APPLY("**f@oo bar**", FORMAT_BOLD, "f@oo **bar**");
    // whitespace inside a span splits it there
    APPLY("**foo @ bar**", FORMAT_BOLD, "**foo** @ **bar**");
    // a cursor in the block prefix acts on the first word
    APPLY("@- item", FORMAT_BOLD, "- **@item**");
    APPLY("@# Title", FORMAT_ITALIC, "# *@Title*");
    // whitespace inside a code span: emphasis has nowhere to go
    SAME("`a @ b`", FORMAT_BOLD);
}

static void test_inline_selection(void)
{
    TOGGLE("hello $world@", FORMAT_BOLD, "hello **$world@**");
    TOGGLE("$hello@ world", FORMAT_ITALIC, "*$hello@* world");
    TOGGLE("a $b@ c", FORMAT_STRIKE, "a ~~$b@~~ c");
    TOGGLE("use $printf@ here", FORMAT_CODE, "use `$printf@` here");
    // a reversed selection keeps its direction
    TOGGLE("@hello$ world", FORMAT_BOLD, "**@hello$** world");
    // whitespace at the ends stays outside the markers
    APPLY("$hello @world", FORMAT_BOLD, "**$hello@** world");
    APPLY("say$ hi @there", FORMAT_ITALIC, "say *$hi@* there");
    // markers inside or just outside the selection count as the style
    APPLY("$**hello**@ x", FORMAT_BOLD, "$hello@ x");
    APPLY("**$hello@**", FORMAT_BOLD, "$hello@");
    APPLY("$~~gone~~@", FORMAT_STRIKE, "$gone@");
    APPLY("$`x`@", FORMAT_CODE, "$x@");
    // bold and italic together
    APPLY("***$hi@***", FORMAT_BOLD, "*$hi@*");
    APPLY("***$hi@***", FORMAT_ITALIC, "**$hi@**");
    APPLY("***h@i***", FORMAT_ITALIC, "**h@i**");
    APPLY("*foo **b@ar***", FORMAT_ITALIC, "*foo* **b@ar**");
    STATE("***$hi@***", FORMAT_BOLD, true);
    STATE("***$hi@***", FORMAT_ITALIC, true);
    // strike around bold
    TOGGLE("**$x@**", FORMAT_STRIKE, "**~~$x@~~**");
    // part of a span: closed before and reopened after
    APPLY("**foo $bar@ baz**", FORMAT_BOLD, "**foo** $bar@ **baz**");
    // a selection crossing a span grows over it, and the inner span is swallowed
    APPLY("a **b $c** d@", FORMAT_BOLD, "a **$b c d@**");
    APPLY("$x **y** z@", FORMAT_BOLD, "**$x y z@**");
    // code with a backtick inside uses a longer fence
    APPLY("$a`b@", FORMAT_CODE, "``$a`b@``");
    // emphasis on text in a code span goes outside it
    APPLY("`$code@`", FORMAT_BOLD, "**$`code`@**");
    // several lines: each line on its own, past list markers
    TOGGLE("$one\ntwo@", FORMAT_BOLD, "**$one**\n**two@**");
    TOGGLE("- $one\n- two@", FORMAT_ITALIC, "- *$one*\n- *two@*");
    TOGGLE("$one\n\ntwo@", FORMAT_STRIKE, "~~$one~~\n\n~~two@~~");
    // mixed lines: the style goes on where it is missing
    APPLY("$**one**\ntwo@", FORMAT_BOLD, "**$one**\n**two@**");
    STATE("$**one**\ntwo@", FORMAT_BOLD, false);
    STATE("$**one**\n**two**@", FORMAT_BOLD, true);
    // CRLF line ends: the '\r' stays outside
    APPLY("$foo\r\nbar@", FORMAT_BOLD, "**$foo**\r\n**bar@**");
    APPLY("foo\r\nb@ar\r\n", FORMAT_BOLD, "foo\r\n**b@ar**\r\n");
    // a selection of whitespace only changes nothing
    SAME("a$  @b", FORMAT_BOLD);
}

static void test_inline_state(void)
{
    STATE("**b@old** plain", FORMAT_BOLD, true);
    STATE("**bold** pl@ain", FORMAT_BOLD, false);
    STATE("**bold and m@ore**", FORMAT_BOLD, true);
    STATE("*it@alic*", FORMAT_ITALIC, true);
    STATE("*it@alic*", FORMAT_BOLD, false);
    STATE("**b@old**", FORMAT_ITALIC, false);
    STATE("~~st@rike~~", FORMAT_STRIKE, true);
    STATE("~s@ub~", FORMAT_STRIKE, false);
    STATE("`co@de`", FORMAT_CODE, true);
    STATE("`**no@t**`", FORMAT_BOLD, false);
    STATE("\\*no@t\\*", FORMAT_ITALIC, false);
    STATE("a **@**", FORMAT_BOLD, true);
    STATE("a @", FORMAT_BOLD, false);
}

static void test_line_styles(void)
{
    // headings: set, change level, remove
    TOGGLE("he@llo", FORMAT_H1, "# he@llo");
    TOGGLE("he@llo", FORMAT_H3, "### he@llo");
    APPLY("# he@llo", FORMAT_H2, "## he@llo");
    APPLY("### he@llo", FORMAT_H1, "# he@llo");
    TOGGLE("@", FORMAT_H2, "## @");
    APPLY("#@ title", FORMAT_H1, "@title");
    STATE("## ti@tle", FORMAT_H2, true);
    STATE("## ti@tle", FORMAT_H1, false);
    // bullets and numbers convert into each other
    TOGGLE("it@em", FORMAT_BULLET, "- it@em");
    TOGGLE("it@em", FORMAT_NUMBERED, "1. it@em");
    APPLY("- it@em", FORMAT_NUMBERED, "1. it@em");
    APPLY("1. it@em", FORMAT_BULLET, "- it@em");
    APPLY("* it@em", FORMAT_BULLET, "it@em");
    TOGGLE("@", FORMAT_BULLET, "- @");
    // indentation is kept
    TOGGLE("  it@em", FORMAT_BULLET, "  - it@em");
    // headings and list markers replace each other
    APPLY("- it@em", FORMAT_H1, "# it@em");
    APPLY("# ti@tle", FORMAT_BULLET, "- ti@tle");
    APPLY("- [ ] it@em", FORMAT_H2, "## it@em");
    // tasks are a box on a list item
    APPLY("bu@y milk", FORMAT_TASK, "- [ ] bu@y milk");
    TOGGLE("- bu@y milk", FORMAT_TASK, "- [ ] bu@y milk");
    APPLY("- [x] do@ne", FORMAT_TASK, "- do@ne");
    TOGGLE("1. it@em", FORMAT_TASK, "1. [ ] it@em");
    APPLY("- [ ] it@em", FORMAT_BULLET, "it@em");
    APPLY("- [ ] it@em", FORMAT_NUMBERED, "1. [ ] it@em");
    STATE("- [ ] it@em", FORMAT_TASK, true);
    STATE("- [ ] it@em", FORMAT_BULLET, true);
    STATE("- it@em", FORMAT_TASK, false);
    // quotes go outside everything
    TOGGLE("quo@te", FORMAT_QUOTE, "> quo@te");
    TOGGLE("- it@em", FORMAT_QUOTE, "> - it@em");
    APPLY("> it@em", FORMAT_BULLET, "> - it@em");
    APPLY("> > de@ep", FORMAT_QUOTE, "> de@ep");
    STATE("> - it@em", FORMAT_QUOTE, true);
    STATE("> - it@em", FORMAT_BULLET, true);
    // the cursor at the line start ends up after the new prefix
    APPLY("@text", FORMAT_QUOTE, "> @text");
    // CRLF
    TOGGLE("it@em\r\nnext", FORMAT_BULLET, "- it@em\r\nnext");
}

static void test_line_styles_multi(void)
{
    TOGGLE("$a\nb\n\nc@", FORMAT_BULLET, "- $a\n- b\n\n- c@");
    TOGGLE("$a\nb\nc@", FORMAT_NUMBERED, "1. $a\n2. b\n3. c@");
    TOGGLE("$a\nb@", FORMAT_H2, "## $a\n## b@");
    APPLY("$a\nb@", FORMAT_TASK, "- [ ] $a\n- [ ] b@");
    APPLY("- [ ] $a\n- [ ] b@", FORMAT_TASK, "- $a\n- b@");
    // blank lines between quoted lines get ">"
    TOGGLE("$a\n\nb@", FORMAT_QUOTE, "> $a\n>\n> b@");
    // lines that have the style already are left alone when others lack it
    APPLY("$# a\nb@", FORMAT_H1, "$# a\n# b@");
    APPLY("$- a\nb@", FORMAT_BULLET, "$- a\n- b@");
    STATE("$# a\nb@", FORMAT_H1, false);
    STATE("$# a\n# b@", FORMAT_H1, true);
    // a selection ending at a line's start leaves that line out
    APPLY("$a\n@b", FORMAT_BULLET, "- $a\n@b");
    // numbered items continue the list above and count per indentation
    APPLY("1. a\n2. b\nc@", FORMAT_NUMBERED, "1. a\n2. b\n3. c@");
    APPLY("$a\n  b\n  c\nd@", FORMAT_NUMBERED, "1. $a\n  1. b\n  2. c\n2. d@");
    // renumbering a mixed run
    APPLY("$- a\n7. b\nc@", FORMAT_NUMBERED, "1. $a\n2. b\n3. c@");
    // mixed lists become bullets
    APPLY("$1. a\n- b@", FORMAT_BULLET, "- $a\n- b@");
    // a selection over blank lines acts on the cursor's line
    APPLY("$\n@", FORMAT_BULLET, "$\n- @");
}

static void test_inserts(void)
{
    APPLY("see $docs@ now", FORMAT_LINK, "see [docs]($url@) now");
    APPLY("see @", FORMAT_LINK, "see [$text@](url)");
    APPLY("$https://x.y@", FORMAT_LINK, "[$text@](https://x.y)");
    APPLY("$ cat @", FORMAT_IMAGE, " ![cat]($path@) ");
    APPLY("@", FORMAT_IMAGE, "![$alt@](path)");
    STATE("[li@nk](url)", FORMAT_LINK, false);

    // tables and rules sit on their own lines with blank lines around
    APPLY("intro@", FORMAT_TABLE, "intro\n\n| @ |  |\n| --- | --- |\n|  |  |\n|  |  |\n");
    APPLY("a@\nb", FORMAT_TABLE, "a\n\n| @ |  |\n| --- | --- |\n|  |  |\n|  |  |\n\nb");
    APPLY("@", FORMAT_TABLE, "| @ |  |\n| --- | --- |\n|  |  |\n|  |  |\n");
    APPLY("abc@", FORMAT_RULE, "abc\n\n---\n@");
    APPLY("@abc", FORMAT_RULE, "---\n\n@abc");
    APPLY("ab@c", FORMAT_RULE, "abc\n\n---\n@");
    APPLY("abc\n\n@\n\nxyz", FORMAT_RULE, "abc\n\n---@\n\nxyz");
    APPLY("abc\n@  \nxyz", FORMAT_RULE, "abc\n\n---\n@\nxyz");

    // code blocks: insert empty, wrap lines, unwrap
    APPLY("@", FORMAT_CODE_BLOCK, "```\n@\n```\n");
    APPLY("```\n@\n```\n", FORMAT_CODE_BLOCK, "@\n");
    TOGGLE("$a\nb@", FORMAT_CODE_BLOCK, "```\n$a\nb@\n```");
    TOGGLE("x\n$a@\ny", FORMAT_CODE_BLOCK, "x\n```\n$a@\n```\ny");
    APPLY("$use ```x```@", FORMAT_CODE_BLOCK, "````\n$use ```x```@\n````");
    STATE("```\nco@de\n```\nafter", FORMAT_CODE_BLOCK, true);
    STATE("```\ncode\n```\naf@ter", FORMAT_CODE_BLOCK, false);
    STATE("~~~\nunclosed@", FORMAT_CODE_BLOCK, true);
}

static void test_cursor_clamped(void)
{
    GapBuffer g;
    FormatCursor c = { 100, 200, true };
    gap_init(&g, 8);
    gap_insert_str(&g, 0, "abc", 3);
    bool changed = format_apply(&g, FORMAT_BOLD, &c);
    char* got = render(&g, &c);
    tests_run++;
    if (!changed || strcmp(got, "**abc@**") != 0) {
        tests_failed++;
        fprintf(stderr, "FAIL %s:%d: clamped cursor gave ", __FILE__, __LINE__);
        show(got);
        fputc('\n', stderr);
    }
    free(got);
    for (int32_t op = 0; op < FORMAT_COUNT; op++) {
        c = (FormatCursor) { 1000, 5000, true };
        format_apply(&g, (FormatOp)op, &c);
        tests_run++;
        if (c.cursor > gap_len(&g) || c.anchor > gap_len(&g)) {
            tests_failed++;
            fprintf(stderr, "FAIL %s:%d: op %d left the cursor outside\n", __FILE__, __LINE__, (int)op);
        }
        (void)format_state(&g, &c, (FormatOp)op);
    }
    gap_free(&g);
}

int main(void)
{
    test_inline_caret();
    test_inline_selection();
    test_inline_state();
    test_line_styles();
    test_line_styles_multi();
    test_inserts();
    test_cursor_clamped();
    printf("%d/%d format tests passed\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}
