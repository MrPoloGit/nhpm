/* library.c - downloaded patches: add, list, delete versions, verify.
 * SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "library.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "net.h"
#include "proc.h"

#define DETAIL_CACHE_MAX_AGE_SECONDS ((time_t)7 * 24 * 3600)
#define LOCAL_ID_PREFIX              "local-"

/* ---- paths and names ---- */

static char *library_dir(void)
{
    char *data_dir = cfg_xdg_data_dir();
    if (data_dir == NULL) {
        return NULL;
    }
    char *dir = path_join(data_dir, "library");
    free(data_dir);
    return dir;
}

/* "(New) Dumplog" -> "new-dumplog": lowercase, runs of non [a-z0-9]
 * collapsed to one '-', leading/trailing '-' trimmed. */
static char *make_slug(const char *name)
{
    buf b;
    buf_init(&b);
    bool pending_dash = false;
    bool any_char = false;
    for (const char *p = name; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c)) {
            if (pending_dash && any_char) {
                buf_append(&b, "-", 1);
            }
            char lower = (char)tolower(c);
            buf_append(&b, &lower, 1);
            pending_dash = false;
            any_char = true;
        } else {
            pending_dash = true;
        }
    }
    if (b.len == 0) {
        buf_append_str(&b, "patch");
    }
    return buf_take(&b);
}

/* Makes `s` safe to use as a single path component: no '/' or control
 * characters, never empty. Returns a newly allocated copy. */
static char *sanitize_component(const char *s)
{
    buf b;
    buf_init(&b);
    for (const char *p = s; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '/' || iscntrl(c)) {
            buf_append(&b, "_", 1);
        } else {
            buf_append(&b, (const char *)&c, 1);
        }
    }
    if (b.len == 0) {
        buf_append_str(&b, "_");
    }
    return buf_take(&b);
}

static bool is_local_id(const char *id)
{
    return strncmp(id, LOCAL_ID_PREFIX, strlen(LOCAL_ID_PREFIX)) == 0;
}

/* The library folder name for a patch: the bare ID for local patches
 * (which already read as "local-<slug>"), or "<id>-<slug(name)>" for
 * patches from the site (docs/file-formats.md: "library/485-new-dumplog/"). */
static char *patch_folder_name(const char *id, const char *name)
{
    if (is_local_id(id)) {
        return str_dup(id);
    }
    char *slug = make_slug(name);
    if (slug == NULL) {
        return NULL;
    }
    buf b;
    buf_init(&b);
    buf_append_fmt(&b, "%s-%s", id, slug);
    free(slug);
    return buf_take(&b);
}

/* Scans the library for the one folder belonging to `id` (its name isn't
 * predictable from `id` alone for site patches, since it also encodes a
 * slug of the patch's name). NULL (not an error) if there's no such patch. */
static char *find_patch_dir(const char *id)
{
    char *lib_dir = library_dir();
    if (lib_dir == NULL) {
        return NULL;
    }

    if (is_local_id(id)) {
        char *dir = path_join(lib_dir, id);
        free(lib_dir);
        if (dir != NULL && fs_exists(dir)) {
            return dir;
        }
        free(dir);
        return NULL;
    }

    DIR *dp = opendir(lib_dir);
    if (dp == NULL) {
        free(lib_dir);
        return NULL;
    }

    size_t id_len = strlen(id);
    char *found = NULL;
    const struct dirent *entry;
    while (found == NULL && (entry = readdir(dp)) != NULL) {
        const char *name = entry->d_name;
        if (strncmp(name, id, id_len) == 0 && name[id_len] == '-') {
            found = path_join(lib_dir, name);
        }
    }
    (void)closedir(dp);
    free(lib_dir);
    return found;
}

/* ---- patch.meta / version.meta ---- */

static nhp_err load_patch_meta(const char *patch_dir, lib_patch *out)
{
    char *path = path_join(patch_dir, "patch.meta");
    if (path == NULL) {
        return NHP_ENOMEM;
    }

    cfg_doc doc;
    cfg_doc_init(&doc);
    nhp_err err = cfg_doc_load(path, &doc);
    free(path);
    if (err != NHP_OK) {
        cfg_doc_free(&doc);
        return err;
    }

    out->id = str_dup(cfg_get(&doc, NULL, "id", ""));
    out->name = str_dup(cfg_get(&doc, NULL, "name", ""));
    out->author = str_dup(cfg_get(&doc, NULL, "author", ""));
    out->target = str_dup(cfg_get(&doc, NULL, "target", ""));
    out->source_page = str_dup(cfg_get(&doc, NULL, "source_page", ""));
    cfg_doc_free(&doc);

    if (out->id == NULL || out->name == NULL || out->author == NULL || out->target == NULL ||
        out->source_page == NULL) {
        return NHP_ENOMEM;
    }
    return NHP_OK;
}

static nhp_err save_patch_meta(const char *patch_dir, const lib_patch *p)
{
    nhp_err err = fs_mkdir_p(patch_dir, 0700);
    if (err != NHP_OK) {
        return err;
    }
    char *path = path_join(patch_dir, "patch.meta");
    if (path == NULL) {
        return NHP_ENOMEM;
    }

    cfg_doc doc;
    cfg_doc_init(&doc);
    err = cfg_set(&doc, NULL, "id", p->id);
    if (err == NHP_OK) {
        err = cfg_set(&doc, NULL, "name", p->name);
    }
    if (err == NHP_OK) {
        err = cfg_set(&doc, NULL, "author", p->author);
    }
    if (err == NHP_OK) {
        err = cfg_set(&doc, NULL, "target", p->target);
    }
    if (err == NHP_OK) {
        err = cfg_set(&doc, NULL, "source_page", p->source_page);
    }
    if (err == NHP_OK) {
        err = cfg_doc_save(path, &doc);
    }
    cfg_doc_free(&doc);
    free(path);
    return err;
}

