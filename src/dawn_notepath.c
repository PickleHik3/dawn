// dawn_notepath.c - Pure naming rules for the files dawn keeps beside a note.
//
// See dawn_notepath.h. Nothing here reads or writes a file: the callers (dawn_file.c, dawn.c,
// dawn_title.c) decide whether a name is free and what to do when it is not.

#include "dawn_notepath.h"

#include <stdio.h>
#include <string.h>

const char* notepath_base(const char* path, size_t* stem_len)
{
    const char* slash = strrchr(path, '/');
    const char* bslash = strrchr(path, '\\');
    if (bslash && (!slash || bslash > slash))
        slash = bslash;
    const char* base = slash ? slash + 1 : path;
    size_t n = strlen(base);
    if (n > 3 && strcmp(base + n - 3, ".md") == 0)
        n -= 3;
    if (stem_len)
        *stem_len = n;
    return base;
}

//! snprintf into out, false when it was cut (or failed).
static bool fits(int w, size_t out_size) { return w >= 0 && (size_t)w < out_size; }

bool notepath_numbered(char* out, size_t out_size, const char* dir, size_t dir_len, char sep,
    const char* stem, int32_t n)
{
    if (!out || out_size == 0 || !stem || !stem[0])
        return false;
    char sep_s[2] = { sep, '\0' };
    const char* joiner = dir_len > 0 ? sep_s : "";
    int w;
    if (n <= 1)
        w = snprintf(out, out_size, "%.*s%s%s.md", (int)dir_len, dir ? dir : "", joiner, stem);
    else
        w = snprintf(out, out_size, "%.*s%s%s-%d.md", (int)dir_len, dir ? dir : "", joiner, stem, (int)n);
    return fits(w, out_size);
}

bool notepath_conflict(char* out, size_t out_size, const char* note_path, const char* stamp, int32_t n)
{
    if (!out || out_size == 0 || !note_path || !stamp)
        return false;
    size_t stem_len;
    const char* base = notepath_base(note_path, &stem_len);
    if (stem_len == 0)
        return false;
    size_t dir_len = (size_t)(base - note_path); // with its trailing separator
    int w;
    if (n <= 1)
        w = snprintf(out, out_size, "%.*s%.*s.conflict-%s.md", (int)dir_len, note_path, (int)stem_len, base, stamp);
    else
        w = snprintf(out, out_size, "%.*s%.*s.conflict-%s-%d.md", (int)dir_len, note_path, (int)stem_len, base,
            stamp, (int)n);
    return fits(w, out_size);
}

bool notepath_version_dir(char* out, size_t out_size, const char* note_path, bool in_notes_dir,
    uint64_t path_hash)
{
    if (!out || out_size == 0 || !note_path)
        return false;
    size_t stem_len;
    const char* base = notepath_base(note_path, &stem_len);

    // At most 127 bytes of the name, cut on a UTF-8 character boundary.
    char name[128];
    size_t n = 0;
    for (size_t i = 0; i < stem_len && n < sizeof(name) - 1; i++) {
        unsigned char c = (unsigned char)base[i];
        bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'
            || c == '_' || c == '.' || c >= 0x80;
        name[n++] = safe ? (char)c : '_';
    }
    // Cut inside a character (the next byte continues it): drop the part already copied.
    if (n < stem_len)
        while (n > 0 && ((unsigned char)base[n] & 0xC0) == 0x80)
            n--;
    name[n] = '\0';
    // "." and ".." are no directory names of a note's own.
    if (n == 0 || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        snprintf(name, sizeof(name), "note");

    int w = in_notes_dir ? snprintf(out, out_size, "%s", name)
                         : snprintf(out, out_size, "%s-%08x", name, (unsigned)(path_hash & 0xffffffffu));
    return fits(w, out_size);
}

bool notepath_is_stamp_name(const char* base, size_t n)
{
    static const char pattern[] = "dddd-dd-dd_dddddd";
    const size_t plen = sizeof(pattern) - 1;
    if (!base || n < plen)
        return false;
    for (size_t i = 0; i < plen; i++) {
        bool ok = pattern[i] == 'd' ? (base[i] >= '0' && base[i] <= '9') : base[i] == pattern[i];
        if (!ok)
            return false;
    }
    if (n == plen)
        return true;
    // -2 .. -99, as notepath_numbered writes them
    if (base[plen] != '-' || n < plen + 2 || n > plen + 3 || base[plen + 1] == '0')
        return false;
    int32_t num = 0;
    for (size_t i = plen + 1; i < n; i++) {
        if (base[i] < '0' || base[i] > '9')
            return false;
        num = num * 10 + (base[i] - '0');
    }
    return num >= 2 && num <= NOTEPATH_MAX_NUMBER;
}
