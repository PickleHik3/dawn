// test_store.c - host tests for dawn's storage layer: dawn_fsio.c (atomic writes through symlinks,
// reads, mkdir -p, no-replace moves, quarantine, hashing) in a scratch directory, the note naming
// rules (dawn_notepath.c: new-note suffixes, conflict copies, versions directories), and the history
// CRDT (dawn_crdt.c: parse, serialize, merge). Each is linked alone; the CRDT's only ties to the
// engine (the app global's backend clock, dawn_strdup, dawn_strncpy) are stubbed below.

#define _GNU_SOURCE

#include "dawn_crdt.h"
#include "dawn_fsio.h"
#include "dawn_notepath.h"
#include "dawn_types.h"
#include "dawn_utils.h"

#include "cJSON.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int tests_run = 0;
static int tests_failed = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        tests_run++;                                                           \
        if (!(cond)) {                                                         \
            tests_failed++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                      \
    } while (0)

// #region Stubs for dawn_crdt.c

App app;

static int64_t stub_sec = 1700000000;
static int64_t stub_ms = 1;

static int64_t stub_clock(DawnClock kind)
{
    return kind == DAWN_CLOCK_MS ? stub_ms++ : stub_sec;
}

static DawnBackend stub_backend;

char* dawn_strdup(const char* string)
{
    if (!string)
        return NULL;
    size_t n = strlen(string) + 1;
    char* copy = malloc(n);
    if (copy)
        memcpy(copy, string, n);
    return copy;
}

void dawn_strncpy(char* dest, const char* src, size_t n)
{
    size_t len = strlen(src);
    if (len > n)
        len = n;
    memcpy(dest, src, len);
    dest[len] = '\0';
}

// #endregion

// #region Helpers

static char scratch[PATH_MAX];

//! scratch/name in a rotating static buffer (enough for the few live at once in one test).
static const char* P(const char* name)
{
    static char bufs[8][PATH_MAX];
    static int next = 0;
    char* b = bufs[next++ % 8];
    int n = snprintf(b, PATH_MAX, "%s/%s", scratch, name);
    if (n < 0 || n >= PATH_MAX) {
        fprintf(stderr, "test path too long: %s\n", name);
        exit(2);
    }
    return b;
}

static void write_raw(const char* path, const char* s)
{
    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot create %s: %s\n", path, strerror(errno));
        exit(2);
    }
    fputs(s, f);
    fclose(f);
}

//! The file's text, or "" when it cannot be read; one static buffer.
static const char* slurp(const char* path)
{
    static char buf[4096];
    size_t len = 0;
    char* data = fsio_read_all(path, &len, sizeof(buf) - 1);
    if (!data)
        return "";
    memcpy(buf, data, len + 1);
    free(data);
    return buf;
}

//! Entries in dir whose name holds ".tmp-": temp files left behind.
static int count_temps(const char* dir)
{
    DIR* d = opendir(dir);
    if (!d)
        return -1;
    int n = 0;
    struct dirent* e;
    while ((e = readdir(d)))
        if (strstr(e->d_name, ".tmp-"))
            n++;
    closedir(d);
    return n;
}

static mode_t mode_of(const char* path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (st.st_mode & 07777) : (mode_t)-1;
}

static bool is_link(const char* path)
{
    struct stat st;
    return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
}