static nhp_err load_version_meta(const char *version_dir, lib_version *out)
{
    char *path = path_join(version_dir, "version.meta");
    if (path == NULL) {
        return NHP_ENOMEM;
    }

    cfg_doc doc;
    cfg_doc_init(&doc);
    nhp_err err = cfg_doc_load(path, &doc);
    free(path);
    if (err != NHP_OK) {
        cfg_doc_free(&doc);
        return err;
    }

    out->version = str_dup(cfg_get(&doc, NULL, "version", ""));
    out->downloaded = str_dup(cfg_get(&doc, NULL, "downloaded", ""));
    out->url = str_dup(cfg_get(&doc, NULL, "url", ""));
    out->via_wayback = cfg_get_bool(&doc, NULL, "via_wayback", false);
    out->sha256 = str_dup(cfg_get(&doc, NULL, "sha256", ""));
    out->files_touched = str_dup(cfg_get(&doc, NULL, "files_touched", ""));
    cfg_doc_free(&doc);

    if (out->version == NULL || out->downloaded == NULL || out->url == NULL ||
        out->sha256 == NULL || out->files_touched == NULL) {
        return NHP_ENOMEM;
    }
    return NHP_OK;
}

static nhp_err save_version_meta(const char *version_dir, const lib_version *v)
{
    nhp_err err = fs_mkdir_p(version_dir, 0700);
    if (err != NHP_OK) {
        return err;
    }
    char *path = path_join(version_dir, "version.meta");
    if (path == NULL) {
        return NHP_ENOMEM;
    }

    cfg_doc doc;
    cfg_doc_init(&doc);
    err = cfg_set(&doc, NULL, "version", v->version);
    if (err == NHP_OK) {
        err = cfg_set(&doc, NULL, "downloaded", v->downloaded);
    }
    if (err == NHP_OK) {
        err = cfg_set(&doc, NULL, "url", v->url);
    }
    if (err == NHP_OK) {
        err = cfg_set(&doc, NULL, "via_wayback", v->via_wayback ? "yes" : "no");
    }
    if (err == NHP_OK) {
        err = cfg_set(&doc, NULL, "sha256", v->sha256);
    }
    if (err == NHP_OK) {
        err = cfg_set(&doc, NULL, "files_touched", v->files_touched);
    }
    if (err == NHP_OK) {
        err = cfg_doc_save(path, &doc);
    }
    cfg_doc_free(&doc);
    free(path);
    return err;
}

static void lib_version_free_contents(lib_version *v)
{
    free(v->version);
    free(v->downloaded);
    free(v->url);
    free(v->sha256);
    free(v->files_touched);
    memset(v, 0, sizeof(*v));
}

void lib_patch_free(lib_patch *p)
{
    for (size_t i = 0; i < p->version_count; i++) {
        lib_version_free_contents(&p->versions[i]);
    }
    free(p->versions);
    free(p->id);
    free(p->name);
    free(p->author);
    free(p->target);
    free(p->source_page);
    memset(p, 0, sizeof(*p));
}

void lib_list_free(lib_patch *patches, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        lib_patch_free(&patches[i]);
    }
    free(patches);
}

static int compare_strings(const void *a, const void *b)
{
    const char *const *sa = a;
    const char *const *sb = b;
    return strcmp(*sa, *sb);
}

/* Reads every version subdirectory of `patch_dir` (any entry with a
 * version.meta in it) into *p, oldest-looking-name first. */
static nhp_err load_versions(const char *patch_dir, lib_patch *p)
{
    DIR *dp = opendir(patch_dir);
    if (dp == NULL) {
        err_set("could not open %s: %s", patch_dir, strerror(errno));
        return NHP_EIO;
    }

    char **names = NULL;
    size_t name_count = 0;
    size_t name_cap = 0;
    const struct dirent *entry;
    nhp_err err = NHP_OK;

    while (err == NHP_OK && (entry = readdir(dp)) != NULL) {
        if (str_eq(entry->d_name, ".") || str_eq(entry->d_name, "..")) {
            continue;
        }
        char *child = path_join(patch_dir, entry->d_name);
        struct stat st;
        bool is_version_dir = child != NULL && stat(child, &st) == 0 && S_ISDIR(st.st_mode);
        free(child);
        if (!is_version_dir) {
            continue;
        }
        if (name_count == name_cap) {
            size_t new_cap = (name_cap == 0) ? 8 : name_cap * 2;
            char **grown = realloc(names, new_cap * sizeof(*grown));
            if (grown == NULL) {
                err = NHP_ENOMEM;
                break;
            }
            names = grown;
            name_cap = new_cap;
        }
        names[name_count] = str_dup(entry->d_name);
        if (names[name_count] == NULL) {
            err = NHP_ENOMEM;
            break;
        }
        name_count++;
    }
    (void)closedir(dp);

    if (err == NHP_OK && name_count > 0) {
        qsort(names, name_count, sizeof(*names), compare_strings);
        p->versions = calloc(name_count, sizeof(*p->versions));
        if (p->versions == NULL) {
            err = NHP_ENOMEM;
        } else {
            for (size_t i = 0; i < name_count && err == NHP_OK; i++) {
                char *version_dir = path_join(patch_dir, names[i]);
                if (version_dir == NULL) {
                    err = NHP_ENOMEM;
                    break;
                }
                err = load_version_meta(version_dir, &p->versions[i]);
                free(version_dir);
                if (err == NHP_OK) {
                    p->version_count++;
                }
            }
        }
    }

    for (size_t i = 0; i < name_count; i++) {
        free(names[i]);
    }
    free(names);
    return err;
}

nhp_err lib_find(const char *id, lib_patch *out)
{
    memset(out, 0, sizeof(*out));

    char *patch_dir = find_patch_dir(id);
    if (patch_dir == NULL) {
        err_set("%s is not in the library", id);
        return NHP_ENOENT;
    }

    nhp_err err = load_patch_meta(patch_dir, out);
    if (err == NHP_OK) {
        err = load_versions(patch_dir, out);
    }
    free(patch_dir);
    if (err != NHP_OK) {
        lib_patch_free(out);
    }
    return err;
}

