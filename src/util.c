/* util.c - strings, dynamic arrays, safe paths, atomic save, errors, logging.
 * SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <ftw.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- errors ---- */

static char g_err_buf[512] = "";

void err_set(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err_buf, sizeof(g_err_buf), fmt, ap);
    va_end(ap);
}

const char *err_get(void)
{
    return g_err_buf;
}

void err_clear(void)
{
    g_err_buf[0] = '\0';
}

/* ---- logging ---- */

static FILE *g_log_fp = NULL;
static log_level g_log_min = LOG_INFO;

void log_init(const char *path, log_level min_level)
{
    log_close();
    g_log_min = min_level;
    if (path == NULL) {
        return;
    }
    g_log_fp = fopen(path, "a");
}

void log_close(void)
{
    if (g_log_fp != NULL) {
        /* Best-effort: a failure closing the log is not worth reporting. */
        (void)fclose(g_log_fp);
        g_log_fp = NULL;
    }
}

static const char *level_name(log_level level)
{
    switch (level) {
        case LOG_DEBUG:
            return "DEBUG";
        case LOG_INFO:
            return "INFO";
        case LOG_WARN:
            return "WARN";
        case LOG_ERROR:
            return "ERROR";
        default:
            return "?";
    }
}

void log_write(log_level level, const char *module, const char *fmt, ...)
{
    if (g_log_fp == NULL || level < g_log_min) {
        return;
    }

    /* Best-effort from here on: a failure writing a log line isn't worth
     * reporting (there's nowhere good to report it to). */
    time_t now = time(NULL);
    struct tm tm_utc;
    (void)gmtime_r(&now, &tm_utc);
    char stamp[32];
    (void)strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);

    (void)fprintf(g_log_fp, "%s %-5s %-10s ", stamp, level_name(level), module);

    va_list ap;
    va_start(ap, fmt);
    (void)vfprintf(g_log_fp, fmt, ap);
    va_end(ap);

    (void)fputc('\n', g_log_fp);
    (void)fflush(g_log_fp);
}

/* ---- strings ---- */

char *str_dup(const char *s)
{
    if (s == NULL) {
        return NULL;
    }
    return str_ndup(s, strlen(s));
}

char *str_ndup(const char *s, size_t n)
{
    if (s == NULL) {
        return NULL;
    }
    char *out = malloc(n + 1);
    if (out == NULL) {
        err_set("out of memory duplicating a %zu-byte string", n);
        return NULL;
    }
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

bool str_eq(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return a == b;
    }
    return strcmp(a, b) == 0;
}

