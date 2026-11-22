/* sources.c - base sources: import from folder/tarball/git, verify, remove.
 * SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "sources.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "config.h"
#include "proc.h"

#define GIT_REPO_URL "https://github.com/NetHack/NetHack"

static char *sources_dir(void)
{
    char *data_dir = cfg_xdg_data_dir();
    if (data_dir == NULL) {
        return NULL;
    }
    char *dir = path_join(data_dir, "sources");
    free(data_dir);
    return dir;
}

static char *source_dir_for(const char *version)
{
    char *dir = sources_dir();
    if (dir == NULL) {
        return NULL;
    }
    char *result = path_join(dir, version);
    free(dir);
    return result;
}

char *bsrc_tree_path(const char *version)
{
    char *dir = source_dir_for(version);
    if (dir == NULL) {
        return NULL;
    }
    char *tree = path_join(dir, "tree");
    free(dir);
    return tree;
}

/* ---- source.meta ---- */

static nhp_err load_source_meta(const char *source_dir, bsrc_source *out)
{
    char *path = path_join(source_dir, "source.meta");
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
    out->origin = str_dup(cfg_get(&doc, NULL, "origin", ""));
    out->added = str_dup(cfg_get(&doc, NULL, "added", ""));
    out->sha256 = str_dup(cfg_get(&doc, NULL, "sha256", ""));
    out->is_variant = cfg_get_bool(&doc, NULL, "variant", false);
    cfg_doc_free(&doc);

    if (out->version == NULL || out->origin == NULL || out->added == NULL || out->sha256 == NULL) {
        return NHP_ENOMEM;
    }
    return NHP_OK;
}

static nhp_err save_source_meta(const char *source_dir, const bsrc_source *s)
{
    nhp_err err = fs_mkdir_p(source_dir, 0700);
    if (err != NHP_OK) {
        return err;
    }
    char *path = path_join(source_dir, "source.meta");
    if (path == NULL) {
        return NHP_ENOMEM;
    }

    cfg_doc doc;
    cfg_doc_init(&doc);
    err = cfg_set(&doc, NULL, "version", s->version);
    if (err == NHP_OK) {
        err = cfg_set(&doc, NULL, "origin", s->origin);
    }
    if (err == NHP_OK) {
        err = cfg_set(&doc, NULL, "added", s->added);
    }
    if (err == NHP_OK) {
        err = cfg_set(&doc, NULL, "sha256", s->sha256);
    }
    if (err == NHP_OK) {
        err = cfg_set(&doc, NULL, "variant", s->is_variant ? "yes" : "no");
    }
    if (err == NHP_OK) {
        err = cfg_doc_save(path, &doc);
    }
    cfg_doc_free(&doc);
    free(path);
    return err;
}

void bsrc_source_free(bsrc_source *s)
{
    free(s->version);
    free(s->origin);
    free(s->added);
    free(s->sha256);
    memset(s, 0, sizeof(*s));
}

void bsrc_list_free(bsrc_source *sources, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        bsrc_source_free(&sources[i]);
    }
    free(sources);
}

nhp_err bsrc_find(const char *version, bsrc_source *out)
{
    memset(out, 0, sizeof(*out));
    char *dir = source_dir_for(version);
    if (dir == NULL) {
        return NHP_ENOMEM;
    }
    if (!fs_exists(dir)) {
        free(dir);
        err_set("no base source for %s", version);
        return NHP_ENOENT;
    }
    nhp_err err = load_source_meta(dir, out);
    free(dir);
    if (err != NHP_OK) {
        bsrc_source_free(out);
    }
    return err;
}

