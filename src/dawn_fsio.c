// dawn_fsio.c - Durable, crash-safe file primitives under every note dawn writes.
//
// See dawn_fsio.h for the contract. The parts worth knowing when changing this file:
// - Temp names are `.<name>.tmp-<12 base32 chars>`, created with O_EXCL so two processes (or two
//   threads) can never write the same temp file; the random part mixes the pid, the wall clock in
//   nanoseconds, a process-wide atomic counter and a stack address through splitmix64. That needs
//   no getrandom or arc4random, so it builds the same on glibc of any age, bionic and macOS; the
//   O_EXCL retry, not the randomness, is what makes the name safe. The leading dot keeps a temp
//   file left by a crash out of dawn's note listings (posix_list_dir skips dot files).
// - The data is fsynced before the rename and the directory after it: without the first a crash
//   can leave an empty file under the note's name, without the second the rename itself can be
//   lost.
// - No renameat2: RENAME_NOREPLACE is outside the seccomp filter of some Android app sandboxes and
//   the kernel kills the process. fsio_move_no_replace uses linkat + unlink instead.
// - Nothing here touches app, the backend or notices; tests/test_store.c links this file alone.

#ifndef _WIN32

#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _GNU_SOURCE
#endif

#include "dawn_fsio.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

#define TEMP_RAND_CHARS 12 //!< 60 random bits in the temp name, 5 per base32 char
#define TEMP_SUFFIX ".tmp-" //!< Between the target's name and the random part

// #region Helpers

//! Copy src into out, or fail with ENAMETOOLONG when it does not fit (never truncates).
static bool copy_path(char* out, size_t out_size, const char* src)
{
    size_t n = strlen(src);
    if (!out || n >= out_size) {
        errno = ENAMETOOLONG;
        return false;
    }
    memcpy(out, src, n + 1);
    return true;
}