nhp_err lib_list(lib_patch **out_patches, size_t *out_count)
{
    *out_patches = NULL;
    *out_count = 0;

    char *lib_dir = library_dir();
    if (lib_dir == NULL) {
        return NHP_ENOMEM;
    }

    DIR *dp = opendir(lib_dir);
    if (dp == NULL) {
        if (errno == ENOENT) {
            free(lib_dir);
            return NHP_OK; /* no library yet: an empty list, not an error */
        }
        err_set("could not open %s: %s", lib_dir, strerror(errno));
        free(lib_dir);
        return NHP_EIO;
    }

    lib_patch *patches = NULL;
    size_t count = 0;
    size_t cap = 0;
    const struct dirent *entry;
    nhp_err err = NHP_OK;

    while (err == NHP_OK && (entry = readdir(dp)) != NULL) {
        if (str_eq(entry->d_name, ".") || str_eq(entry->d_name, "..")) {
            continue;
        }
        char *patch_dir = path_join(lib_dir, entry->d_name);
        if (patch_dir == NULL) {
            err = NHP_ENOMEM;
            break;
        }
        struct stat st;
        bool is_patch_dir = stat(patch_dir, &st) == 0 && S_ISDIR(st.st_mode);
        if (!is_patch_dir) {
            free(patch_dir);
            continue;
        }

        if (count == cap) {
            size_t new_cap = (cap == 0) ? 8 : cap * 2;
            lib_patch *grown = realloc(patches, new_cap * sizeof(*grown));
            if (grown == NULL) {
                err = NHP_ENOMEM;
                free(patch_dir);
                break;
            }
            patches = grown;
            cap = new_cap;
        }

        memset(&patches[count], 0, sizeof(patches[count]));
        err = load_patch_meta(patch_dir, &patches[count]);
        if (err == NHP_OK) {
            err = load_versions(patch_dir, &patches[count]);
        }
        free(patch_dir);
        if (err != NHP_OK) {
            lib_patch_free(&patches[count]);
            break;
        }
        count++;
    }
    (void)closedir(dp);
    free(lib_dir);

    if (err != NHP_OK) {
        lib_list_free(patches, count);
        return err;
    }
    *out_patches = patches;
    *out_count = count;
    return NHP_OK;
}

/* ---- detail page cache ---- */

static char *detail_cache_path(const char *id)
{
    char *cache_dir = cfg_xdg_cache_dir();
    if (cache_dir == NULL) {
        return NULL;
    }
    char *pages_dir = path_join(cache_dir, "pages");
    free(cache_dir);
    if (pages_dir == NULL) {
        return NULL;
    }

    buf filename_buf;
    buf_init(&filename_buf);
    if (!buf_append_fmt(&filename_buf, "%s.html", id)) {
        buf_free(&filename_buf);
        free(pages_dir);
        return NULL;
    }
    char *filename = buf_take(&filename_buf);
    char *path = path_join(pages_dir, filename);
    free(filename);
    free(pages_dir);
    return path;
}

nhp_err lib_fetch_detail(const char *id, site_detail *out)
{
    char *cache_path = detail_cache_path(id);
    if (cache_path == NULL) {
        return NHP_ENOMEM;
    }

    char *html = NULL;
    size_t html_len = 0;
    time_t mtime;
    bool have_fresh_cache = false;
    if (fs_mtime(cache_path, &mtime) == NHP_OK) {
        time_t now = time(NULL);
        if (now >= mtime && now - mtime < DETAIL_CACHE_MAX_AGE_SECONDS &&
            fs_read_file(cache_path, &html, &html_len) == NHP_OK) {
            have_fresh_cache = true;
        }
    }

    if (!have_fresh_cache) {
        char *dir = cfg_xdg_cache_dir();
        char *pages_dir = (dir != NULL) ? path_join(dir, "pages") : NULL;
        free(dir);
        if (pages_dir != NULL) {
            (void)fs_mkdir_p(pages_dir, 0700);
            free(pages_dir);
        }

        char *url = site_detail_url(id);
        if (url == NULL) {
            free(cache_path);
            return NHP_ENOMEM;
        }
        net_resp resp = {0};
        nhp_err err = net_get(url, &resp, NULL, NULL);
        free(url);
        if (err != NHP_OK) {
            net_resp_free(&resp);
            free(cache_path);
            return err;
        }

        /* best-effort: a failure to cache doesn't stop us using the page
         * we just fetched. */
        (void)fs_write_atomic(cache_path, resp.data, resp.len);

        html = resp.data;
        html_len = resp.len;
        resp.data = NULL; /* ownership moved to html */
        net_resp_free(&resp);
    }
    free(cache_path);

    nhp_err err = site_parse_detail_page(html, html_len, out);
    free(html);
    return err;
}

/* ---- diff detection (docs/features.md section 7.4, step 4) ---- */

/* True if `line` (len bytes, no trailing newline) looks like an ed-style
 * "plain" diff hunk header, e.g. "3,5c3,5", "10a11", "7d6". */
static bool looks_like_plain_hunk_header(const char *line, size_t len)
{
    size_t i = 0;
    // cppcheck-suppress unsignedLessThanZero ; i is 0 here, not negative; this is a normal bounds
    // check
    if (i >= len || !isdigit((unsigned char)line[i])) {
        return false;
    }
    while (i < len && isdigit((unsigned char)line[i])) {
        i++;
    }
    if (i < len && line[i] == ',') {
        i++;
        if (i >= len || !isdigit((unsigned char)line[i])) {
            return false;
        }
        while (i < len && isdigit((unsigned char)line[i])) {
            i++;
        }
    }
    if (i >= len || (line[i] != 'a' && line[i] != 'c' && line[i] != 'd')) {
        return false;
    }
    i++;
    if (i >= len || !isdigit((unsigned char)line[i])) {
        return false;
    }
    while (i < len && isdigit((unsigned char)line[i])) {
        i++;
    }
    if (i < len && line[i] == ',') {
        i++;
        if (i >= len || !isdigit((unsigned char)line[i])) {
            return false;
        }
        while (i < len && isdigit((unsigned char)line[i])) {
            i++;
        }
    }
    return i == len;
}

/* Calls `fn` once per line of `data` (len bytes; lines may be terminated by
 * "\n", "\r\n", or not terminated at all for the last line), with the
 * trailing line ending stripped. Stops early if `fn` returns true. */
static void for_each_line(const char *data, size_t len,
                          bool (*fn)(const char *line, size_t line_len, void *user_data),
                          void *user_data)
{
    const char *p = data;
    const char *end = data + len;
    while (p < end) {
        const char *line_start = p;
        while (p < end && *p != '\n') {
            p++;
        }
        size_t line_len = (size_t)(p - line_start);
        if (line_len > 0 && line_start[line_len - 1] == '\r') {
            line_len--;
        }
        if (fn(line_start, line_len, user_data)) {
            return;
        }
        if (p < end) {
            p++; /* skip the '\n' */
        }
    }
}

typedef struct diff_sniff_state {
    bool is_diff;
} diff_sniff_state;