nhp_err bsrc_list(bsrc_source **out_sources, size_t *out_count)
{
    *out_sources = NULL;
    *out_count = 0;

    char *dir = sources_dir();
    if (dir == NULL) {
        return NHP_ENOMEM;
    }

    DIR *dp = opendir(dir);
    if (dp == NULL) {
        if (errno == ENOENT) {
            free(dir);
            return NHP_OK; /* no sources yet: an empty list */
        }
        err_set("could not open %s: %s", dir, strerror(errno));
        free(dir);
        return NHP_EIO;
    }

    bsrc_source *sources = NULL;
    size_t count = 0;
    size_t cap = 0;
    const struct dirent *entry;
    nhp_err err = NHP_OK;

    while (err == NHP_OK && (entry = readdir(dp)) != NULL) {
        if (str_eq(entry->d_name, ".") || str_eq(entry->d_name, "..")) {
            continue;
        }
        char *entry_dir = path_join(dir, entry->d_name);
        if (entry_dir == NULL) {
            err = NHP_ENOMEM;
            break;
        }
        struct stat st;
        bool is_source_dir = stat(entry_dir, &st) == 0 && S_ISDIR(st.st_mode);
        if (!is_source_dir) {
            free(entry_dir);
            continue;
        }

        if (count == cap) {
            size_t new_cap = (cap == 0) ? 8 : cap * 2;
            bsrc_source *grown = realloc(sources, new_cap * sizeof(*grown));
            if (grown == NULL) {
                err = NHP_ENOMEM;
                free(entry_dir);
                break;
            }
            sources = grown;
            cap = new_cap;
        }

        memset(&sources[count], 0, sizeof(sources[count]));
        err = load_source_meta(entry_dir, &sources[count]);
        free(entry_dir);
        if (err != NHP_OK) {
            bsrc_source_free(&sources[count]);
            break;
        }
        count++;
    }
    (void)closedir(dp);
    free(dir);

    if (err != NHP_OK) {
        bsrc_list_free(sources, count);
        return err;
    }
    *out_sources = sources;
    *out_count = count;
    return NHP_OK;
}

/* ---- adding ---- */

static char *now_iso8601(void)
{
    time_t now = time(NULL);
    struct tm tm_utc;
    (void)gmtime_r(&now, &tm_utc);
    char stamp[32];
    (void)strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return str_dup(stamp);
}

/* Reads the version out of `folder` (patchlevel.h, falling back to
 * `version_hint`), copies it into sources/<version>/tree, chmods the copy
 * read-only, and writes source.meta with `origin`/`tarball_sha256`. */
static nhp_err import_folder(const char *folder, const char *version_hint, const char *origin,
                             const char *tarball_sha256, char **out_version)
{
    ver_version v;
    nhp_err err = ver_read_patchlevel(folder, &v);
    if (err != NHP_OK) {
        if (version_hint == NULL || version_hint[0] == '\0') {
            err_set("could not read the version from %s/include/patchlevel.h, and no version "
                    "was given",
                    folder);
            return NHP_EPARSE;
        }
        err = ver_parse(version_hint, &v);
        if (err != NHP_OK) {
            return err;
        }
    }

    char *version_str = ver_format(&v);
    bool is_variant = v.is_variant;
    ver_free(&v);
    if (version_str == NULL) {
        return NHP_ENOMEM;
    }

    char *source_dir = source_dir_for(version_str);
    char *tree_dir = (source_dir != NULL) ? path_join(source_dir, "tree") : NULL;
    if (source_dir == NULL || tree_dir == NULL) {
        free(source_dir);
        free(tree_dir);
        free(version_str);
        return NHP_ENOMEM;
    }

    if (fs_exists(tree_dir)) {
        err_set("a base source for %s already exists", version_str);
        free(source_dir);
        free(tree_dir);
        /* Handed back rather than freed, so a caller offering "replace it?"
         * knows which version conflicted without parsing the error text. */
        *out_version = version_str;
        return NHP_EEXIST;
    }

    err = fs_copy_tree(folder, tree_dir);
    if (err == NHP_OK) {
        /* "kept read-only" (docs/features.md section 9.2); best-effort --
         * a failure here doesn't invalidate the import. */
        const char *argv[] = {"chmod", "-R", "a-w", tree_dir, NULL};
        proc_result result = {0};
        (void)proc_run(argv, NULL, NULL, &result);
        proc_result_free(&result);

        char *added = now_iso8601();
        bsrc_source meta = {0};
        meta.version = version_str;
        meta.origin = str_dup(origin);
        meta.added = added;
        meta.sha256 = str_dup((tarball_sha256 != NULL) ? tarball_sha256 : "");
        meta.is_variant = is_variant;
        if (meta.origin == NULL || meta.added == NULL || meta.sha256 == NULL) {
            err = NHP_ENOMEM;
        } else {
            err = save_source_meta(source_dir, &meta);
        }
        free(meta.origin);
        free(meta.added);
        free(meta.sha256);
        /* meta.version is version_str, freed via *out_version below */
    }

    free(tree_dir);
    free(source_dir);

    if (err == NHP_OK) {
        *out_version = version_str;
    } else {
        free(version_str);
    }
    return err;
}

nhp_err bsrc_add_from_folder(const char *folder, const char *version_hint, char **out_version)
{
    *out_version = NULL;
    buf origin_buf;
    buf_init(&origin_buf);
    buf_append_fmt(&origin_buf, "folder:%s", folder);
    char *origin = buf_take(&origin_buf);
    if (origin == NULL) {
        return NHP_ENOMEM;
    }
    nhp_err err = import_folder(folder, version_hint, origin, NULL, out_version);
    free(origin);
    return err;
}