bool str_contains_ci(const char *haystack, const char *needle)
{
    if (needle[0] == '\0') {
        return true;
    }
    size_t needle_len = strlen(needle);
    size_t haystack_len = strlen(haystack);
    if (needle_len > haystack_len) {
        return false;
    }
    for (size_t i = 0; i + needle_len <= haystack_len; i++) {
        if (strncasecmp(haystack + i, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

char *str_trim(char *s)
{
    if (s == NULL) {
        return NULL;
    }

    size_t start = 0;
    while (s[start] != '\0' && isspace((unsigned char)s[start])) {
        start++;
    }

    size_t end = strlen(s);
    while (end > start && isspace((unsigned char)s[end - 1])) {
        end--;
    }

    size_t new_len = end - start;
    if (start > 0) {
        memmove(s, s + start, new_len);
    }
    s[new_len] = '\0';
    return s;
}

char *str_replace_all(const char *s, const char *needle, const char *replacement)
{
    if (s == NULL) {
        return NULL;
    }
    size_t needle_len = strlen(needle);
    if (needle_len == 0) {
        return str_dup(s);
    }

    buf out;
    buf_init(&out);
    size_t len = strlen(s);
    for (size_t i = 0; i < len;) {
        if (i + needle_len <= len && memcmp(s + i, needle, needle_len) == 0) {
            if (!buf_append_str(&out, replacement)) {
                buf_free(&out);
                return NULL;
            }
            i += needle_len;
        } else {
            if (!buf_append(&out, s + i, 1)) {
                buf_free(&out);
                return NULL;
            }
            i++;
        }
    }
    return buf_take(&out);
}

nhp_err str_split_whitespace(const char *s, char ***out_tokens, size_t *out_count)
{
    *out_tokens = NULL;
    *out_count = 0;
    if (s == NULL || s[0] == '\0') {
        return NHP_OK;
    }

    char *copy = str_dup(s);
    if (copy == NULL) {
        return NHP_ENOMEM;
    }
    size_t cap = 4;
    char **tokens = malloc(cap * sizeof(*tokens));
    if (tokens == NULL) {
        free(copy);
        return NHP_ENOMEM;
    }

    size_t count = 0;
    char *saveptr = NULL;
    const char *tok = strtok_r(copy, " \t", &saveptr);
    while (tok != NULL) {
        if (count == cap) {
            cap *= 2;
            char **grown = realloc(tokens, cap * sizeof(*grown));
            if (grown == NULL) {
                str_tokens_free(tokens, count);
                free(copy);
                return NHP_ENOMEM;
            }
            tokens = grown;
        }
        tokens[count] = str_dup(tok);
        if (tokens[count] == NULL) {
            str_tokens_free(tokens, count);
            free(copy);
            return NHP_ENOMEM;
        }
        count++;
        tok = strtok_r(NULL, " \t", &saveptr);
    }
    free(copy);
    *out_tokens = tokens;
    *out_count = count;
    return NHP_OK;
}

void str_tokens_free(char **tokens, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        free(tokens[i]);
    }
    free(tokens);
}

/* ---- growable byte buffer ---- */

void buf_init(buf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

static bool buf_reserve(buf *b, size_t extra)
{
    size_t needed = b->len + extra + 1; /* +1 for the NUL terminator */
    if (needed <= b->cap) {
        return true;
    }

    size_t new_cap = (b->cap == 0) ? 64 : b->cap;
    while (new_cap < needed) {
        new_cap *= 2;
    }

    char *new_data = realloc(b->data, new_cap);
    if (new_data == NULL) {
        err_set("out of memory growing a buffer to %zu bytes", new_cap);
        return false;
    }
    b->data = new_data;
    b->cap = new_cap;
    return true;
}

bool buf_append(buf *b, const char *data, size_t len)
{
    if (!buf_reserve(b, len)) {
        return false;
    }
    memcpy(b->data + b->len, data, len);
    b->len += len;
    b->data[b->len] = '\0';
    return true;
}

bool buf_append_str(buf *b, const char *s)
{
    return buf_append(b, s, strlen(s));
}

bool buf_append_fmt(buf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap_copy;
    va_copy(ap_copy, ap);
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (needed < 0) {
        va_end(ap_copy);
        err_set("formatting a buffer append failed");
        return false;
    }

    if (!buf_reserve(b, (size_t)needed)) {
        va_end(ap_copy);
        return false;
    }
    vsnprintf(b->data + b->len, (size_t)needed + 1, fmt, ap_copy);
    va_end(ap_copy);
    b->len += (size_t)needed;
    return true;
}

char *buf_take(buf *b)
{
    char *out = b->data;
    if (out == NULL) {
        out = str_dup("");
    }
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    return out;
}

void buf_free(buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

/* ---- paths ---- */

char *path_join(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return NULL;
    }

    size_t b_start = 0;
    while (b[b_start] == '/') {
        b_start++;
    }

    size_t a_len = strlen(a);
    if (a_len == 0) {
        return str_dup(b + b_start);
    }

    bool a_has_slash = a[a_len - 1] == '/';
    size_t sep_len = a_has_slash ? 0 : 1;
    size_t b_len = strlen(b + b_start);
    size_t total = a_len + sep_len + b_len;

    char *out = malloc(total + 1);
    if (out == NULL) {
        err_set("out of memory joining paths");
        return NULL;
    }
    memcpy(out, a, a_len);
    if (sep_len != 0) {
        out[a_len] = '/';
    }
    memcpy(out + a_len + sep_len, b + b_start, b_len);
    out[total] = '\0';
    return out;
}

bool path_is_safe(const char *rel_path)
{
    if (rel_path == NULL || rel_path[0] == '\0' || rel_path[0] == '/') {
        return false;
    }

    const char *p = rel_path;
    while (*p != '\0') {
        const char *seg_start = p;
        while (*p != '\0' && *p != '/') {
            p++;
        }
        size_t seg_len = (size_t)(p - seg_start);
        if (seg_len == 2 && seg_start[0] == '.' && seg_start[1] == '.') {
            return false;
        }
        if (*p == '/') {
            p++;
        }
    }
    return true;
}

/* ---- filesystem ---- */

bool fs_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

nhp_err fs_mtime(const char *path, time_t *out_mtime)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        if (errno == ENOENT) {
            return NHP_ENOENT;
        }
        err_set("could not stat %s: %s", path, strerror(errno));
        return NHP_EIO;
    }
    *out_mtime = st.st_mtime;
    return NHP_OK;
}

nhp_err fs_read_file(const char *path, char **out_data, size_t *out_len)
{
    *out_data = NULL;
    *out_len = 0;

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        err_set("could not open %s: %s", path, strerror(errno));
        return (errno == ENOENT) ? NHP_ENOENT : NHP_EIO;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        err_set("could not seek in %s: %s", path, strerror(errno));
        (void)fclose(fp); /* already failing; a close error wouldn't add anything */
        return NHP_EIO;
    }
    long size = ftell(fp);
    if (size < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        err_set("could not determine the size of %s", path);
        (void)fclose(fp);
        return NHP_EIO;
    }

    char *data = malloc((size_t)size + 1);
    if (data == NULL) {
        err_set("out of memory reading %s (%ld bytes)", path, size);
        (void)fclose(fp);
        return NHP_ENOMEM;
    }

    size_t got = fread(data, 1, (size_t)size, fp);
    bool close_ok = fclose(fp) == 0;
    if (got != (size_t)size) {
        err_set("could not read all of %s", path);
        free(data);
        return NHP_EIO;
    }
    if (!close_ok) {
        err_set("could not close %s: %s", path, strerror(errno));
        free(data);
        return NHP_EIO;
    }
    data[size] = '\0';

    *out_data = data;
    *out_len = (size_t)size;
    return NHP_OK;
}

nhp_err fs_write_atomic(const char *path, const char *data, size_t len)
{
    const char *slash = strrchr(path, '/');
    char *dir = (slash != NULL) ? str_ndup(path, (size_t)(slash - path)) : str_dup(".");
    if (dir == NULL) {
        return NHP_ENOMEM;
    }

    char *tmpl = path_join(dir, ".nhpm-tmp-XXXXXX");
    free(dir);
    if (tmpl == NULL) {
        return NHP_ENOMEM;
    }

    int fd = mkstemp(tmpl);
    if (fd < 0) {
        err_set("could not create a temporary file next to %s: %s", path, strerror(errno));
        free(tmpl);
        return NHP_EIO;
    }

    nhp_err result = NHP_OK;
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, data + written, len - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            err_set("could not write to %s: %s", tmpl, strerror(errno));
            result = NHP_EIO;
            break;
        }
        written += (size_t)n;
    }

    if (result == NHP_OK && fsync(fd) != 0) {
        err_set("could not fsync %s: %s", tmpl, strerror(errno));
        result = NHP_EIO;
    }

    if (close(fd) != 0 && result == NHP_OK) {
        err_set("could not close %s: %s", tmpl, strerror(errno));
        result = NHP_EIO;
    }

    if (result == NHP_OK && rename(tmpl, path) != 0) {
        err_set("could not rename %s to %s: %s", tmpl, path, strerror(errno));
        result = NHP_EIO;
    }

    if (result != NHP_OK) {
        (void)unlink(tmpl); /* best-effort cleanup; we're already returning an error */
    }

    free(tmpl);
    return result;
}