static bool sniff_line(const char *line, size_t len, void *user_data)
{
    diff_sniff_state *state = user_data;
    /* unified diff hunk header, context diff hunk separator, or ed-style
     * "plain" diff hunk header */
    bool unified = len >= 3 && strncmp(line, "@@ ", 3) == 0;
    bool context = len >= 15 && strncmp(line, "***************", 15) == 0;
    bool plain = looks_like_plain_hunk_header(line, len);
    state->is_diff = unified || context || plain;
    return state->is_diff;
}

bool lib_looks_like_diff(const char *data, size_t len)
{
    diff_sniff_state state = {.is_diff = false};
    for_each_line(data, len, sniff_line, &state);
    return state.is_diff;
}

/* ---- CRLF normalization ---- */

static char *normalize_line_endings(const char *data, size_t len, size_t *out_len)
{
    buf b;
    buf_init(&b);
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\r') {
            buf_append(&b, "\n", 1);
            if (i + 1 < len && data[i + 1] == '\n') {
                i++;
            }
        } else {
            buf_append(&b, data + i, 1);
        }
    }
    *out_len = b.len;
    return buf_take(&b);
}

/* ---- files touched (best-effort, for version.meta) ---- */

static void append_unique_path(buf *out, const char *path, size_t len)
{
    if (len == 0 || (len == 9 && strncmp(path, "/dev/null", 9) == 0)) {
        return;
    }
    /* strip a "a/" or "b/" prefix, if present */
    if (len > 2 && (path[0] == 'a' || path[0] == 'b') && path[1] == '/') {
        path += 2;
        len -= 2;
    }
    /* a unified-diff path line ends with a tab-separated timestamp */
    const char *tab = memchr(path, '\t', len);
    if (tab != NULL) {
        len = (size_t)(tab - path);
    }
    if (len == 0) {
        return;
    }

    /* skip if already present */
    for (size_t i = 0; i + len <= out->len; i++) {
        if (memcmp(out->data + i, path, len) == 0 && (i == 0 || out->data[i - 1] == ' ') &&
            (i + len == out->len || out->data[i + len] == ' ')) {
            return;
        }
    }

    if (out->len > 0) {
        buf_append(out, " ", 1);
    }
    buf_append(out, path, len);
}

typedef struct files_touched_state {
    buf *out;
} files_touched_state;

static bool collect_touched_line(const char *line, size_t len, void *user_data)
{
    if (len >= 4 && (strncmp(line, "--- ", 4) == 0 || strncmp(line, "+++ ", 4) == 0 ||
                     strncmp(line, "*** ", 4) == 0)) {
        files_touched_state *state = user_data;
        const char *path = line + 4;
        size_t path_len = len - 4;
        append_unique_path(state->out, path, path_len);
    }
    return false;
}

/* Best-effort list of files a diff touches, for version.meta's
 * files_touched (docs/file-formats.md); "" if none could be identified
 * (e.g. a "plain" ed-style diff, which carries no file names at all). */
static char *extract_files_touched(const char *diff_text, size_t diff_len)
{
    buf out;
    buf_init(&out);
    files_touched_state state = {.out = &out};
    for_each_line(diff_text, diff_len, collect_touched_line, &state);
    return buf_take(&out);
}

/* ---- archive extraction (docs/features.md 7.4 step 5): shells out to
 * tar/gzip/unzip via proc.c -- no shell, no need to implement our own. ---- */

typedef enum archive_kind {
    ARCHIVE_NONE,
    ARCHIVE_GZIP, /* a single gzip-compressed file, not a tarball */
    ARCHIVE_TARGZ,
    ARCHIVE_ZIP,
} archive_kind;

static bool url_has_suffix(const char *url, const char *suffix)
{
    size_t url_len = strlen(url);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > url_len) {
        return false;
    }
    for (size_t i = 0; i < suffix_len; i++) {
        if (tolower((unsigned char)url[url_len - suffix_len + i]) != suffix[i]) {
            return false;
        }
    }
    return true;
}

static archive_kind detect_archive_kind(const char *data, size_t len, const char *url)
{
    bool gzip_magic = len >= 2 && (unsigned char)data[0] == 0x1f && (unsigned char)data[1] == 0x8b;
    bool zip_magic =
        len >= 4 && data[0] == 'P' && data[1] == 'K' && data[2] == 0x03 && data[3] == 0x04;

    if (zip_magic) {
        return ARCHIVE_ZIP;
    }
    if (gzip_magic) {
        if (url_has_suffix(url, ".tar.gz") || url_has_suffix(url, ".tgz")) {
            return ARCHIVE_TARGZ;
        }
        return ARCHIVE_GZIP;
    }
    return ARCHIVE_NONE;
}

/* A dynamic array of owned path strings. */
typedef struct path_list {
    char **paths;
    size_t count;
    size_t cap;
} path_list;

static void path_list_init(path_list *list)
{
    list->paths = NULL;
    list->count = 0;
    list->cap = 0;
}

static void path_list_free(path_list *list)
{
    for (size_t i = 0; i < list->count; i++) {
        free(list->paths[i]);
    }
    free(list->paths);
    path_list_init(list);
}

/* Takes ownership of `path` on success. */
static bool path_list_append(path_list *list, char *path)
{
    if (list->count == list->cap) {
        size_t new_cap = (list->cap == 0) ? 16 : list->cap * 2;
        char **grown = realloc(list->paths, new_cap * sizeof(*grown));
        if (grown == NULL) {
            return false;
        }
        list->paths = grown;
        list->cap = new_cap;
    }
    list->paths[list->count++] = path;
    return true;
}

static nhp_err collect_regular_files(const char *dir, path_list *out)
{
    DIR *dp = opendir(dir);
    if (dp == NULL) {
        err_set("could not open %s: %s", dir, strerror(errno));
        return NHP_EIO;
    }

    nhp_err err = NHP_OK;
    const struct dirent *entry;
    while (err == NHP_OK && (entry = readdir(dp)) != NULL) {
        if (str_eq(entry->d_name, ".") || str_eq(entry->d_name, "..")) {
            continue;
        }
        char *child = path_join(dir, entry->d_name);
        if (child == NULL) {
            err = NHP_ENOMEM;
            break;
        }
        struct stat st;
        if (stat(child, &st) != 0) {
            free(child);
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            err = collect_regular_files(child, out);
            free(child);
        } else if (S_ISREG(st.st_mode)) {
            if (!path_list_append(out, child)) {
                err = NHP_ENOMEM;
                free(child);
            }
        } else {
            free(child);
        }
    }
    (void)closedir(dp);
    return err;
}