/* True if every entry in `tarball_path` is safe to extract: no absolute
 * paths, no ".." components (docs/security.md's "archive checks"). Uses
 * `tar tf`, which auto-detects compression (.tar.gz, .tgz, .tar.bz2, ...). */
static nhp_err check_tar_entries_safe(const char *tarball_path)
{
    const char *argv[] = {"tar", "tf", tarball_path, NULL};
    proc_result result = {0};
    nhp_err err = proc_run(argv, NULL, NULL, &result);
    if (err != NHP_OK) {
        proc_result_free(&result);
        return err;
    }
    if (result.signaled || result.exit_code != 0) {
        err_set("could not list the contents of %s", tarball_path);
        proc_result_free(&result);
        return NHP_EIO;
    }

    nhp_err check_err = NHP_OK;
    const char *p = result.out_data;
    while (*p != '\0' && check_err == NHP_OK) {
        const char *line_start = p;
        while (*p != '\0' && *p != '\n') {
            p++;
        }
        size_t line_len = (size_t)(p - line_start);
        if (*p == '\n') {
            p++;
        }
        if (line_len == 0) {
            continue;
        }
        char *entry = str_ndup(line_start, line_len);
        if (entry == NULL) {
            check_err = NHP_ENOMEM;
            break;
        }
        if (!path_is_safe(entry)) {
            err_set("the archive contains an unsafe path: %s", entry);
            check_err = NHP_EPARSE;
        }
        free(entry);
    }
    proc_result_free(&result);
    return check_err;
}

nhp_err bsrc_extract_tarball_checked(const char *tarball_path, const char *dest_dir)
{
    nhp_err err = check_tar_entries_safe(tarball_path);
    if (err != NHP_OK) {
        return err;
    }

    const char *argv[] = {"tar", "xf", tarball_path, "-C", dest_dir, NULL};
    proc_result result = {0};
    err = proc_run(argv, NULL, NULL, &result);
    if (err == NHP_OK && (result.signaled || result.exit_code != 0)) {
        err_set("could not extract %s", tarball_path);
        err = NHP_EIO;
    }
    proc_result_free(&result);
    return err;
}

/* If extracting `tarball_path` produced exactly one top-level directory
 * (the common case for release tarballs, e.g. "nethack-367-src.tgz" ->
 * "nh367/..."), returns its path; otherwise returns `dest_dir` itself. */
static char *find_single_top_level_dir(const char *dest_dir)
{
    DIR *dp = opendir(dest_dir);
    if (dp == NULL) {
        return str_dup(dest_dir);
    }

    char *only_dir = NULL;
    bool more_than_one = false;
    const struct dirent *entry;
    while ((entry = readdir(dp)) != NULL) {
        if (str_eq(entry->d_name, ".") || str_eq(entry->d_name, "..")) {
            continue;
        }
        char *child = path_join(dest_dir, entry->d_name);
        struct stat st;
        bool is_dir = child != NULL && stat(child, &st) == 0 && S_ISDIR(st.st_mode);
        if (!is_dir || only_dir != NULL) {
            more_than_one = true;
            free(child);
            continue;
        }
        only_dir = child;
    }
    (void)closedir(dp);

    if (only_dir != NULL && !more_than_one) {
        return only_dir;
    }
    free(only_dir);
    return str_dup(dest_dir);
}

nhp_err bsrc_add_from_tarball(const char *tarball_path, const char *version_hint,
                              char **out_version)
{
    *out_version = NULL;

    char *tarball_data = NULL;
    size_t tarball_len = 0;
    nhp_err err = fs_read_file(tarball_path, &tarball_data, &tarball_len);
    if (err != NHP_OK) {
        return err;
    }
    char tarball_sha[65];
    sha256_hex(tarball_data, tarball_len, tarball_sha);
    free(tarball_data);

    char *dest_dir = fs_make_temp_dir("nhpm-source");
    if (dest_dir == NULL) {
        return NHP_ENOMEM;
    }

    err = bsrc_extract_tarball_checked(tarball_path, dest_dir);
    if (err != NHP_OK) {
        (void)fs_remove_tree(dest_dir);
        free(dest_dir);
        return err;
    }

    char *source_root = find_single_top_level_dir(dest_dir);
    if (source_root == NULL) {
        (void)fs_remove_tree(dest_dir);
        free(dest_dir);
        return NHP_ENOMEM;
    }

    buf origin_buf;
    buf_init(&origin_buf);
    buf_append_fmt(&origin_buf, "tarball:%s", tarball_path);
    char *origin = buf_take(&origin_buf);
    if (origin == NULL) {
        err = NHP_ENOMEM;
    } else {
        err = import_folder(source_root, version_hint, origin, tarball_sha, out_version);
    }

    free(origin);
    free(source_root);
    (void)fs_remove_tree(dest_dir);
    free(dest_dir);
    return err;
}

