// dawn_fsio.h - Durable, crash-safe file primitives under every note dawn writes.
//
// The lowest storage layer, kept pure: nothing here touches dawn's globals, the backend vtable,
// notices or threads, only libc and POSIX, so tests/test_store.c can drive it in a scratch
// directory. dawn_backend_posix.c builds its filesystem functions on it; note-level policy
// (conflict detection, snapshots, history and settings) lives above it and decides what to do
// with the errors reported here.
//
// The rules it keeps:
// - A write replaces the whole file or nothing: the bytes go to a unique hidden temp file beside
//   the target, reach the disk (fsync), take the target's place in one rename, and the directory
//   entry is synced too. A failure before the rename leaves the old file exactly as it was.
// - A symlinked note stays a symlink: the file it points to is the one replaced.
// - Two dawn processes never share a temp name, and no call can replace a file it was told not to
//   (fsio_move_no_replace never uses renameat2: Android's seccomp filter kills the process on it).
// - Failures return false or NULL with errno set, so a caller can tell a missing file (ENOENT)
//   from a real error.
//
// POSIX only; on Windows the whole module compiles to nothing.

#ifndef DAWN_FSIO_H
#define DAWN_FSIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef _WIN32

#include <sys/types.h>

#define FSIO_MAX_LINKS 40 //!< Symlinks followed before a write gives up with ELOOP
#define FSIO_TEMP_TRIES 32 //!< Temp names tried before a write gives up with EEXIST

// #region Writing

//! Replace path with exactly len bytes of data, durably and atomically. A symlink at path is
//! followed (relative links against the link's directory, a dangling link to the path it names)
//! and the file it points to is replaced; the link itself survives. An existing file keeps its
//! permission bits; a new one gets 0666 minus the umask.
//! @return true once the data is in place and synced. false with errno set otherwise; when the
//!         failure came before the rename (the usual case) the old file is untouched and no temp
//!         file is left behind. A failed directory fsync after the rename also returns false: the
//!         new content is then visible but may not survive a power cut.
bool fsio_write_atomic(const char* path, const void* data, size_t len);

//! Create a new, empty, uniquely named temp file `.<name>.tmp-<random>` in the directory of
//! target (target's last component is <name>, cut so the result fits NAME_MAX), opened
//! O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC with mode 0666 minus the umask. The name goes to out.
//! @return the open file descriptor, or -1 with errno set (ENAMETOOLONG if out is too small).
int fsio_create_temp(const char* target, char* out, size_t out_size);

//! The file a write to path lands on: path itself when it is not a symlink, else the end of its
//! symlink chain (at most FSIO_MAX_LINKS links), which need not exist yet.
//! @return false with errno set (ELOOP, ENAMETOOLONG, a readlink error).
bool fsio_resolve_link(const char* path, char* out, size_t out_size);

// #endregion

// #region Reading

//! The whole file, NUL-terminated (the NUL is not counted in *out_len), read to EOF so a file that
//! grew since fstat is not cut short. out_len may be NULL.
//! @return a malloc'd buffer the caller frees, or NULL with errno set: ENOENT when the file does
//!         not exist, EFBIG when it holds more than max_len bytes, EISDIR for a directory, or the
//!         open/read error.
char* fsio_read_all(const char* path, size_t* out_len, size_t max_len);

// #endregion

// #region Directories and names

//! mkdir -p: create path and any missing parents, each one this call creates with mode (minus
//! the umask). Existing directories are fine.
//! @return false with errno set: ENAMETOOLONG for a path of PATH_MAX bytes or more (never
//!         truncated), ENOTDIR when a component exists and is not a directory, or the mkdir error.
bool fsio_mkdir_p(const char* path, mode_t mode);

//! Move from to to, never over an existing to (a dangling symlink at to counts as existing).
//! Atomic through link(2) + unlink(2) where hard links work; on a filesystem or SELinux policy
//! without them (EPERM, EACCES, ENOSYS, EXDEV, EMLINK, ENOTSUP) it checks with lstat and renames,
//! which leaves a tiny race window.
//! @return false with errno EEXIST when to exists, or another errno on failure.
bool fsio_move_no_replace(const char* from, const char* to);

//! Set a damaged file aside: rename path to `<path>.corrupt-<YYYYmmddTHHMMSSZ>` (UTC), with `-2`,
//! `-3`, ... added when that name is taken, never replacing anything. The name used goes to out
//! (out may be NULL when out_size is 0). A symlink at path is moved, not the file it points to.
//! @return false with errno set; ENAMETOOLONG when the new name does not fit out or PATH_MAX.
bool fsio_quarantine(const char* path, char* out, size_t out_size);

// #endregion

// #region Content checks

//! 64-bit FNV-1a of data: cheap "did the file change on disk" fingerprint. Stable across runs and
//! platforms; 0xcbf29ce484222325 for empty input.
uint64_t fsio_hash(const void* data, size_t len);

//! True when data holds a NUL byte, the usual sign of a file that is not text.
bool fsio_is_binary(const void* data, size_t len);

// #endregion

#endif // _WIN32

#endif // DAWN_FSIO_H