/* Extracts `archive_path` (of the given kind) into `dest_dir` (already
 * created) using the external tar/gunzip/unzip tools. */
static nhp_err run_extractor(archive_kind kind, const char *archive_path, const char *dest_dir)
{
    proc_result result = {0};
    nhp_err err;

    if (kind == ARCHIVE_TARGZ) {
        const char *argv[] = {"tar", "xzf", archive_path, "-C", dest_dir, NULL};
        err = proc_run(argv, NULL, NULL, &result);
    } else if (kind == ARCHIVE_ZIP) {
        const char *argv[] = {"unzip", "-o", "-q", archive_path, "-d", dest_dir, NULL};
        err = proc_run(argv, NULL, NULL, &result);
    } else {
        char *dest_file = path_join(dest_dir, "extracted");
        if (dest_file == NULL) {
            return NHP_ENOMEM;
        }
        const char *argv[] = {"gunzip", "-c", archive_path, NULL};
        err = proc_run(argv, NULL, NULL, &result);
        if (err == NHP_OK && !result.signaled && result.exit_code == 0) {
            err = fs_write_atomic(dest_file, result.out_data, result.out_len);
        }
        free(dest_file);
    }

    if (err == NHP_OK && (result.signaled || result.exit_code != 0)) {
        err_set("could not extract %s: %s exited with %s", archive_path,
                (kind == ARCHIVE_ZIP)     ? "unzip"
                : (kind == ARCHIVE_TARGZ) ? "tar"
                                          : "gunzip",
                result.signaled ? "a signal" : "a nonzero status");
        err = NHP_EIO;
    }
    proc_result_free(&result);
    return err;
}

/* Extracts a single diff out of `data`, picking the first found
 * (alphabetically) if there's more than one -- letting the user choose
 * (docs/features.md 7.4 step 5) doesn't exist yet in command-line mode. */
static nhp_err extract_one_diff(archive_kind kind, const char *data, size_t len, char **out_diff,
                                size_t *out_len)
{
    char *archive_path = NULL;
    nhp_err err = fs_write_temp_file("nhpm-dl", data, len, &archive_path);
    if (err != NHP_OK) {
        return err;
    }

    char *dest_dir = fs_make_temp_dir("nhpm-extract");
    if (dest_dir == NULL) {
        free(archive_path);
        return NHP_ENOMEM;
    }

    err = run_extractor(kind, archive_path, dest_dir);
    (void)unlink(archive_path);
    free(archive_path);

    path_list files;
    path_list_init(&files);
    if (err == NHP_OK) {
        err = collect_regular_files(dest_dir, &files);
    }

    if (err == NHP_OK) {
        const char *chosen_path = NULL;
        qsort(files.paths, files.count, sizeof(*files.paths), compare_strings);
        for (size_t i = 0; i < files.count && chosen_path == NULL; i++) {
            char *contents = NULL;
            size_t contents_len = 0;
            if (fs_read_file(files.paths[i], &contents, &contents_len) == NHP_OK) {
                if (lib_looks_like_diff(contents, contents_len)) {
                    chosen_path = files.paths[i];
                    *out_diff = contents;
                    *out_len = contents_len;
                } else {
                    free(contents);
                }
            }
        }
        if (chosen_path == NULL) {
            err_set("no diff found inside the archive");
            err = NHP_EPARSE;
        }
    }

    path_list_free(&files);
    (void)fs_remove_tree(dest_dir);
    free(dest_dir);
    return err;
}

/* ---- download / import ---- */

static bool version_dir_exists(const char *patch_dir, const char *version)
{
    char *safe_version = sanitize_component(version);
    if (safe_version == NULL) {
        return false;
    }
    char *version_dir = path_join(patch_dir, safe_version);
    free(safe_version);
    if (version_dir == NULL) {
        return false;
    }
    bool exists = fs_exists(version_dir);
    free(version_dir);
    return exists;
}

static char *wayback_url(const char *original_url)
{
    buf b;
    buf_init(&b);
    buf_append_str(&b, "https://web.archive.org/web/2id_/");
    buf_append_str(&b, original_url);
    return buf_take(&b);
}

/* Fetches `url`'s bytes, falling back to the Wayback Machine if the direct
 * fetch fails (a network error, or an HTTP error status) and
 * app->wayback_fallback is set (docs/features.md section 7.4, step 3). */
static nhp_err fetch_patch_bytes(const char *url, const cfg_app *app, net_resp *out,
                                 bool *out_via_wayback)
{
    *out_via_wayback = false;
    nhp_err err = net_get(url, out, NULL, NULL);
    if (err == NHP_OK && out->http_code < 400) {
        return NHP_OK;
    }

    long direct_http_code = out->http_code;
    net_resp_free(out);

    if (!app->wayback_fallback) {
        if (err == NHP_OK) {
            err_set("could not download %s: HTTP %ld", url, direct_http_code);
            return NHP_ENET;
        }
        return err;
    }

    char *fallback_url = wayback_url(url);
    if (fallback_url == NULL) {
        return NHP_ENOMEM;
    }
    err = net_get(fallback_url, out, NULL, NULL);
    free(fallback_url);
    if (err == NHP_OK && out->http_code < 400) {
        *out_via_wayback = true;
        return NHP_OK;
    }

    if (err == NHP_OK) {
        long wayback_http_code = out->http_code;
        net_resp_free(out);
        err_set("could not download %s (HTTP %ld) or via the Wayback Machine (HTTP %ld)", url,
                direct_http_code, wayback_http_code);
        return NHP_ENET;
    }
    net_resp_free(out);
    return err;
}

/* Writes a new version's patch.diff + version.meta under patch_dir,
 * computing sha256/files_touched from `normalized` (already CRLF-fixed).
 * Shared by lib_download() and lib_import_local(). */