nhp_err bsrc_add_from_git(const char *tag, char **out_version)
{
    *out_version = NULL;

    char *dest_dir = fs_make_temp_dir("nhpm-source");
    if (dest_dir == NULL) {
        return NHP_ENOMEM;
    }

    const char *argv[] = {"git", "clone",      "--depth", "1", "--branch",
                          tag,   GIT_REPO_URL, dest_dir,  NULL};
    proc_result result = {0};
    nhp_err err = proc_run(argv, NULL, NULL, &result);
    if (err == NHP_OK && (result.signaled || result.exit_code != 0)) {
        err_set("could not clone %s (tag/branch %s): %s", GIT_REPO_URL, tag, result.err_data);
        err = NHP_ENET;
    }
    proc_result_free(&result);
    if (err != NHP_OK) {
        (void)fs_remove_tree(dest_dir);
        free(dest_dir);
        return err;
    }

    /* don't copy git's own metadata into what's supposed to be a clean
     * source snapshot. */
    char *git_dir = path_join(dest_dir, ".git");
    if (git_dir != NULL) {
        (void)fs_remove_tree(git_dir);
        free(git_dir);
    }

    buf origin_buf;
    buf_init(&origin_buf);
    buf_append_fmt(&origin_buf, "git:%s#%s", GIT_REPO_URL, tag);
    char *origin = buf_take(&origin_buf);
    if (origin == NULL) {
        err = NHP_ENOMEM;
    } else {
        err = import_folder(dest_dir, NULL, origin, NULL, out_version);
    }

    free(origin);
    (void)fs_remove_tree(dest_dir);
    free(dest_dir);
    return err;
}

nhp_err bsrc_remove(const char *version)
{
    char *dir = source_dir_for(version);
    if (dir == NULL) {
        return NHP_ENOMEM;
    }
    if (!fs_exists(dir)) {
        err_set("no base source for %s", version);
        free(dir);
        return NHP_ENOENT;
    }

    /* the tree was made read-only on import; allow removal regardless. */
    const char *argv[] = {"chmod", "-R", "u+w", dir, NULL};
    proc_result result = {0};
    (void)proc_run(argv, NULL, NULL, &result);
    proc_result_free(&result);

    nhp_err err = fs_remove_tree(dir);
    free(dir);
    return err;
}

/* ---- known versions ---- */

/* Hand-verified, not guessed: each entry was actually fetched and checked
 * (a real, buildable source tree, not a redirect/404/stub) before being
 * added. Add a row only once verified the same way (see sources.h). */
const bsrc_known_version BSRC_KNOWN_VERSIONS[] = {
    {"3.4.3", BSRC_KNOWN_TARBALL, "https://www.nethack.org/download/3.4.3/nethack-343-src.tgz"},
    {"3.6.7", BSRC_KNOWN_GIT, "NetHack-3.6.7_Released"},
};
const size_t BSRC_KNOWN_VERSION_COUNT =
    sizeof(BSRC_KNOWN_VERSIONS) / sizeof(BSRC_KNOWN_VERSIONS[0]);

/* A NetHack source tarball is a few MB (3.4.3's own is under 4); this
 * leaves generous headroom without being unbounded (docs/security.md:
 * "size limits on downloads"). */
#define KNOWN_TARBALL_MAX_BYTES (100UL * 1024 * 1024)

nhp_err bsrc_add_known(const bsrc_known_version *entry, net_progress_fn progress, void *user_data,
                       char **out_version)
{
    *out_version = NULL;

    if (entry->kind == BSRC_KNOWN_GIT) {
        return bsrc_add_from_git(entry->location, out_version);
    }

    char *dest_dir = fs_make_temp_dir("nhpm-known-source");
    if (dest_dir == NULL) {
        return NHP_ENOMEM;
    }
    char *tarball_path = path_join(dest_dir, "source.tgz");
    if (tarball_path == NULL) {
        (void)fs_remove_tree(dest_dir);
        free(dest_dir);
        return NHP_ENOMEM;
    }

    nhp_err err =
        net_download(entry->location, tarball_path, KNOWN_TARBALL_MAX_BYTES, progress, user_data);
    if (err == NHP_OK) {
        err = bsrc_add_from_tarball(tarball_path, entry->version, out_version);
    }

    (void)fs_remove_tree(dest_dir);
    free(tarball_path);
    free(dest_dir);
    return err;
}