nhp_err fs_mkdir_p(const char *path, unsigned mode)
{
    if (path == NULL || path[0] == '\0') {
        return NHP_EINVAL;
    }

    char *copy = str_dup(path);
    if (copy == NULL) {
        return NHP_ENOMEM;
    }

    nhp_err err = NHP_OK;
    for (char *p = copy + 1; *p != '\0'; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (mkdir(copy, (mode_t)mode) != 0 && errno != EEXIST) {
            err_set("could not create directory %s: %s", copy, strerror(errno));
            err = NHP_EIO;
            *p = '/';
            goto done;
        }
        *p = '/';
    }

    if (mkdir(copy, (mode_t)mode) != 0 && errno != EEXIST) {
        err_set("could not create directory %s: %s", copy, strerror(errno));
        err = NHP_EIO;
    }

done:
    free(copy);
    return err;
}

static int remove_cb(const char *path, const struct stat *sb, int typeflag, struct FTW *ftwbuf)
{
    (void)sb;
    (void)typeflag;
    (void)ftwbuf;
    return remove(path);
}

nhp_err fs_remove_tree(const char *path)
{
    if (!fs_exists(path)) {
        return NHP_OK;
    }
    if (nftw(path, remove_cb, 16, FTW_DEPTH | FTW_PHYS) != 0) {
        err_set("could not remove %s: %s", path, strerror(errno));
        return NHP_EIO;
    }
    return NHP_OK;
}