static nhp_err write_version_files(const char *patch_dir, const char *version, const char *url,
                                   bool via_wayback, const char *normalized, size_t normalized_len)
{
    char *safe_version = sanitize_component(version);
    char *version_dir = (safe_version != NULL) ? path_join(patch_dir, safe_version) : NULL;
    free(safe_version);
    if (version_dir == NULL) {
        return NHP_ENOMEM;
    }

    nhp_err err = fs_mkdir_p(version_dir, 0700);
    char *diff_path = NULL;
    if (err == NHP_OK) {
        diff_path = path_join(version_dir, "patch.diff");
        if (diff_path == NULL) {
            err = NHP_ENOMEM;
        }
    }
    if (err == NHP_OK) {
        err = fs_write_atomic(diff_path, normalized, normalized_len);
    }

    if (err == NHP_OK) {
        char sha_hex[65];
        sha256_hex(normalized, normalized_len, sha_hex);
        char *files_touched = extract_files_touched(normalized, normalized_len);

        time_t now = time(NULL);
        struct tm tm_utc;
        (void)gmtime_r(&now, &tm_utc);
        char stamp[32];
        (void)strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);

        lib_version v = {0};
        v.version = str_dup(version);
        v.downloaded = str_dup(stamp);
        v.url = str_dup(url);
        v.via_wayback = via_wayback;
        v.sha256 = str_dup(sha_hex);
        v.files_touched = files_touched;
        if (v.version == NULL || v.downloaded == NULL || v.url == NULL || v.sha256 == NULL ||
            v.files_touched == NULL) {
            err = NHP_ENOMEM;
        } else {
            err = save_version_meta(version_dir, &v);
        }
        lib_version_free_contents(&v);
    }

    free(diff_path);
    free(version_dir);
    return err;
}

/* Writes patch.meta under patch_dir. Shared by lib_download() and
 * lib_import_local(). */
static nhp_err write_patch_meta_file(const char *patch_dir, const char *id, const char *name,
                                     const char *author, const char *target,
                                     const char *source_page)
{
    lib_patch meta = {0};
    meta.id = str_dup(id);
    meta.name = str_dup(name);
    meta.author = str_dup(author);
    meta.target = str_dup(target);
    meta.source_page = str_dup(source_page);

    nhp_err err;
    if (meta.id == NULL || meta.name == NULL || meta.author == NULL || meta.target == NULL ||
        meta.source_page == NULL) {
        err = NHP_ENOMEM;
    } else {
        err = save_patch_meta(patch_dir, &meta);
    }
    lib_patch_free(&meta);
    return err;
}

/* Fetches the patch bytes from `download_url` (falling back to the Wayback
 * Machine), extracts a single diff if it's an archive, validates it, and
 * normalizes line endings. Factored out of lib_download() for size. */
static nhp_err obtain_normalized_diff(const char *id, const char *download_url, const cfg_app *app,
                                      bool *out_via_wayback, char **out_normalized, size_t *out_len)
{
    net_resp resp = {0};
    char *diff_text = NULL;
    size_t diff_len = 0;

    nhp_err err = fetch_patch_bytes(download_url, app, &resp, out_via_wayback);
    if (err != NHP_OK) {
        return err;
    }

    long mb_limit = (app->max_download_mb > 0) ? app->max_download_mb : 5;
    size_t max_bytes = (size_t)mb_limit * 1024 * 1024;
    if (resp.len > max_bytes) {
        err_set("the download for %s is %zu bytes, over the configured %ld MB limit", id, resp.len,
                mb_limit);
        net_resp_free(&resp);
        return NHP_EIO;
    }

    archive_kind kind = detect_archive_kind(resp.data, resp.len, download_url);
    if (kind != ARCHIVE_NONE) {
        err = extract_one_diff(kind, resp.data, resp.len, &diff_text, &diff_len);
        net_resp_free(&resp);
        if (err != NHP_OK) {
            return err;
        }
    } else {
        diff_text = resp.data;
        diff_len = resp.len;
        resp.data = NULL; /* ownership moved to diff_text */
        net_resp_free(&resp);
    }

    if (!lib_looks_like_diff(diff_text, diff_len)) {
        err_set("the download for %s doesn't look like a patch", id);
        free(diff_text);
        return NHP_EPARSE;
    }

    *out_normalized = normalize_line_endings(diff_text, diff_len, out_len);
    free(diff_text);
    return (*out_normalized != NULL) ? NHP_OK : NHP_ENOMEM;
}

nhp_err lib_download(const char *id, const cfg_app *app, char **out_version)
{
    *out_version = NULL;

    site_detail detail = {0};
    char *name_part = NULL;
    char *version_part = NULL;
    char *patch_dir = NULL;
    bool via_wayback = false;
    char *normalized = NULL;
    size_t normalized_len = 0;

    nhp_err err = lib_fetch_detail(id, &detail);
    if (err != NHP_OK) {
        goto cleanup;
    }

    err = site_split_name_version(detail.name, &name_part, &version_part);
    if (err != NHP_OK) {
        goto cleanup;
    }

    {
        char *folder_name = patch_folder_name(id, name_part);
        char *lib_dir = library_dir();
        if (folder_name == NULL || lib_dir == NULL) {
            free(folder_name);
            free(lib_dir);
            err = NHP_ENOMEM;
            goto cleanup;
        }
        patch_dir = path_join(lib_dir, folder_name);
        free(lib_dir);
        free(folder_name);
    }
    if (patch_dir == NULL) {
        err = NHP_ENOMEM;
        goto cleanup;
    }

    if (version_dir_exists(patch_dir, version_part)) {
        err_set("%s version %s is already in the library", id, version_part);
        err = NHP_EEXIST;
        goto cleanup;
    }

    err = obtain_normalized_diff(id, detail.download_url, app, &via_wayback, &normalized,
                                 &normalized_len);
    if (err != NHP_OK) {
        goto cleanup;
    }

    err = write_version_files(patch_dir, version_part, detail.download_url, via_wayback, normalized,
                              normalized_len);
    if (err != NHP_OK) {
        goto cleanup;
    }

    {
        char *detail_url = site_detail_url(id);
        err = write_patch_meta_file(patch_dir, id, name_part, detail.author, detail.target,
                                    (detail_url != NULL) ? detail_url : "");
        free(detail_url);
        if (err != NHP_OK) {
            goto cleanup;
        }
    }

    *out_version = str_dup(version_part);
    if (*out_version == NULL) {
        err = NHP_ENOMEM;
    }

cleanup:
    free(normalized);
    free(patch_dir);
    free(name_part);
    free(version_part);
    site_detail_free(&detail);
    return err;
}

/* "My Local Patch" -> "local-my-local-patch" (docs/features.md section 8:
 * "IDs start with local-"). */