static bool exists(const char* path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

static int rm_entry(const char* path, const struct stat* st, int flag, struct FTW* ftw)
{
    (void)st;
    (void)flag;
    (void)ftw;
    chmod(path, 0700);
    return remove(path);
}

// #endregion

// #region fsio: writing

static void test_write_new_and_overwrite(void)
{
    const char* p = P("note.md");
    CHECK(fsio_write_atomic(p, "hello", 5));
    CHECK(strcmp(slurp(p), "hello") == 0);
    CHECK(mode_of(p) == 0644); // 0666 minus the 022 umask set in main
    CHECK(count_temps(scratch) == 0);

    CHECK(chmod(p, 0600) == 0);
    CHECK(fsio_write_atomic(p, "world!", 6));
    CHECK(strcmp(slurp(p), "world!") == 0);
    CHECK(mode_of(p) == 0600);

    // Empty content is a valid file, not an error.
    CHECK(fsio_write_atomic(p, NULL, 0));
    CHECK(exists(p) && strcmp(slurp(p), "") == 0);

    for (int i = 0; i < 50; i++) {
        char text[32];
        int n = snprintf(text, sizeof(text), "version %d", i);
        CHECK(fsio_write_atomic(p, text, (size_t)n));
    }
    CHECK(strcmp(slurp(p), "version 49") == 0);
    CHECK(count_temps(scratch) == 0);
}

static void test_write_failures(void)
{
    // A target that is a directory: the temp file is made, the rename fails, the temp is removed.
    const char* d = P("adir");
    CHECK(mkdir(d, 0755) == 0);
    errno = 0;
    CHECK(!fsio_write_atomic(d, "x", 1));
    CHECK(errno != 0);
    CHECK(count_temps(scratch) == 0);

    // A directory that cannot be written: false, the old file untouched, nothing left behind.
    if (geteuid() == 0) {
        printf("  (root: read-only directory case skipped)\n");
        return;
    }
    const char* ro = P("ro");
    const char* f = P("ro/keep.md");
    CHECK(mkdir(ro, 0755) == 0);
    write_raw(f, "original");
    CHECK(chmod(ro, 0555) == 0);
    errno = 0;
    CHECK(!fsio_write_atomic(f, "replacement", 11));
    CHECK(errno == EACCES);
    CHECK(strcmp(slurp(f), "original") == 0);
    CHECK(count_temps(ro) == 0);
    CHECK(chmod(ro, 0755) == 0);

    // No directory at all.
    errno = 0;
    CHECK(!fsio_write_atomic(P("missing-dir/x.md"), "x", 1));
    CHECK(errno == ENOENT);
}

static void test_write_symlinks(void)
{
    // A relative link: the file behind it changes, the link stays a link.
    const char* real = P("real.md");
    const char* link = P("link.md");
    write_raw(real, "v1");
    CHECK(symlink("real.md", link) == 0);
    CHECK(fsio_write_atomic(link, "v2", 2));
    CHECK(is_link(link));
    char target[PATH_MAX];
    ssize_t n = readlink(link, target, sizeof(target) - 1);
    CHECK(n == 7 && memcmp(target, "real.md", 7) == 0);
    CHECK(strcmp(slurp(real), "v2") == 0);
    CHECK(count_temps(scratch) == 0);

    // A chain through another directory with a ../ link, and an absolute link at its end.
    CHECK(mkdir(P("sub"), 0755) == 0);
    CHECK(symlink("../link.md", P("sub/l2")) == 0);
    const char* l3 = P("l3");
    CHECK(symlink(P("sub/l2"), l3) == 0);
    CHECK(fsio_write_atomic(l3, "v3", 2));
    CHECK(is_link(l3) && is_link(P("sub/l2")) && is_link(link));
    CHECK(strcmp(slurp(real), "v3") == 0);
    CHECK(count_temps(P("sub")) == 0 && count_temps(scratch) == 0);

    char resolved[PATH_MAX];
    CHECK(fsio_resolve_link(l3, resolved, sizeof(resolved)));
    CHECK(strcmp(slurp(resolved), "v3") == 0);
    CHECK(!is_link(resolved));
    CHECK(!fsio_resolve_link(l3, resolved, 4) && errno == ENAMETOOLONG);

    // A dangling link: the file it names is created, the link survives and now resolves.
    const char* dang = P("dangling.md");
    CHECK(symlink("created-by-write.md", dang) == 0);
    CHECK(fsio_write_atomic(dang, "fresh", 5));
    CHECK(is_link(dang));
    CHECK(strcmp(slurp(P("created-by-write.md")), "fresh") == 0);

    // A loop: refused, nothing created.
    CHECK(symlink("loop2", P("loop1")) == 0);
    CHECK(symlink("loop1", P("loop2")) == 0);
    errno = 0;
    CHECK(!fsio_write_atomic(P("loop1"), "x", 1));
    CHECK(errno == ELOOP);
    CHECK(is_link(P("loop1")) && is_link(P("loop2")));
    CHECK(count_temps(scratch) == 0);
}

static void test_temp_names(void)
{
    char a[PATH_MAX], b[PATH_MAX];
    int fa = fsio_create_temp(P("t.md"), a, sizeof(a));
    int fb = fsio_create_temp(P("t.md"), b, sizeof(b));
    CHECK(fa >= 0 && fb >= 0);
    CHECK(strcmp(a, b) != 0);
    const char* base = strrchr(a, '/') + 1;
    CHECK(strncmp(base, ".t.md.tmp-", 10) == 0 && strlen(base) == 10 + 12);
    CHECK((fcntl(fa, F_GETFD) & FD_CLOEXEC) != 0);
    close(fa);
    close(fb);
    unlink(a);
    unlink(b);

    // Too small an output buffer: refused, nothing created.
    char tiny[8];
    CHECK(fsio_create_temp(P("t.md"), tiny, sizeof(tiny)) == -1 && errno == ENAMETOOLONG);
    CHECK(count_temps(scratch) == 0);

    // A name near NAME_MAX still gets a temp beside it (the name part is cut), and writes work.
    char longname[251];
    memset(longname, 'n', 247);
    memcpy(longname + 247, ".md", 4);
    CHECK(fsio_write_atomic(P(longname), "long", 4));
    CHECK(strcmp(slurp(P(longname)), "long") == 0);
    CHECK(count_temps(scratch) == 0);

    // The cut never splits a UTF-8 sequence: 120 two-byte letters (240 bytes) + ".md".
    char utf[256];
    size_t un = 0;
    for (int i = 0; i < 120; i++) {
        utf[un++] = (char)0xD8;
        utf[un++] = (char)0xB3;
    }
    memcpy(utf + un, ".md", 4);
    int fu = fsio_create_temp(P(utf), a, sizeof(a));
    CHECK(fu >= 0);
    if (fu >= 0) {
        base = strrchr(a, '/') + 1;
        size_t cut = (size_t)(strstr(base, ".tmp-") - base) - 1; // the name bytes after the dot
        CHECK(cut % 2 == 0);
        close(fu);
        unlink(a);
    }
}

// #endregion

// #region fsio: reading

static void test_read(void)
{
    size_t len = 99;
    errno = 0;
    CHECK(fsio_read_all(P("nope.md"), &len, 1024) == NULL);
    CHECK(errno == ENOENT);
    CHECK(len == 0);

    char hundred[101];
    memset(hundred, 'x', 100);
    hundred[100] = '\0';
    write_raw(P("hundred"), hundred);
    errno = 0;
    CHECK(fsio_read_all(P("hundred"), &len, 99) == NULL);
    CHECK(errno == EFBIG);
    char* data = fsio_read_all(P("hundred"), &len, 100);
    CHECK(data && len == 100 && data[100] == '\0');
    free(data);

    const char nul[] = { 'a', '\0', 'b' };
    CHECK(fsio_write_atomic(P("binary"), nul, 3));
    data = fsio_read_all(P("binary"), &len, 1024);
    CHECK(data && len == 3 && data[0] == 'a' && data[1] == '\0' && data[2] == 'b' && data[3] == '\0');
    CHECK(data && fsio_is_binary(data, len));
    free(data);

    write_raw(P("empty"), "");
    data = fsio_read_all(P("empty"), &len, 1024);
    CHECK(data && len == 0 && data[0] == '\0');
    free(data);

    errno = 0;
    CHECK(fsio_read_all(scratch, &len, 1024) == NULL);
    CHECK(errno == EISDIR);

    // A file far larger than one read() returns, read whole and byte for byte.
    size_t big = 300000;
    char* blob = malloc(big);
    for (size_t i = 0; i < big; i++)
        blob[i] = (char)('a' + i % 26);
    CHECK(fsio_write_atomic(P("big"), blob, big));
    data = fsio_read_all(P("big"), &len, big);
    CHECK(data && len == big && memcmp(data, blob, big) == 0);
    free(data);
    free(blob);
    CHECK(fsio_read_all(P("big"), NULL, big - 1) == NULL && errno == EFBIG);
}

// #endregion

// #region fsio: directories, moves, quarantine

static void test_mkdir_p(void)
{
    const char* nested = P("m/a/b/c");
    CHECK(fsio_mkdir_p(nested, 0700));
    CHECK(mode_of(nested) == 0700 && mode_of(P("m/a")) == 0700);
    CHECK(fsio_mkdir_p(nested, 0700)); // already there
    CHECK(fsio_mkdir_p(P("m/a/b/c/"), 0700)); // trailing slash
    CHECK(fsio_mkdir_p(P("m//x///y"), 0700)); // doubled slashes
    CHECK(mode_of(P("m/x/y")) == 0700);

    char overlong[PATH_MAX + 64];
    size_t n = 0;
    while (n + 10 < sizeof(overlong) - 1) {
        memcpy(overlong + n, "/aaaaaaaaa", 10);
        n += 10;
    }
    overlong[n] = '\0';
    errno = 0;
    CHECK(!fsio_mkdir_p(overlong, 0700));
    CHECK(errno == ENAMETOOLONG);

    write_raw(P("m/file"), "x");
    errno = 0;
    CHECK(!fsio_mkdir_p(P("m/file/sub"), 0700));
    CHECK(errno == ENOTDIR);
    errno = 0;
    CHECK(!fsio_mkdir_p(P("m/file"), 0700));
    CHECK(errno == ENOTDIR);
    CHECK(!fsio_mkdir_p("", 0700) && errno == ENOENT);
}

static void test_move_no_replace(void)
{
    write_raw(P("mv1"), "one");
    write_raw(P("mv2"), "two");
    errno = 0;
    CHECK(!fsio_move_no_replace(P("mv1"), P("mv2")));
    CHECK(errno == EEXIST);
    CHECK(strcmp(slurp(P("mv1")), "one") == 0 && strcmp(slurp(P("mv2")), "two") == 0);

    CHECK(fsio_move_no_replace(P("mv1"), P("mv3")));
    CHECK(!exists(P("mv1")));
    CHECK(strcmp(slurp(P("mv3")), "one") == 0);

    // A dangling symlink is something that exists.
    CHECK(symlink("nowhere", P("mv4")) == 0);
    errno = 0;
    CHECK(!fsio_move_no_replace(P("mv3"), P("mv4")));
    CHECK(errno == EEXIST && exists(P("mv3")));

    // A symlink is moved as itself, not as the file it names.
    CHECK(fsio_move_no_replace(P("mv4"), P("mv5")));
    CHECK(is_link(P("mv5")) && !exists(P("mv4")));

    errno = 0;
    CHECK(!fsio_move_no_replace(P("mv-missing"), P("mv6")));
    CHECK(errno == ENOENT && !exists(P("mv6")));
}

static void stamp_now(char* out, size_t size)
{
    time_t now = time(NULL);
    struct tm utc;
    gmtime_r(&now, &utc);
    strftime(out, size, "%Y%m%dT%H%M%SZ", &utc);
}

static void test_quarantine(void)
{
    const char* q = P("q.md");
    write_raw(q, "bad");
    char out[PATH_MAX];
    CHECK(fsio_quarantine(q, out, sizeof(out)));
    CHECK(!exists(q));
    CHECK(strcmp(slurp(out), "bad") == 0);
    size_t ql = strlen(q);
    CHECK(strncmp(out, q, ql) == 0 && strncmp(out + ql, ".corrupt-", 9) == 0);
    const char* stamp = out + ql + 9;
    CHECK(strlen(stamp) == 16 && stamp[8] == 'T' && stamp[15] == 'Z');

    // A taken name gets -2: pre-create this second's name and check, unless the clock ticked.
    for (int attempt = 0; attempt < 3; attempt++) {
        char before[32], after[32], taken[PATH_MAX * 2];
        write_raw(q, "bad again");
        stamp_now(before, sizeof(before));
        snprintf(taken, sizeof(taken), "%s.corrupt-%s", q, before);
        if (!exists(taken))
            write_raw(taken, "earlier");
        char out2[PATH_MAX * 2];
        bool ok = fsio_quarantine(q, out2, sizeof(out2));
        stamp_now(after, sizeof(after));
        CHECK(ok);
        CHECK(strcmp(slurp(taken), "bad again") != 0); // the taken name was never replaced
        if (strcmp(before, after) != 0)
            continue;
        char want[PATH_MAX * 2 + 8];
        snprintf(want, sizeof(want), "%s-2", taken);
        bool fresh_two = strcmp(out2, want) == 0;
        // On a second pass in the same second, -2 is already taken and -3 is next.
        snprintf(want, sizeof(want), "%s-3", taken);
        CHECK(fresh_two || strcmp(out2, want) == 0);
        CHECK(strcmp(slurp(out2), "bad again") == 0);
        break;
    }

    // An out buffer too small: refused before anything moves.
    write_raw(q, "stay");
    char small[8];
    errno = 0;
    CHECK(!fsio_quarantine(q, small, sizeof(small)));
    CHECK(errno == ENAMETOOLONG);
    CHECK(strcmp(slurp(q), "stay") == 0);

    errno = 0;
    CHECK(!fsio_quarantine(P("not-there.md"), out, sizeof(out)));
    CHECK(errno == ENOENT);
}

// #endregion

// #region fsio: content checks

static void test_hash_and_binary(void)
{
    CHECK(fsio_hash("", 0) == 0xcbf29ce484222325ULL);
    CHECK(fsio_hash("a", 1) == 0xaf63dc4c8601ec8cULL);
    CHECK(fsio_hash("foobar", 6) == 0x85944171f73967e8ULL);
    CHECK(fsio_hash("note text", 9) == fsio_hash("note text", 9));
    CHECK(fsio_hash("note text", 9) != fsio_hash("note texT", 9));

    CHECK(!fsio_is_binary("abc", 3));
    CHECK(fsio_is_binary("a\0c", 3));
    CHECK(!fsio_is_binary("a\0c", 1));
    CHECK(!fsio_is_binary(NULL, 0));
}

// #endregion

// #region Note names (dawn_notepath)

static void test_notepath_numbered(void)
{
    char out[64];
    const char* dir = "/notes/dawn/extra";
    size_t dir_len = strlen("/notes/dawn"); // only a prefix of dir counts
    CHECK(notepath_numbered(out, sizeof(out), dir, dir_len, '/', "2026-10-07_143005", 1));
    CHECK(strcmp(out, "/notes/dawn/2026-10-07_143005.md") == 0);
    CHECK(notepath_numbered(out, sizeof(out), dir, dir_len, '/', "2026-10-07_143005", 2));
    CHECK(strcmp(out, "/notes/dawn/2026-10-07_143005-2.md") == 0);
    CHECK(notepath_numbered(out, sizeof(out), dir, dir_len, '/', "eid-plans", 99));
    CHECK(strcmp(out, "/notes/dawn/eid-plans-99.md") == 0);
    CHECK(notepath_numbered(out, sizeof(out), "C:\\notes", 8, '\\', "a", 1));
    CHECK(strcmp(out, "C:\\notes\\a.md") == 0);
    CHECK(notepath_numbered(out, sizeof(out), NULL, 0, '/', "bare", 3));
    CHECK(strcmp(out, "bare-3.md") == 0);

    // Never a cut-short path: one byte short of fitting fails, an exact fit works.
    const char* want = "/notes/dawn/x-2.md";
    size_t need = strlen(want) + 1;
    CHECK(!notepath_numbered(out, need - 1, dir, dir_len, '/', "x", 2));
    CHECK(notepath_numbered(out, need, dir, dir_len, '/', "x", 2) && strcmp(out, want) == 0);
    CHECK(!notepath_numbered(out, sizeof(out), dir, dir_len, '/', "", 1));
}

static void test_notepath_stamp_name(void)
{
    const char* ok[] = { "2026-10-07_143005", "2026-10-07_143005-2", "2026-10-07_143005-10", "2026-10-07_143005-99" };
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++)
        CHECK(notepath_is_stamp_name(ok[i], strlen(ok[i])));
    const char* bad[] = { "2026-10-07_14300", "2026-10-07_143005-", "2026-10-07_143005-1", "2026-10-07_143005-0",
        "2026-10-07_143005-02", "2026-10-07_143005-100", "2026-10-07_143005-2a", "2026-10-07_143005_2",
        "2026-10-07-143005", "eid-plans", "" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(!notepath_is_stamp_name(bad[i], strlen(bad[i])));
    // The length given is what counts (the caller passes the name without ".md").
    CHECK(notepath_is_stamp_name("2026-10-07_143005-3.md", strlen("2026-10-07_143005-3")));
    CHECK(!notepath_is_stamp_name("2026-10-07_143005-3.md", strlen("2026-10-07_143005-3.md")));
}

static void test_notepath_conflict(void)
{
    char out[128];
    CHECK(notepath_conflict(out, sizeof(out), "/n/2026-10-07_143005.md", "20261007-150102", 1));
    CHECK(strcmp(out, "/n/2026-10-07_143005.conflict-20261007-150102.md") == 0);
    CHECK(notepath_conflict(out, sizeof(out), "/n/eid plans.md", "20261007-150102", 3));
    CHECK(strcmp(out, "/n/eid plans.conflict-20261007-150102-3.md") == 0);
    // A file without .md keeps its whole name as the stem; the copy is still a .md.
    CHECK(notepath_conflict(out, sizeof(out), "/n/todo.txt", "20261007-150102", 1));
    CHECK(strcmp(out, "/n/todo.txt.conflict-20261007-150102.md") == 0);
    CHECK(notepath_conflict(out, sizeof(out), "rel.md", "20261007-150102", 1));
    CHECK(strcmp(out, "rel.conflict-20261007-150102.md") == 0);
    CHECK(!notepath_conflict(out, sizeof(out), "/n/", "20261007-150102", 1));
    CHECK(!notepath_conflict(out, 20, "/n/2026-10-07_143005.md", "20261007-150102", 1));

    // Two copies made in one second: the second takes -2, and the copy never lands on the note.
    char first[PATH_MAX], second[PATH_MAX];
    CHECK(notepath_conflict(first, sizeof(first), P("note.md"), "20261007-150102", 1));
    CHECK(notepath_conflict(second, sizeof(second), P("note.md"), "20261007-150102", 2));
    CHECK(strcmp(first, second) != 0 && strcmp(first, P("note.md")) != 0);
}

static void test_notepath_version_dir(void)
{
    char out[160];
    // Inside the notes directory: the plain name, as before.
    CHECK(notepath_version_dir(out, sizeof(out), "/data/dawn/2026-10-07_143005.md", true, 0x1122334455667788ULL));
    CHECK(strcmp(out, "2026-10-07_143005") == 0);
    // Elsewhere: the low 32 bits of the path's hash keep same-named notes apart.
    CHECK(notepath_version_dir(out, sizeof(out), "/home/u/a/todo.md", false, 0x1122334455667788ULL));
    CHECK(strcmp(out, "todo-55667788") == 0);
    CHECK(notepath_version_dir(out, sizeof(out), "/home/u/b/todo.md", false, 0xABCDEF0000000001ULL));
    CHECK(strcmp(out, "todo-00000001") == 0);
    // Unsafe characters become '_'; nothing left, or a dot name, is "note".
    CHECK(notepath_version_dir(out, sizeof(out), "/x/a b:c*.md", true, 0));
    CHECK(strcmp(out, "a_b_c_") == 0);
    CHECK(notepath_version_dir(out, sizeof(out), "/x/", true, 0) && strcmp(out, "note") == 0);
    CHECK(notepath_version_dir(out, sizeof(out), "/x/.md", true, 0) && strcmp(out, ".md") == 0); // as before: no stem cut from ".md" alone
    CHECK(notepath_version_dir(out, sizeof(out), "/x/...md", true, 0) && strcmp(out, "note") == 0);
    CHECK(notepath_version_dir(out, sizeof(out), "/x/..", true, 0) && strcmp(out, "note") == 0);
    // A long UTF-8 name is cut to 127 bytes on a character boundary.
    char longname[400] = "/x/";
    for (int i = 0; i < 100; i++)
        strcat(longname, "\xC3\xA9"); // U+00E9, two bytes
    strcat(longname, ".md");
    CHECK(notepath_version_dir(out, sizeof(out), longname, true, 0));
    CHECK(strlen(out) == 126);
    CHECK(notepath_version_dir(out, sizeof(out), longname, false, 0xFFFFFFFFULL));
    CHECK(strlen(out) == 126 + 9 && strcmp(out + 126, "-ffffffff") == 0);
    CHECK(!notepath_version_dir(out, 8, "/x/abcdefgh.md", true, 0));

    size_t stem_len;
    CHECK(strcmp(notepath_base("/a/b/c.md", &stem_len), "c.md") == 0 && stem_len == 1);
    CHECK(strcmp(notepath_base("C:\\a\\d.md", &stem_len), "d.md") == 0 && stem_len == 1);
    CHECK(strcmp(notepath_base("plain", &stem_len), "plain") == 0 && stem_len == 5);
}

// #endregion

// #region CRDT

static bool same_entry(const CrdtEntry* a, const CrdtEntry* b)
{
    if (!a || !b)
        return false;
    bool values = (!a->value && !b->value) || (a->value && b->value && strcmp(a->value, b->value) == 0);
    return strcmp(a->key, b->key) == 0 && values && a->timestamp == b->timestamp && strcmp(a->node, b->node) == 0;
}

static void test_crdt_round_trip(void)
{
    stub_sec = 1700000000;
    CrdtState* s = crdt_create();
    CHECK(s && strlen(s->node) == CRDT_NODE_ID_LEN);
    crdt_upsert(s, "/notes/a.md", "Alpha");
    stub_sec++;
    crdt_upsert(s, "/notes/b.md", NULL);
    crdt_meta_set_str(crdt_find(s, "/notes/a.md"), "title", "First");
    crdt_meta_set_int(crdt_find(s, "/notes/a.md"), "words", 42);
    stub_sec++;
    crdt_remove(s, "/notes/gone.md");

    char* json = crdt_serialize(s);
    CHECK(json != NULL);
    CrdtState* back = json ? crdt_parse(json, strlen(json)) : NULL;
    CHECK(back != NULL);
    if (back) {
        CHECK(strcmp(back->node, s->node) == 0);
        CHECK(back->entry_count == 2 && back->tombstone_count == 1);
        CHECK(same_entry(crdt_find(back, "/notes/a.md"), crdt_find(s, "/notes/a.md")));
        CHECK(same_entry(crdt_find(back, "/notes/b.md"), crdt_find(s, "/notes/b.md")));
        CHECK(crdt_find(back, "/notes/gone.md") == NULL);
        CHECK(back->tombstone_count == 1 && strcmp(back->tombstones[0].key, "/notes/gone.md") == 0
            && back->tombstones[0].timestamp == s->tombstones[0].timestamp);
        const char* title = crdt_meta_get_str(crdt_find(back, "/notes/a.md"), "title");
        CHECK(title && strcmp(title, "First") == 0);
        int64_t words = 0;
        CHECK(crdt_meta_get_int(crdt_find(back, "/notes/a.md"), "words", &words) && words == 42);

        char* again = crdt_serialize(back);
        CHECK(again && strcmp(again, json) == 0);
        free(again);
    }
    free(json);
    crdt_free(back);
    crdt_free(s);
}

static void test_crdt_parse_rejects(void)
{
    const char* garbage = "this is not json {";
    CHECK(crdt_parse(garbage, strlen(garbage)) == NULL);
    CHECK(crdt_parse("", 0) == NULL);
    CHECK(crdt_parse(NULL, 0) == NULL);
    const char* old = "{\"version\":1,\"node\":\"abc\",\"entries\":{}}";
    CHECK(crdt_parse(old, strlen(old)) == NULL);
    const char* none = "{\"node\":\"abc\",\"entries\":{}}";
    CHECK(crdt_parse(none, strlen(none)) == NULL);
    const char* str = "{\"version\":\"2\"}";
    CHECK(crdt_parse(str, strlen(str)) == NULL);
    const char* truncated = "{\"version\":2,\"node\":\"abc\",\"entries\":{\"x\":{\"value\":\"v\"";
    CHECK(crdt_parse(truncated, strlen(truncated)) == NULL);
    const char* minimal = "{\"version\":2}";
    CrdtState* ok = crdt_parse(minimal, strlen(minimal));
    CHECK(ok && ok->entry_count == 0 && ok->tombstone_count == 0);
    crdt_free(ok);
}

static const char* value_of(const CrdtState* s, const char* key)
{
    const CrdtEntry* e = crdt_find(s, key);
    return e && e->value ? e->value : "";
}

//! Every live entry of a has an identical counterpart in b, and the counts agree.
static bool same_live(const CrdtState* a, const CrdtState* b)
{
    int32_t na = 0, nb = 0;
    CrdtEntry** la = crdt_get_live(a, &na);
    CrdtEntry** lb = crdt_get_live(b, &nb);
    bool same = na == nb;
    for (int32_t i = 0; same && i < na; i++)
        same = same_entry(la[i], crdt_find(b, la[i]->key));
    free(la);
    free(lb);
    return same;
}

static void test_crdt_merge(void)
{
    stub_sec = 1700001000;
    CrdtState* a = crdt_create();
    CrdtState* b = crdt_create();
    CHECK(a && b && strcmp(a->node, b->node) != 0);

    crdt_upsert(a, "a1.md", "from a");
    crdt_upsert(a, "a2.md", "also a");
    crdt_upsert(b, "b1.md", "from b");
    crdt_remove(b, "b-gone.md");

    // One key both sides wrote: the later write wins in either order.
    stub_sec = 1700002000;
    crdt_upsert(a, "shared.md", "older");
    stub_sec = 1700003000;
    crdt_upsert(b, "shared.md", "newer");

    CrdtState* ab = crdt_merge(a, b);
    CrdtState* ba = crdt_merge(b, a);
    CHECK(ab && ba);
    if (ab && ba) {
        const char* keys[] = { "a1.md", "a2.md", "b1.md", "shared.md" };
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            CHECK(crdt_find(ab, keys[i]) != NULL);
            CHECK(crdt_find(ba, keys[i]) != NULL);
        }
        CHECK(strcmp(value_of(ab, "shared.md"), "newer") == 0);
        CHECK(strcmp(value_of(ba, "shared.md"), "newer") == 0);
        CHECK(same_entry(crdt_find(ab, "a1.md"), crdt_find(a, "a1.md")));
        CHECK(same_entry(crdt_find(ab, "b1.md"), crdt_find(b, "b1.md")));
        CHECK(crdt_find(ab, "b-gone.md") == NULL && ab->tombstone_count == 1);
        CHECK(crdt_find(ba, "b-gone.md") == NULL && ba->tombstone_count == 1);
        CHECK(same_live(ab, ba) && same_live(ba, ab));

        // A delete later than the write removes the entry on merge.
        stub_sec = 1700004000;
        crdt_remove(a, "b1.md");
        CrdtState* later = crdt_merge(a, b);
        CHECK(later && crdt_find(later, "b1.md") == NULL);
        crdt_free(later);
    }

    crdt_free(ab);
    crdt_free(ba);
    crdt_free(a);
    crdt_free(b);
}

// #endregion

int main(void)
{
    stub_backend.clock = stub_clock;
    app.ctx.b = &stub_backend;
    umask(022);

    const char* tmp = getenv("TMPDIR");
    snprintf(scratch, sizeof(scratch), "%s/dawn-store-XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
    if (!mkdtemp(scratch)) {
        fprintf(stderr, "mkdtemp failed: %s\n", strerror(errno));
        return 2;
    }

    test_write_new_and_overwrite();
    test_write_failures();
    test_write_symlinks();
    test_temp_names();
    test_read();
    test_mkdir_p();
    test_move_no_replace();
    test_quarantine();
    test_hash_and_binary();
    test_notepath_numbered();
    test_notepath_stamp_name();
    test_notepath_conflict();
    test_notepath_version_dir();
    test_crdt_round_trip();
    test_crdt_parse_rejects();
    test_crdt_merge();

    nftw(scratch, rm_entry, 16, FTW_DEPTH | FTW_PHYS);

    printf("test-store: %d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