//! snprintf that reports truncation (and encoding errors) as failure with ENAMETOOLONG.
static bool format_path(char* out, size_t out_size, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
static bool format_path(char* out, size_t out_size, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int32_t n = vsnprintf(out, out_size, fmt, args);
    va_end(args);
    if (n < 0 || (size_t)n >= out_size) {
        errno = ENAMETOOLONG;
        return false;
    }
    return true;
}

static uint64_t splitmix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

//! A fresh 60-bit token for a temp name, different on every call within a process and very likely
//! different across processes (pid and nanosecond clock differ).
static uint64_t temp_token(void)
{
    static _Atomic uint64_t counter = 0;
    uint64_t n = atomic_fetch_add(&counter, 1);
    struct timespec ts = { 0 };
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t seed = ((uint64_t)getpid() << 32) ^ ((uint64_t)ts.tv_sec * 1000000007ULL) ^ (uint64_t)ts.tv_nsec;
    seed ^= splitmix64(n) ^ (uint64_t)(uintptr_t)&n;
    return splitmix64(seed);
}

//! How many bytes of name fit in at most max bytes without splitting a UTF-8 sequence (some
//! filesystems, Android's FUSE and APFS among them, refuse names that are not valid UTF-8).
static size_t utf8_cut(const char* name, size_t len, size_t max)
{
    if (len <= max)
        return len;
    size_t n = max;
    while (n > 0 && ((unsigned char)name[n] & 0xC0) == 0x80)
        n--;
    return n;
}

//! The directory part of path, for opening it: "." for a bare name, "/" for a file in the root.
static bool dir_of(const char* path, char* out, size_t out_size)
{
    const char* slash = strrchr(path, '/');
    if (!slash)
        return copy_path(out, out_size, ".");
    size_t n = slash == path ? 1 : (size_t)(slash - path);
    if (n >= out_size) {
        errno = ENAMETOOLONG;
        return false;
    }
    memcpy(out, path, n);
    out[n] = '\0';
    return true;
}

static ssize_t read_retry(int fd, void* buf, size_t len)
{
    ssize_t n;
    do {
        n = read(fd, buf, len);
    } while (n < 0 && errno == EINTR);
    return n;
}

//! Every byte or false: short writes are continued, EINTR retried, a zero-byte write is EIO.
static bool write_all(int fd, const void* data, size_t len)
{
    const char* p = data;
    while (len > 0) {
        size_t chunk = len > (1u << 30) ? (1u << 30) : len;
        ssize_t n = write(fd, p, chunk);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0) {
            errno = EIO;
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static int fsync_retry(int fd)
{
    int r;
    do {
        r = fsync(fd);
    } while (r != 0 && errno == EINTR);
    return r;
}

//! Make a rename in path's directory durable. Filesystems that cannot fsync a directory (EINVAL,
//! EROFS, some FUSE mounts) or will not open one for reading count as done: the rename has
//! already happened and there is nothing more this process can do about it.
static bool sync_dir_of(const char* path)
{
    char dir[PATH_MAX];
    if (!dir_of(path, dir, sizeof(dir)))
        return true;
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0)
        return true;
    int r = fsync_retry(dfd);
    int e = errno;
    close(dfd);
    if (r == 0 || e == EINVAL || e == EROFS || e == EACCES || e == EBADF)
        return true;
    errno = e;
    return false;
}

static bool make_dir(const char* dir, mode_t mode)
{
    if (mkdir(dir, mode) == 0)
        return true;
    // EEXIST is the usual case, but a parent this process may not write to (Android's /data, a
    // read-only /) can answer EACCES or EROFS for a directory that is already there.
    int e = errno;
    struct stat st;
    if (stat(dir, &st) == 0) {
        if (S_ISDIR(st.st_mode))
            return true;
        errno = ENOTDIR;
        return false;
    }
    errno = e;
    return false;
}

//! Errors from link(2) that mean "no hard links here", not "this move cannot happen".
static bool link_unsupported(int e)
{
    switch (e) {
    case EPERM:
    case EACCES:
    case ENOSYS:
    case EXDEV:
    case EMLINK:
#ifdef ENOTSUP
    case ENOTSUP:
#endif
#if defined(EOPNOTSUPP) && (!defined(ENOTSUP) || EOPNOTSUPP != ENOTSUP)
    case EOPNOTSUPP:
#endif
        return true;
    default:
        return false;
    }
}

// #endregion

// #region Writing

bool fsio_resolve_link(const char* path, char* out, size_t out_size)
{
    if (!path || !*path) {
        errno = ENOENT;
        return false;
    }
    char cur[PATH_MAX];
    if (!copy_path(cur, sizeof(cur), path))
        return false;

    for (int32_t depth = 0;; depth++) {
        // Missing (a dangling link's target, or a new file) or not a link: this is the file.
        struct stat st;
        if (lstat(cur, &st) != 0 || !S_ISLNK(st.st_mode))
            break;
        if (depth == FSIO_MAX_LINKS) {
            errno = ELOOP;
            return false;
        }

        char link[PATH_MAX];
        ssize_t n = readlink(cur, link, sizeof(link) - 1);
        if (n < 0)
            return false;
        if ((size_t)n >= sizeof(link) - 1) {
            errno = ENAMETOOLONG;
            return false;
        }
        if (n == 0) {
            errno = ENOENT;
            return false;
        }
        link[n] = '\0';

        // A relative link is relative to the directory holding the link. Joining the strings is
        // enough even when that directory is itself reached through symlinks: the kernel resolves
        // `dir/../x` against the real directory.
        const char* slash = strrchr(cur, '/');
        if (link[0] == '/' || !slash) {
            memcpy(cur, link, (size_t)n + 1);
        } else {
            size_t dl = (size_t)(slash - cur) + 1;
            if (dl + (size_t)n >= sizeof(cur)) {
                errno = ENAMETOOLONG;
                return false;
            }
            memcpy(cur + dl, link, (size_t)n + 1);
        }
    }
    return copy_path(out, out_size, cur);
}

int fsio_create_temp(const char* target, char* out, size_t out_size)
{
    if (!target || !*target || !out) {
        errno = EINVAL;
        return -1;
    }
    const char* slash = strrchr(target, '/');
    const char* name = slash ? slash + 1 : target;
    size_t prefix_len = slash ? (size_t)(slash - target) + 1 : 0;
    size_t name_len = strlen(name);
    if (name_len == 0) {
        errno = EISDIR;
        return -1;
    }
    // "." + name + ".tmp-" + token must stay within one directory entry.
    size_t room = NAME_MAX - 1 - (sizeof(TEMP_SUFFIX) - 1) - TEMP_RAND_CHARS;
    size_t keep = utf8_cut(name, name_len, room);

    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz234567";
    for (int32_t attempt = 0; attempt < FSIO_TEMP_TRIES; attempt++) {
        uint64_t t = temp_token();
        char token[TEMP_RAND_CHARS + 1];
        for (int32_t i = 0; i < TEMP_RAND_CHARS; i++)
            token[i] = alphabet[(t >> (i * 5)) & 31];
        token[TEMP_RAND_CHARS] = '\0';

        if (!format_path(out, out_size, "%.*s.%.*s" TEMP_SUFFIX "%s", (int)prefix_len, target, (int)keep, name, token))
            return -1;
        int fd = open(out, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (fd >= 0)
            return fd;
        if (errno != EEXIST && errno != EINTR)
            return -1;
    }
    errno = EEXIST;
    return -1;
}

bool fsio_write_atomic(const char* path, const void* data, size_t len)
{
    if (!path || (!data && len > 0)) {
        errno = EINVAL;
        return false;
    }

    char target[PATH_MAX];
    if (!fsio_resolve_link(path, target, sizeof(target)))
        return false;

    struct stat st;
    bool existed = stat(target, &st) == 0;

    char tmp[PATH_MAX];
    int fd = fsio_create_temp(target, tmp, sizeof(tmp));
    if (fd < 0)
        return false;

    // The replacement keeps the old file's permission bits. A filesystem that has no Unix modes
    // (Android's shared storage, some FUSE mounts) refuses fchmod; the file then gets that
    // filesystem's fixed mode, the same one the old file had, so the write carries on.
    if (existed && fchmod(fd, st.st_mode & 07777) != 0) {
        // Deliberately not fatal; see above.
    }

    if (!write_all(fd, data, len) || fsync_retry(fd) != 0)
        goto fail;

    // After a successful fsync, a close that reports EINTR has still released the descriptor
    // (Linux, bionic) and the data is already on disk.
    int r = close(fd);
    fd = -1;
    if (r != 0 && errno != EINTR)
        goto fail;

    if (rename(tmp, target) != 0)
        goto fail;
    return sync_dir_of(target);

fail: {
    int e = errno;
    if (fd >= 0)
        close(fd);
    unlink(tmp);
    errno = e;
    return false;
}
}

// #endregion

// #region Reading

char* fsio_read_all(const char* path, size_t* out_len, size_t max_len)
{
    if (out_len)
        *out_len = 0;
    if (!path) {
        errno = EINVAL;
        return NULL;
    }
    if (max_len == SIZE_MAX)
        max_len = SIZE_MAX - 1; // room for the NUL

    int fd;
    do {
        fd = open(path, O_RDONLY | O_CLOEXEC);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return NULL;

    char* buf = NULL;
    size_t len = 0;
    size_t cap = 4096;

    struct stat st;
    if (fstat(fd, &st) != 0)
        goto fail;
    if (S_ISDIR(st.st_mode)) {
        errno = EISDIR;
        goto fail;
    }
    if (S_ISREG(st.st_mode)) {
        if (st.st_size < 0 || (uint64_t)st.st_size > (uint64_t)max_len) {
            errno = EFBIG;
            goto fail;
        }
        cap = (size_t)st.st_size + 1;
    }
    if (cap > max_len + 1)
        cap = max_len + 1;

    buf = malloc(cap);
    if (!buf)
        goto fail;

    for (;;) {
        if (len + 1 < cap) {
            ssize_t n = read_retry(fd, buf + len, cap - 1 - len);
            if (n < 0)
                goto fail;
            if (n == 0)
                break;
            len += (size_t)n;
            continue;
        }

        // Full to the size hint: read on, so a file that grew is not cut short.
        char chunk[4096];
        ssize_t n = read_retry(fd, chunk, sizeof(chunk));
        if (n < 0)
            goto fail;
        if (n == 0)
            break;
        if ((size_t)n > max_len - len) {
            errno = EFBIG;
            goto fail;
        }
        size_t need = len + (size_t)n + 1;
        size_t grow = cap > SIZE_MAX / 2 ? need : cap * 2;
        if (grow < need)
            grow = need;
        if (grow > max_len + 1)
            grow = max_len + 1;
        char* bigger = realloc(buf, grow);
        if (!bigger)
            goto fail;
        buf = bigger;
        cap = grow;
        memcpy(buf + len, chunk, (size_t)n);
        len += (size_t)n;
    }

    close(fd);
    buf[len] = '\0';
    if (out_len)
        *out_len = len;
    return buf;

fail: {
    int e = errno;
    free(buf);
    close(fd);
    errno = e;
    return NULL;
}
}

// #endregion

// #region Directories and names

bool fsio_mkdir_p(const char* path, mode_t mode)
{
    if (!path || !*path) {
        errno = ENOENT;
        return false;
    }
    char tmp[PATH_MAX];
    if (!copy_path(tmp, sizeof(tmp), path))
        return false;

    size_t n = strlen(tmp);
    while (n > 1 && tmp[n - 1] == '/')
        tmp[--n] = '\0';

    for (char* p = tmp + 1; *p; p++) {
        if (*p != '/' || p[-1] == '/')
            continue;
        *p = '\0';
        bool ok = make_dir(tmp, mode);
        *p = '/';
        if (!ok)
            return false;
    }
    return make_dir(tmp, mode);
}

bool fsio_move_no_replace(const char* from, const char* to)
{
    if (!from || !to) {
        errno = EINVAL;
        return false;
    }

    // linkat with no flags links a symlink itself rather than its target, on every platform
    // (plain link() follows it on macOS).
    if (linkat(AT_FDCWD, from, AT_FDCWD, to, 0) == 0) {
        if (unlink(from) == 0)
            return true;
        int e = errno;
        unlink(to); // the same inode we just linked: undo
        errno = e;
        return false;
    }

    int e = errno;
    if (!link_unsupported(e)) {
        errno = e; // EEXIST among them
        return false;
    }

    struct stat st;
    if (lstat(to, &st) == 0) {
        errno = EEXIST;
        return false;
    }
    if (errno != ENOENT)
        return false;
    return rename(from, to) == 0;
}

bool fsio_quarantine(const char* path, char* out, size_t out_size)
{
    if (!path || !*path) {
        errno = EINVAL;
        return false;
    }
    time_t now = time(NULL);
    struct tm utc;
    char stamp[32];
    if (!gmtime_r(&now, &utc) || strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%SZ", &utc) == 0) {
        errno = EOVERFLOW;
        return false;
    }

    char cand[PATH_MAX];
    for (int32_t i = 1; i <= 100; i++) {
        bool ok = i == 1 ? format_path(cand, sizeof(cand), "%s.corrupt-%s", path, stamp)
                         : format_path(cand, sizeof(cand), "%s.corrupt-%s-%d", path, stamp, (int)i);
        if (!ok)
            return false;
        if (out && strlen(cand) >= out_size) {
            errno = ENAMETOOLONG;
            return false;
        }
        if (fsio_move_no_replace(path, cand)) {
            if (out)
                memcpy(out, cand, strlen(cand) + 1);
            return true;
        }
        if (errno != EEXIST)
            return false;
    }
    errno = EEXIST;
    return false;
}

// #endregion

// #region Content checks

uint64_t fsio_hash(const void* data, size_t len)
{
    const unsigned char* p = data;
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

bool fsio_is_binary(const void* data, size_t len)
{
    return data && len > 0 && memchr(data, 0, len) != NULL;
}

// #endregion

#else

// The CMake glob builds this file on Windows too; one declaration keeps the translation unit
// non-empty (MSVC's C4206 under /W4 /WX, -Wpedantic under MinGW).
typedef int DawnFsioUnusedOnWindows;

#endif // _WIN32