static char *make_local_id(const char *name)
{
    char *slug = make_slug(name);
    if (slug == NULL) {
        return NULL;
    }
    buf b;
    buf_init(&b);
    if (!buf_append_fmt(&b, "%s%s", LOCAL_ID_PREFIX, slug)) {
        free(slug);
        buf_free(&b);
        return NULL;
    }
    free(slug);
    return buf_take(&b);
}

nhp_err lib_import_local(const char *path, const char *name, const char *author, const char *target,
                         const char *version, char **out_id)
{
    *out_id = NULL;

    char *diff_text = NULL;
    size_t diff_len = 0;
    char *normalized = NULL;
    size_t normalized_len = 0;
    char *id = NULL;
    char *patch_dir = NULL;

    nhp_err err = fs_read_file(path, &diff_text, &diff_len);
    if (err != NHP_OK) {
        goto cleanup;
    }

    if (!lib_looks_like_diff(diff_text, diff_len)) {
        err_set("%s doesn't look like a patch", path);
        err = NHP_EPARSE;
        goto cleanup;
    }

    normalized = normalize_line_endings(diff_text, diff_len, &normalized_len);
    if (normalized == NULL) {
        err = NHP_ENOMEM;
        goto cleanup;
    }

    id = make_local_id(name);
    if (id == NULL) {
        err = NHP_ENOMEM;
        goto cleanup;
    }

    {
        char *lib_dir = library_dir();
        patch_dir = (lib_dir != NULL) ? path_join(lib_dir, id) : NULL;
        free(lib_dir);
    }
    if (patch_dir == NULL) {
        err = NHP_ENOMEM;
        goto cleanup;
    }

    {
        const char *actual_version = (version != NULL && version[0] != '\0') ? version : "1.0";
        if (version_dir_exists(patch_dir, actual_version)) {
            err_set("%s version %s is already in the library", id, actual_version);
            err = NHP_EEXIST;
            goto cleanup;
        }

        err = write_version_files(patch_dir, actual_version, "", false, normalized, normalized_len);
        if (err != NHP_OK) {
            goto cleanup;
        }
    }

    err = write_patch_meta_file(patch_dir, id, name, author, (target != NULL) ? target : "", "");
    if (err != NHP_OK) {
        goto cleanup;
    }

    *out_id = id;
    id = NULL; /* ownership transferred to *out_id */

cleanup:
    free(diff_text);
    free(normalized);
    free(patch_dir);
    free(id);
    return err;
}

/* "1.0" -> "1.1", "2.3" -> "2.4"; NULL if `version` isn't a bare
 * "MAJOR.MINOR" number, since anything else needs an explicit version
 * from the caller. */
static char *bump_minor_version(const char *version)
{
    const char *dot = strchr(version, '.');
    if (dot == NULL || dot == version || *(dot + 1) == '\0') {
        return NULL;
    }
    for (const char *p = version; p < dot; p++) {
        if (!isdigit((unsigned char)*p)) {
            return NULL;
        }
    }
    for (const char *p = dot + 1; *p != '\0'; p++) {
        if (!isdigit((unsigned char)*p)) {
            return NULL;
        }
    }
    long major = strtol(version, NULL, 10);
    long minor = strtol(dot + 1, NULL, 10);
    buf b;
    buf_init(&b);
    if (!buf_append_fmt(&b, "%ld.%ld", major, minor + 1)) {
        buf_free(&b);
        return NULL;
    }
    return buf_take(&b);
}

nhp_err lib_add_version(const char *id, const char *name, const char *author, const char *target,
                        const char *version, const char *path, char **out_version)
{
    *out_version = NULL;

    char *patch_dir = find_patch_dir(id);
    bool is_new_patch = patch_dir == NULL;
    if (is_new_patch) {
        if (!is_local_id(id)) {
            err_set("%s is not in the library", id);
            return NHP_ENOENT;
        }
        char *lib_dir = library_dir();
        patch_dir = (lib_dir != NULL) ? path_join(lib_dir, id) : NULL;
        free(lib_dir);
        if (patch_dir == NULL) {
            return NHP_ENOMEM;
        }
    }

    char *guessed = NULL;
    const char *actual_version = (version != NULL && version[0] != '\0') ? version : NULL;
    if (actual_version == NULL) {
        if (is_new_patch) {
            actual_version = "1.0";
        } else {
            lib_patch existing;
            if (lib_find(id, &existing) == NHP_OK) {
                if (existing.version_count > 0) {
                    guessed =
                        bump_minor_version(existing.versions[existing.version_count - 1].version);
                }
                lib_patch_free(&existing);
            }
            if (guessed == NULL) {
                err_set("could not guess the next version for %s -- specify one explicitly", id);
                free(patch_dir);
                return NHP_EINVAL;
            }
            actual_version = guessed;
        }
    }

    if (version_dir_exists(patch_dir, actual_version)) {
        err_set("%s version %s is already in the library", id, actual_version);
        free(guessed);
        free(patch_dir);
        return NHP_EEXIST;
    }

    char *diff_text = NULL;
    size_t diff_len = 0;
    nhp_err err = fs_read_file(path, &diff_text, &diff_len);
    if (err != NHP_OK) {
        free(guessed);
        free(patch_dir);
        return err;
    }

    if (!lib_looks_like_diff(diff_text, diff_len)) {
        err_set("%s doesn't look like a patch", path);
        free(diff_text);
        free(guessed);
        free(patch_dir);
        return NHP_EPARSE;
    }

    size_t normalized_len = 0;
    char *normalized = normalize_line_endings(diff_text, diff_len, &normalized_len);
    free(diff_text);
    if (normalized == NULL) {
        free(guessed);
        free(patch_dir);
        return NHP_ENOMEM;
    }

    err = write_version_files(patch_dir, actual_version, "", false, normalized, normalized_len);
    free(normalized);
    if (err == NHP_OK && is_new_patch) {
        err = write_patch_meta_file(patch_dir, id, (name != NULL) ? name : "",
                                    (author != NULL) ? author : "", (target != NULL) ? target : "",
                                    "");
    }
    free(patch_dir);
    if (err != NHP_OK) {
        free(guessed);
        return err;
    }

    *out_version = str_dup(actual_version);
    free(guessed);
    return (*out_version != NULL) ? NHP_OK : NHP_ENOMEM;
}

char *lib_make_local_id(const char *name)
{
    return make_local_id(name);
}