/* nftw()'s callback has no user-data parameter, so fs_copy_tree() stashes
 * its state here. Not reentrant, but nhpm never copies two trees at once. */
static struct {
    size_t src_prefix_len;
    const char *dst_root;
    nhp_err err;
} g_copy_tree_state;

static int copy_tree_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf)
{
    (void)ftwbuf;
    if (g_copy_tree_state.err != NHP_OK) {
        return 0; /* already failed; unwind without doing more work */
    }

    const char *rel = fpath + g_copy_tree_state.src_prefix_len;
    char *dst_path = (rel[0] == '\0') ? str_dup(g_copy_tree_state.dst_root)
                                      : path_join(g_copy_tree_state.dst_root, rel);
    if (dst_path == NULL) {
        g_copy_tree_state.err = NHP_ENOMEM;
        return 0;
    }

    if (typeflag == FTW_D || typeflag == FTW_DP) {
        if (mkdir(dst_path, (sb->st_mode & 0777) | 0700) != 0 && errno != EEXIST) {
            err_set("could not create directory %s: %s", dst_path, strerror(errno));
            g_copy_tree_state.err = NHP_EIO;
        }
    } else if (typeflag == FTW_F) {
        char *data = NULL;
        size_t len = 0;
        nhp_err err = fs_read_file(fpath, &data, &len);
        if (err == NHP_OK) {
            err = fs_write_atomic(dst_path, data, len);
            free(data);
        }
        if (err == NHP_OK && chmod(dst_path, sb->st_mode & 0777) != 0) {
            err_set("could not set permissions on %s: %s", dst_path, strerror(errno));
            err = NHP_EIO;
        }
        if (err != NHP_OK) {
            g_copy_tree_state.err = err;
        }
    }
    /* symlinks (FTW_SL) and other special files are skipped silently; a
     * NetHack source tree doesn't contain any. */

    free(dst_path);
    return 0;
}

nhp_err fs_copy_tree(const char *src, const char *dst)
{
    char *src_norm = str_dup(src);
    if (src_norm == NULL) {
        return NHP_ENOMEM;
    }
    size_t src_len = strlen(src_norm);
    while (src_len > 1 && src_norm[src_len - 1] == '/') {
        src_norm[--src_len] = '\0';
    }

    nhp_err err = fs_mkdir_p(dst, 0700);
    if (err != NHP_OK) {
        free(src_norm);
        return err;
    }

    g_copy_tree_state.src_prefix_len = src_len;
    g_copy_tree_state.dst_root = dst;
    g_copy_tree_state.err = NHP_OK;

    if (nftw(src_norm, copy_tree_cb, 16, FTW_PHYS) != 0 && g_copy_tree_state.err == NHP_OK) {
        err_set("could not walk %s: %s", src_norm, strerror(errno));
        g_copy_tree_state.err = NHP_EIO;
    }
    free(src_norm);
    return g_copy_tree_state.err;
}

char *fs_make_temp_dir(const char *prefix)
{
    const char *tmp_root = getenv("TMPDIR");
    if (tmp_root == NULL || tmp_root[0] == '\0') {
        tmp_root = "/tmp";
    }

    buf name;
    buf_init(&name);
    if (!buf_append_fmt(&name, "%s-XXXXXX", prefix)) {
        buf_free(&name);
        return NULL;
    }
    char *name_str = buf_take(&name);

    char *tmpl = path_join(tmp_root, name_str);
    free(name_str);
    if (tmpl == NULL) {
        return NULL;
    }

    if (mkdtemp(tmpl) == NULL) {
        err_set("could not create a temporary directory under %s: %s", tmp_root, strerror(errno));
        free(tmpl);
        return NULL;
    }
    return tmpl;
}

