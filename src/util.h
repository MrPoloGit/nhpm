/* util.h - strings, dynamic arrays, safe paths, atomic save, errors, logging.
 * SPDX-License-Identifier: NGPL */
#ifndef NHPM_UTIL_H
#define NHPM_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#if defined(__GNUC__) || defined(__clang__)
#define NHP_PRINTF(fmt_idx, args_idx) __attribute__((format(printf, fmt_idx, args_idx)))
#else
#define NHP_PRINTF(fmt_idx, args_idx)
#endif

#define NHP_VERSION      "0.1"
#define NHP_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* Error codes returned by functions throughout nhpm. NHP_OK (0) is
 * success; everything else is negative. See docs/style.md section 6.6. */
typedef enum nhp_err {
    NHP_OK = 0,
    NHP_ENOMEM = -1,     /* out of memory */
    NHP_EIO = -2,        /* filesystem or subprocess I/O failed */
    NHP_ENOENT = -3,     /* the thing being looked up doesn't exist */
    NHP_EEXIST = -4,     /* the thing being created already exists */
    NHP_EINVAL = -5,     /* a caller-supplied value doesn't make sense */
    NHP_ENET = -6,       /* a network request failed */
    NHP_EPARSE = -7,     /* malformed input (a file, a page, a diff) */
    NHP_ECONFLICT = -8,  /* a patch/quilt operation hit a conflict */
    NHP_ELOCKED = -9,    /* the instance is locked by another nhpm */
    NHP_ECANCELED = -10, /* the user canceled a long-running operation */
} nhp_err;

/* Sets the human-readable message returned by err_get(). Called by a
 * function right before it returns an error, so the UI/CLI can show why. */
void err_set(const char *fmt, ...) NHP_PRINTF(1, 2);

/* Returns the last message set with err_set(), or "" if none was set (or it
 * was cleared). Owned by util.c; the caller never frees it. */
const char *err_get(void);

void err_clear(void);

/* ---- logging: writes to ~/.cache/nhpm/nhpm.log (docs/file-formats.md) ---- */

typedef enum log_level {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
} log_level;

/* Opens `path` for appending; pass NULL to disable logging (the default
 * until this is called). `min_level` is the lowest level that gets written;
 * -v / log_level = debug (docs/style.md section 6.9) should pass LOG_DEBUG. */
void log_init(const char *path, log_level min_level);

void log_close(void);

void log_write(log_level level, const char *module, const char *fmt, ...) NHP_PRINTF(3, 4);

#define log_debug(module, ...) log_write(LOG_DEBUG, (module), __VA_ARGS__)
#define log_info(module, ...)  log_write(LOG_INFO, (module), __VA_ARGS__)
#define log_warn(module, ...)  log_write(LOG_WARN, (module), __VA_ARGS__)
#define log_error(module, ...) log_write(LOG_ERROR, (module), __VA_ARGS__)

/* ---- strings ---- */

/* Returns a newly allocated copy of s, or NULL if s is NULL or on OOM. */
char *str_dup(const char *s);

/* Returns a newly allocated, NUL-terminated copy of the first n bytes of s. */
char *str_ndup(const char *s, size_t n);

bool str_eq(const char *a, const char *b);

/* Case-insensitive substring search. True if `needle` occurs in
 * `haystack`; an empty `needle` always matches. (Not strcasestr(): that's
 * a BSD/GNU extension, not guaranteed under strict POSIX.) */
bool str_contains_ci(const char *haystack, const char *needle);

/* Trims leading and trailing ASCII whitespace from s in place; returns s. */
char *str_trim(char *s);

/* ---- growable byte buffer ---- */

typedef struct buf {
    char *data;
    size_t len;
    size_t cap;
} buf;

void buf_init(buf *b);
bool buf_append(buf *b, const char *data, size_t len);
bool buf_append_str(buf *b, const char *s);
bool buf_append_fmt(buf *b, const char *fmt, ...) NHP_PRINTF(2, 3);

/* Returns the buffer's contents as a NUL-terminated, newly allocated string
 * (the caller owns it), and resets *b to empty. Never returns NULL, even for
 * an empty buffer. */
char *buf_take(buf *b);

void buf_free(buf *b);

/* ---- paths ---- */

/* Returns a newly allocated "a/b" (collapsing a duplicate separator between
 * them), or NULL on OOM or if either argument is NULL. */
char *path_join(const char *a, const char *b);

/* True if `rel_path` is safe to extract or apply under some root: not NULL
 * or empty, not absolute, and no ".." component. */
bool path_is_safe(const char *rel_path);

/* ---- filesystem ---- */

bool fs_exists(const char *path);

/* Reads the whole file into a newly allocated, NUL-terminated buffer.
 * *out_len is the file's size, not counting the added NUL. */
nhp_err fs_read_file(const char *path, char **out_data, size_t *out_len);

/* Writes `data` to `path` by writing a temp file in the same directory,
 * fsync()ing it, then renaming it into place (docs/style.md section 6.8),
 * so a crash or full disk never leaves `path` half-written. */
nhp_err fs_write_atomic(const char *path, const char *data, size_t len);

/* Like "mkdir -p": creates `path` and any missing parent directories.
 * Not an error if `path` already exists as a directory. */
nhp_err fs_mkdir_p(const char *path, unsigned mode);

/* Recursively deletes `path`. Not an error if `path` doesn't exist. */
nhp_err fs_remove_tree(const char *path);

/* Recursively copies `src` to `dst` (creating `dst`), preserving
 * permission bits; symlinks/specials are skipped. Implemented natively
 * so util.c doesn't depend on proc.c. */
nhp_err fs_copy_tree(const char *src, const char *dst);

/* Creates a new temporary directory named "<prefix>-XXXXXX" under $TMPDIR
 * (falling back to /tmp) and returns its newly allocated path. */
char *fs_make_temp_dir(const char *prefix);

/* Writes `data` (length `len`) to a new temp file "<prefix>-XXXXXX"
 * under $TMPDIR, returning its path -- for handing an in-memory buffer
 * to an external tool (e.g. tar/unzip) that needs a real file. */
nhp_err fs_write_temp_file(const char *prefix, const char *data, size_t len, char **out_path);

/* The file's last-modified time, or NHP_ENOENT if it doesn't exist. */
nhp_err fs_mtime(const char *path, time_t *out_mtime);

/* ---- time ---- */

/* The current time, UTC, as "2026-11-15T20:14:03Z" -- the ISO 8601
 * format every timestamp field in instance.conf and elsewhere uses. */
char *time_now_iso8601(void);

/* ---- sha256 ---- */

/* Writes the lowercase hex SHA-256 digest of data[0..len) into out_hex,
 * which must have room for 65 bytes (64 hex digits + NUL). */
void sha256_hex(const void *data, size_t len, char out_hex[65]);

#endif /* NHPM_UTIL_H */