nhp_err lib_fork_as_local(const char *original_id, const char *original_version, const char *name,
                          const char *author, const char *target, const char *path, char **out_id,
                          char **out_version)
{
    *out_id = NULL;
    *out_version = NULL;

    char *slug = make_slug(name);
    if (slug == NULL) {
        return NHP_ENOMEM;
    }
    buf id_buf;
    buf_init(&id_buf);
    bool id_ok = buf_append_fmt(&id_buf, "%s%s-%s-my", LOCAL_ID_PREFIX, original_id, slug);
    free(slug);
    if (!id_ok) {
        buf_free(&id_buf);
        return NHP_ENOMEM;
    }
    char *id = buf_take(&id_buf);
    if (id == NULL) {
        return NHP_ENOMEM;
    }

    char *lib_dir = library_dir();
    char *patch_dir = (lib_dir != NULL) ? path_join(lib_dir, id) : NULL;
    free(lib_dir);
    if (patch_dir == NULL) {
        free(id);
        return NHP_ENOMEM;
    }
    bool is_new_patch = !fs_exists(patch_dir);

    char *version = NULL;
    for (int n = 1; n <= 1000 && version == NULL; n++) {
        buf version_buf;
        buf_init(&version_buf);
        bool version_ok = buf_append_fmt(&version_buf, "%s-%d", original_version, n);
        char *candidate = version_ok ? buf_take(&version_buf) : NULL;
        if (!version_ok) {
            buf_free(&version_buf);
        }
        if (candidate == NULL) {
            free(patch_dir);
            free(id);
            return NHP_ENOMEM;
        }
        if (!version_dir_exists(patch_dir, candidate)) {
            version = candidate;
        } else {
            free(candidate);
        }
    }
    if (version == NULL) {
        err_set("could not find a free local version for %s", id);
        free(patch_dir);
        free(id);
        return NHP_EEXIST;
    }

    char *diff_text = NULL;
    size_t diff_len = 0;
    nhp_err err = fs_read_file(path, &diff_text, &diff_len);
    if (err != NHP_OK) {
        free(version);
        free(patch_dir);
        free(id);
        return err;
    }
    if (!lib_looks_like_diff(diff_text, diff_len)) {
        err_set("%s doesn't look like a patch", path);
        free(diff_text);
        free(version);
        free(patch_dir);
        free(id);
        return NHP_EPARSE;
    }
    size_t normalized_len = 0;
    char *normalized = normalize_line_endings(diff_text, diff_len, &normalized_len);
    free(diff_text);
    if (normalized == NULL) {
        free(version);
        free(patch_dir);
        free(id);
        return NHP_ENOMEM;
    }

    err = write_version_files(patch_dir, version, "", false, normalized, normalized_len);
    free(normalized);
    if (err == NHP_OK && is_new_patch) {
        err =
            write_patch_meta_file(patch_dir, id, name, author, (target != NULL) ? target : "", "");
    }
    free(patch_dir);
    if (err != NHP_OK) {
        free(version);
        free(id);
        return err;
    }

    *out_id = id;
    *out_version = version;
    return NHP_OK;
}

/* ---- delete / verify ---- */

nhp_err lib_delete_version(const char *id, const char *version)
{
    char *patch_dir = find_patch_dir(id);
    if (patch_dir == NULL) {
        err_set("%s is not in the library", id);
        return NHP_ENOENT;
    }

    char *safe_version = sanitize_component(version);
    char *version_dir = (safe_version != NULL) ? path_join(patch_dir, safe_version) : NULL;
    free(safe_version);
    free(patch_dir);
    if (version_dir == NULL) {
        return NHP_ENOMEM;
    }

    if (!fs_exists(version_dir)) {
        err_set("%s version %s is not in the library", id, version);
        free(version_dir);
        return NHP_ENOENT;
    }

    nhp_err err = fs_remove_tree(version_dir);
    free(version_dir);
    return err;
}

nhp_err lib_delete_patch(const char *id)
{
    char *patch_dir = find_patch_dir(id);
    if (patch_dir == NULL) {
        err_set("%s is not in the library", id);
        return NHP_ENOENT;
    }
    nhp_err err = fs_remove_tree(patch_dir);
    free(patch_dir);
    return err;
}

nhp_err lib_version_diff_path(const char *id, const char *version, char **out_path)
{
    *out_path = NULL;

    char *patch_dir = find_patch_dir(id);
    if (patch_dir == NULL) {
        err_set("%s is not in the library", id);
        return NHP_ENOENT;
    }

    char *safe_version = sanitize_component(version);
    char *version_dir = (safe_version != NULL) ? path_join(patch_dir, safe_version) : NULL;
    free(safe_version);
    free(patch_dir);
    if (version_dir == NULL) {
        return NHP_ENOMEM;
    }

    if (!fs_exists(version_dir)) {
        err_set("%s version %s is not in the library", id, version);
        free(version_dir);
        return NHP_ENOENT;
    }

    *out_path = path_join(version_dir, "patch.diff");
    free(version_dir);
    return (*out_path != NULL) ? NHP_OK : NHP_ENOMEM;
}

nhp_err lib_verify_version(const char *id, const char *version, bool *out_ok)
{
    *out_ok = false;

    char *patch_dir = find_patch_dir(id);
    if (patch_dir == NULL) {
        err_set("%s is not in the library", id);
        return NHP_ENOENT;
    }

    char *safe_version = sanitize_component(version);
    char *version_dir = (safe_version != NULL) ? path_join(patch_dir, safe_version) : NULL;
    free(safe_version);
    free(patch_dir);
    if (version_dir == NULL) {
        return NHP_ENOMEM;
    }

    lib_version meta = {0};
    nhp_err err = load_version_meta(version_dir, &meta);
    if (err != NHP_OK) {
        free(version_dir);
        lib_version_free_contents(&meta);
        return err;
    }

    char *diff_path = path_join(version_dir, "patch.diff");
    free(version_dir);
    if (diff_path == NULL) {
        lib_version_free_contents(&meta);
        return NHP_ENOMEM;
    }

    char *data = NULL;
    size_t len = 0;
    err = fs_read_file(diff_path, &data, &len);
    free(diff_path);
    if (err != NHP_OK) {
        lib_version_free_contents(&meta);
        return err;
    }

    char sha_hex[65];
    sha256_hex(data, len, sha_hex);
    free(data);

    *out_ok = str_eq(sha_hex, meta.sha256);
    lib_version_free_contents(&meta);
    return NHP_OK;
}