nhp_err fs_write_temp_file(const char *prefix, const char *data, size_t len, char **out_path)
{
    const char *tmp_root = getenv("TMPDIR");
    if (tmp_root == NULL || tmp_root[0] == '\0') {
        tmp_root = "/tmp";
    }

    buf name;
    buf_init(&name);
    if (!buf_append_fmt(&name, "%s-XXXXXX", prefix)) {
        buf_free(&name);
        return NHP_ENOMEM;
    }
    char *name_str = buf_take(&name);
    char *tmpl = path_join(tmp_root, name_str);
    free(name_str);
    if (tmpl == NULL) {
        return NHP_ENOMEM;
    }

    int fd = mkstemp(tmpl);
    if (fd < 0) {
        err_set("could not create a temporary file under %s: %s", tmp_root, strerror(errno));
        free(tmpl);
        return NHP_EIO;
    }

    nhp_err result = NHP_OK;
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, data + written, len - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            err_set("could not write to %s: %s", tmpl, strerror(errno));
            result = NHP_EIO;
            break;
        }
        written += (size_t)n;
    }

    if (close(fd) != 0 && result == NHP_OK) {
        err_set("could not close %s: %s", tmpl, strerror(errno));
        result = NHP_EIO;
    }

    if (result != NHP_OK) {
        (void)unlink(tmpl);
        free(tmpl);
        return result;
    }

    *out_path = tmpl;
    return NHP_OK;
}

/* ---- sha256 (FIPS 180-4) ---- */

// NOLINTNEXTLINE(readability-identifier-naming) -- snake_case data table, not a manifest constant
static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

typedef struct sha256_ctx {
    uint32_t state[8];
    uint8_t block[64];
    size_t block_len;
    uint64_t total_bits;
} sha256_ctx;

static uint32_t rotr32(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

/* Processes exactly one 64-byte block, updating ctx->state. */
static void sha256_process_block(sha256_ctx *ctx, const uint8_t block[64])
{
    uint32_t w[64];
    for (size_t i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    }
    for (size_t i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    uint32_t f = ctx->state[5];
    uint32_t g = ctx->state[6];
    uint32_t h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t temp1 = h + s1 + ch + sha256_k[i] + w[i];
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_init(sha256_ctx *ctx)
{
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->block_len = 0;
    ctx->total_bits = 0;
}

static void sha256_update(sha256_ctx *ctx, const void *data, size_t len)
{
    const uint8_t *p = data;
    ctx->total_bits += (uint64_t)len * 8;

    while (len > 0) {
        size_t take = 64 - ctx->block_len;
        if (take > len) {
            take = len;
        }
        memcpy(ctx->block + ctx->block_len, p, take);
        ctx->block_len += take;
        p += take;
        len -= take;

        if (ctx->block_len == 64) {
            sha256_process_block(ctx, ctx->block);
            ctx->block_len = 0;
        }
    }
}

static void sha256_final(sha256_ctx *ctx, uint8_t digest[32])
{
    uint8_t pad[72]; /* worst case: 1 (0x80) + 63 zero pad bytes + 8 length bytes */
    size_t pad_len = 0;
    pad[pad_len++] = 0x80;

    size_t zeros = (ctx->block_len <= 55) ? (55 - ctx->block_len) : (119 - ctx->block_len);
    memset(pad + pad_len, 0, zeros);
    pad_len += zeros;

    for (int i = 7; i >= 0; i--) {
        pad[pad_len++] = (uint8_t)(ctx->total_bits >> (i * 8));
    }

    sha256_update(ctx, pad, pad_len);
    /* sha256_update() just processed the final block(s); block_len is 0 now. */

    for (size_t i = 0; i < 8; i++) {
        digest[i * 4] = (uint8_t)(ctx->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

char *time_now_iso8601(void)
{
    time_t now = time(NULL);
    struct tm tm_utc;
    (void)gmtime_r(&now, &tm_utc);
    char stamp[32];
    (void)strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return str_dup(stamp);
}

void sha256_hex(const void *data, size_t len, char out_hex[65])
{
    sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);

    uint8_t digest[32];
    sha256_final(&ctx, digest);

    static const char hex_digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; i++) {
        out_hex[i * 2] = hex_digits[digest[i] >> 4];
        out_hex[i * 2 + 1] = hex_digits[digest[i] & 0x0f];
    }
    out_hex[64] = '\0';
}
