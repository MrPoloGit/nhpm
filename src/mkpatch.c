/* mkpatch.c - combined patches, capturing changes, patches from a folder
 * or git, ignore rules, header writing, verification.
 * SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "mkpatch.h"

#include "library.h"
#include "proc.h"
#include "quilt.h"
#include "sources.h"
#include "stack.h"
#include "version.h"

#include <ctype.h>
#include <dirent.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

/* ---- 12.1: ignore rules, binary detection, new-file default ticks ---- */

/* nhpm's/Quilt's own bookkeeping, never a real edit -- always excluded,
 * even with "show hidden" (12.5's "h" only reveals build-output guesses
 * below). Trailing "/" excludes a dir; else an fnmatch() glob, any depth. */
static const char *const STRUCTURAL_IGNORE[] = {
    ".pc/", "patches/", "series", ".nhpm/", ".git/",
};

/* docs/features.md 12.1 rule 1: build output nhpm *guesses* to hide by
 * default, true for every NetHack version. A guess can be wrong, so
 * 12.5's "h" can reveal these (tagged hidden, never dropped outright). */
static const char *const GENERAL_IGNORE[] = {
    "*.o", "*.a", "*.obj", "*~", "*.orig", "*.rej",
};

/* docs/features.md 12.1's example gives 3.6 a concrete list; 3.4/3.7 get
 * only the general list. "*Makefile" stands in for the Makefiles
 * sys/unix/setup.sh copies from templates, whose exact paths aren't named. */
static const char *const V36_IGNORE[] = {
    "include/onames.h", "include/pm.h",  "include/date.h", "src/monstr.c",
    "src/vis_tab.c",    "util/makedefs", "util/lev_comp",  "dat/*.lev",
    "dat/data",         "dat/nhdat",     "*Makefile",
};

static bool matches_one(const char *path, const char *pattern)
{
    size_t plen = strlen(pattern);
    if (plen > 0 && pattern[plen - 1] == '/') {
        size_t dir_len = plen - 1;
        return strncmp(path, pattern, dir_len) == 0 &&
               (path[dir_len] == '/' || path[dir_len] == '\0');
    }
    return fnmatch(pattern, path, 0) == 0;
}

static bool matches_any(const char *path, const char *const *patterns, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (matches_one(path, patterns[i])) {
            return true;
        }
    }
    return false;
}

/* Matches `path` against the whitespace-separated globs in `patterns`
 * (e.g. cfg_app.ignore, "*.bak *.tmp"). "" is never a match. */
static bool matches_extra(const char *path, const char *patterns)
{
    if (patterns == NULL || patterns[0] == '\0') {
        return false;
    }
    char *copy = str_dup(patterns);
    if (copy == NULL) {
        return false;
    }
    bool found = false;
    char *saveptr = NULL;
    const char *tok = strtok_r(copy, " \t", &saveptr);
    while (tok != NULL && !found) {
        found = matches_one(path, tok);
        tok = strtok_r(NULL, " \t", &saveptr);
    }
    free(copy);
    return found;
}

bool mkp_is_structural(const char *path)
{
    return matches_any(path, STRUCTURAL_IGNORE, NHP_ARRAY_LEN(STRUCTURAL_IGNORE));
}

bool mkp_is_build_output(const char *path, const char *nethack_version, const char *extra_patterns)
{
    if (matches_any(path, GENERAL_IGNORE, NHP_ARRAY_LEN(GENERAL_IGNORE))) {
        return true;
    }
    if (nethack_version != NULL && strncmp(nethack_version, "3.6", 3) == 0 &&
        matches_any(path, V36_IGNORE, NHP_ARRAY_LEN(V36_IGNORE))) {
        return true;
    }
    return matches_extra(path, extra_patterns);
}

bool mkp_is_ignored(const char *path, const char *nethack_version, const char *extra_patterns)
{
    return mkp_is_structural(path) || mkp_is_build_output(path, nethack_version, extra_patterns);
}

bool mkp_looks_like_source(const char *path)
{
    static const char *const source_ext[] = {".c", ".h", ".des", ".lua"};
    const char *dot = strrchr(path, '.');
    if (dot != NULL) {
        for (size_t i = 0; i < NHP_ARRAY_LEN(source_ext); i++) {
            if (str_eq(dot, source_ext[i])) {
                return true;
            }
        }
        if (str_eq(dot, ".txt") &&
            (strncmp(path, "dat/", 4) == 0 || strncmp(path, "doc/", 4) == 0)) {
            return true;
        }
    }
    return false;
}

bool mkp_looks_binary(const void *data, size_t len)
{
    size_t scan = (len < 8192) ? len : 8192;
    const unsigned char *bytes = data;
    for (size_t i = 0; i < scan; i++) {
        if (bytes[i] == '\0') {
            return true;
        }
    }
    return false;
}

void mkp_file_list_free(mkp_file *files, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        free(files[i].path);
    }
    free(files);
}

/* ---- walking a tree, skipping symlinks and special files (security.md) ---- */

typedef struct path_array {
    char **items;
    size_t count;
    size_t cap;
} path_array;

static bool path_array_push(path_array *arr, const char *path)
{
    if (arr->count == arr->cap) {
        size_t new_cap = (arr->cap == 0) ? 64 : arr->cap * 2;
        char **grown = realloc(arr->items, new_cap * sizeof(*grown));
        if (grown == NULL) {
            return false;
        }
        arr->items = grown;
        arr->cap = new_cap;
    }
    arr->items[arr->count] = str_dup(path);
    if (arr->items[arr->count] == NULL) {
        return false;
    }
    arr->count++;
    return true;
}

static void path_array_free(path_array *arr)
{
    for (size_t i = 0; i < arr->count; i++) {
        free(arr->items[i]);
    }
    free(arr->items);
    arr->items = NULL;
    arr->count = 0;
    arr->cap = 0;
}

static int cmp_str_ptr(const void *a, const void *b)
{
    const char *const *pa = a;
    const char *const *pb = b;
    return strcmp(*pa, *pb);
}

/* Recursively collects regular files under `root` (relative paths, "" at
 * the top), skipping symlinks/specials and pruning mkp_is_structural() dirs.
 * Build-output is walked, not pruned -- mkp_compare_trees() tags it `hidden`. */
static nhp_err walk_tree_into(const char *root, const char *rel_dir, path_array *out)
{
    char *abs_dir = (rel_dir[0] != '\0') ? path_join(root, rel_dir) : str_dup(root);
    if (abs_dir == NULL) {
        return NHP_ENOMEM;
    }
    DIR *dp = opendir(abs_dir);
    free(abs_dir);
    if (dp == NULL) {
        return NHP_OK; /* nothing there -- not an error for our callers */
    }

    nhp_err err = NHP_OK;
    const struct dirent *entry;
    while (err == NHP_OK && (entry = readdir(dp)) != NULL) {
        if (str_eq(entry->d_name, ".") || str_eq(entry->d_name, "..")) {
            continue;
        }
        char *rel =
            (rel_dir[0] != '\0') ? path_join(rel_dir, entry->d_name) : str_dup(entry->d_name);
        if (rel == NULL) {
            err = NHP_ENOMEM;
            break;
        }
        char *abs = path_join(root, rel);
        if (abs == NULL) {
            free(rel);
            err = NHP_ENOMEM;
            break;
        }

        struct stat st;
        if (lstat(abs, &st) != 0) {
            free(rel);
            free(abs);
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            char dir_pattern_check[4096];
            (void)snprintf(dir_pattern_check, sizeof(dir_pattern_check), "%s/", rel);
            if (!mkp_is_structural(dir_pattern_check)) {
                err = walk_tree_into(root, rel, out);
            }
        } else if (S_ISREG(st.st_mode)) {
            if (!mkp_is_structural(rel)) {
                if (!path_array_push(out, rel)) {
                    err = NHP_ENOMEM;
                }
            }
        }
        /* symlinks and other special files (fifos, sockets, devices):
         * skipped entirely, never included in a made patch. */
        free(rel);
        free(abs);
    }
    (void)closedir(dp);
    return err;
}

static nhp_err walk_tree(const char *root, path_array *out)
{
    memset(out, 0, sizeof(*out));
    nhp_err err = walk_tree_into(root, "", out);
    if (err != NHP_OK) {
        path_array_free(out);
        return err;
    }
    qsort(out->items, out->count, sizeof(*out->items), cmp_str_ptr);
    return NHP_OK;
}

/* ---- comparing two trees ---- */

static bool file_array_contains(const path_array *arr, const char *path, size_t *out_index)
{
    for (size_t i = 0; i < arr->count; i++) {
        if (str_eq(arr->items[i], path)) {
            *out_index = i;
            return true;
        }
    }
    return false;
}

nhp_err mkp_compare_trees(const char *tree_a, const char *tree_b, const char *nethack_version,
                          const char *extra_ignore, mkp_file **out_files, size_t *out_count)
{
    *out_files = NULL;
    *out_count = 0;

    path_array a = {0};
    path_array b = {0};
    nhp_err err = walk_tree(tree_a, &a);
    if (err == NHP_OK) {
        err = walk_tree(tree_b, &b);
    }
    if (err != NHP_OK) {
        path_array_free(&a);
        path_array_free(&b);
        return err;
    }

    path_array all = {0};
    for (size_t i = 0; i < a.count && err == NHP_OK; i++) {
        if (!path_array_push(&all, a.items[i])) {
            err = NHP_ENOMEM;
        }
    }
    for (size_t i = 0; i < b.count && err == NHP_OK; i++) {
        size_t ignore_idx;
        if (!file_array_contains(&a, b.items[i], &ignore_idx)) {
            if (!path_array_push(&all, b.items[i])) {
                err = NHP_ENOMEM;
            }
        }
    }
    if (err == NHP_OK && all.count > 0) {
        qsort(all.items, all.count, sizeof(*all.items), cmp_str_ptr);
    }

    mkp_file *files = (err == NHP_OK && all.count > 0) ? calloc(all.count, sizeof(*files)) : NULL;
    if (err == NHP_OK && all.count > 0 && files == NULL) {
        err = NHP_ENOMEM;
    }

    size_t n = 0;
    for (size_t i = 0; err == NHP_OK && i < all.count; i++) {
        const char *path = all.items[i];
        size_t idx_a, idx_b;
        bool in_a = file_array_contains(&a, path, &idx_a);
        bool in_b = file_array_contains(&b, path, &idx_b);

        char *full_a = in_a ? path_join(tree_a, path) : NULL;
        char *full_b = in_b ? path_join(tree_b, path) : NULL;
        char *data_a = NULL, *data_b = NULL;
        size_t len_a = 0, len_b = 0;
        if (in_a && fs_read_file(full_a, &data_a, &len_a) != NHP_OK) {
            in_a = false; /* vanished between the walk and here -- treat as absent */
        }
        if (in_b && fs_read_file(full_b, &data_b, &len_b) != NHP_OK) {
            in_b = false;
        }
        free(full_a);
        free(full_b);

        mkp_status status;
        bool default_selected;
        bool skip = false;
        if (in_a && !in_b) {
            status = MKP_DELETED;
            default_selected = true;
        } else if (!in_a && in_b) {
            if (mkp_looks_binary(data_b, len_b)) {
                status = MKP_BINARY;
                default_selected = false;
            } else {
                status = MKP_ADDED;
                default_selected = mkp_looks_like_source(path);
            }
        } else if (in_a && in_b) {
            if (mkp_looks_binary(data_a, len_a) || mkp_looks_binary(data_b, len_b)) {
                if (len_a == len_b && (len_a == 0 || memcmp(data_a, data_b, len_a) == 0)) {
                    skip = true;
                    status = MKP_MODIFIED;
                    default_selected = false;
                } else {
                    status = MKP_BINARY;
                    default_selected = false;
                }
            } else if (len_a == len_b && (len_a == 0 || memcmp(data_a, data_b, len_a) == 0)) {
                skip = true;
                status = MKP_MODIFIED;
                default_selected = false;
            } else {
                status = MKP_MODIFIED;
                default_selected = true;
            }
        } else {
            skip = true; /* vanished from both sides */
            status = MKP_MODIFIED;
            default_selected = false;
        }
        free(data_a);
        free(data_b);

        if (!skip) {
            bool hidden = mkp_is_build_output(path, nethack_version, extra_ignore);
            files[n].path = str_dup(path);
            if (files[n].path == NULL) {
                err = NHP_ENOMEM;
                break;
            }
            files[n].status = status;
            files[n].hidden = hidden;
            files[n].default_selected = hidden ? false : default_selected;
            n++;
        }
    }

    path_array_free(&a);
    path_array_free(&b);
    path_array_free(&all);

    if (err != NHP_OK) {
        mkp_file_list_free(files, n);
        return err;
    }
    *out_files = files;
    *out_count = n;
    return NHP_OK;
}

/* ---- building the diff text for a chosen set of paths ---- */

static char *dirname_of(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (slash == NULL) {
        return str_dup(".");
    }
    return str_ndup(path, (size_t)(slash - path));
}

static nhp_err stage_one_side(const char *tree, const char *side_root, const char *rel_path)
{
    char *src = path_join(tree, rel_path);
    if (src == NULL) {
        return NHP_ENOMEM;
    }
    if (!fs_exists(src)) {
        free(src);
        return NHP_OK; /* missing on this side -- diff -N treats it as absent */
    }

    char *data = NULL;
    size_t len = 0;
    nhp_err err = fs_read_file(src, &data, &len);
    free(src);
    if (err != NHP_OK) {
        return err;
    }

    char *dst = path_join(side_root, rel_path);
    char *dst_dir = (dst != NULL) ? dirname_of(dst) : NULL;
    if (dst == NULL || dst_dir == NULL) {
        free(data);
        free(dst);
        free(dst_dir);
        return NHP_ENOMEM;
    }
    err = fs_mkdir_p(dst_dir, 0700);
    if (err == NHP_OK) {
        err = fs_write_atomic(dst, data, len);
    }
    if (err == NHP_OK) {
        /* Pin the staged copy's mtime so diff's "--- a/path <date>" headers
         * don't vary run to run (docs/testing.md 17.1's byte-identical
         * diff) -- freshly-written files would otherwise get fresh times. */
        struct utimbuf times = {0, 0};
        (void)utime(dst, &times);
    }
    free(data);
    free(dst);
    free(dst_dir);
    return err;
}

static nhp_err run_diff(const char *parent, char **out_diff)
{
    const char *argv[] = {"diff", "-Naur", "a", "b", NULL};
    proc_result result = {0};
    nhp_err err = proc_run(argv, parent, NULL, &result);
    if (err != NHP_OK) {
        proc_result_free(&result);
        return err;
    }
    if (result.signaled || result.exit_code > 1) {
        err_set("diff failed: %s", (result.err_data != NULL) ? result.err_data : "unknown error");
        proc_result_free(&result);
        return NHP_EIO;
    }
    *out_diff = str_dup((result.out_data != NULL) ? result.out_data : "");
    proc_result_free(&result);
    return (*out_diff != NULL) ? NHP_OK : NHP_ENOMEM;
}

nhp_err mkp_diff_files(const char *tree_a, const char *tree_b, const char *const *paths,
                       size_t count, char **out_diff)
{
    *out_diff = NULL;
    if (count == 0) {
        *out_diff = str_dup("");
        return (*out_diff != NULL) ? NHP_OK : NHP_ENOMEM;
    }
    for (size_t i = 0; i < count; i++) {
        if (!path_is_safe(paths[i])) {
            err_set("'%s' is not a safe path for a generated patch", paths[i]);
            return NHP_EINVAL;
        }
    }

    char *parent = fs_make_temp_dir("nhpm-diff");
    if (parent == NULL) {
        return NHP_ENOMEM;
    }
    char *side_a = path_join(parent, "a");
    char *side_b = path_join(parent, "b");
    nhp_err err = (side_a != NULL && side_b != NULL) ? NHP_OK : NHP_ENOMEM;
    if (err == NHP_OK) {
        err = fs_mkdir_p(side_a, 0700);
    }
    if (err == NHP_OK) {
        err = fs_mkdir_p(side_b, 0700);
    }
    for (size_t i = 0; err == NHP_OK && i < count; i++) {
        err = stage_one_side(tree_a, side_a, paths[i]);
        if (err == NHP_OK) {
            err = stage_one_side(tree_b, side_b, paths[i]);
        }
    }
    if (err == NHP_OK) {
        err = run_diff(parent, out_diff);
    }

    free(side_a);
    free(side_b);
    (void)fs_remove_tree(parent);
    free(parent);
    return err;
}

/* ---- the header block (section 12) ---- */

char *mkp_format_header(const mkp_header_info *info, const char *now_iso8601)
{
    buf b;
    buf_init(&b);
    buf_append_fmt(&b, "nhpm %s\n", info->kind);
    buf_append_fmt(&b, "Name:      %s\n", info->name);
    if (info->version != NULL && info->version[0] != '\0') {
        buf_append_fmt(&b, "Version:   %s\n", info->version);
    }
    if (info->author != NULL && info->author[0] != '\0') {
        buf_append_fmt(&b, "Author:    %s\n", info->author);
    }
    if (info->nethack_version != NULL && info->nethack_version[0] != '\0') {
        buf_append_fmt(&b, "NetHack:   %s\n", info->nethack_version);
    }
    buf_append_fmt(&b, "Created:   %s by nhpm %s\n", now_iso8601, NHP_VERSION);
    buf_append_fmt(&b, "Apply:     %s\n", info->apply_hint);

    if (info->short_desc != NULL && info->short_desc[0] != '\0') {
        buf_append_fmt(&b, "\n%s\n", info->short_desc);
    }
    if (info->long_desc != NULL && info->long_desc[0] != '\0') {
        buf_append_fmt(&b, "\n%s\n", info->long_desc);
    }
    if (info->contains_block != NULL && info->contains_block[0] != '\0') {
        buf_append_fmt(&b, "\n%s", info->contains_block);
    }
    (void)buf_append_str(&b, "\n");
    return buf_take(&b);
}

/* ---- shared plumbing: a clean copy, and applying the enabled stack ---- */

/* Mirrors stack.c's private find_patch_filename(): scans <src_dir>/patches/
 * for a file named `id` or "<id>-*". Duplicated since stack.c doesn't
 * export it and it's small enough to keep module-local. */
static char *find_patch_filename(const char *src_dir, const char *id)
{
    char *patches_dir = path_join(src_dir, "patches");
    if (patches_dir == NULL) {
        return NULL;
    }
    DIR *dp = opendir(patches_dir);
    free(patches_dir);
    if (dp == NULL) {
        return NULL;
    }

    size_t id_len = strlen(id);
    char *found = NULL;
    const struct dirent *entry;
    while (found == NULL && (entry = readdir(dp)) != NULL) {
        size_t name_len = strlen(entry->d_name);
        if (name_len > 0 && entry->d_name[name_len - 1] == '~') {
            continue;
        }
        bool is_exact = str_eq(entry->d_name, id);
        bool is_prefixed = strncmp(entry->d_name, id, id_len) == 0 && entry->d_name[id_len] == '-';
        if (is_exact || is_prefixed) {
            found = str_dup(entry->d_name);
        }
    }
    (void)closedir(dp);
    return found;
}

/* docs/features.md 12.2 step 1: a disposable copy of *inst's clean base --
 * the base source for a managed instance, or the source folder popped
 * clean with Quilt (in the copy) for a linked one. Caller removes *out_dir. */
static nhp_err make_clean_copy(const inst_instance *inst, char **out_dir)
{
    *out_dir = NULL;
    char *dest = fs_make_temp_dir("nhpm-clean");
    if (dest == NULL) {
        return NHP_ENOMEM;
    }

    nhp_err err;
    if (inst->kind == INST_LINKED) {
        char *live = inst_source_full_path(inst);
        err = (live != NULL) ? fs_copy_tree(live, dest) : NHP_ENOMEM;
        free(live);
        if (err == NHP_OK) {
            proc_result result = {0};
            err = quilt_pop_all(dest, &result);
            bool none_applied = !result.signaled && result.err_data != NULL &&
                                strstr(result.err_data, "No patch") != NULL;
            if (err == NHP_OK && (result.signaled || (result.exit_code != 0 && !none_applied))) {
                err_set("could not clean a copy of %s's source: %s", inst->name,
                        (result.err_data != NULL) ? result.err_data : "quilt pop -a failed");
                err = NHP_EIO;
            }
            proc_result_free(&result);
        }
    } else {
        char *base = bsrc_tree_path(inst->source);
        err = (base != NULL) ? fs_copy_tree(base, dest) : NHP_ENOMEM;
        free(base);
        if (err == NHP_OK) {
            /* Base source is chmod'd read-only (sources.c) and fs_copy_tree
             * preserves that, so re-chmod writable before patch/remove_tree
             * can touch it -- same as inst_new()'s copy of the same base. */
            const char *argv[] = {"chmod", "-R", "u+w", dest, NULL};
            proc_result result = {0};
            err = proc_run(argv, NULL, NULL, &result);
            if (err == NHP_OK && (result.signaled || result.exit_code != 0)) {
                err_set("could not make a clean copy of %s's base source writable", inst->name);
                err = NHP_EIO;
            }
            proc_result_free(&result);
        }
    }

    if (err != NHP_OK) {
        (void)fs_remove_tree(dest);
        free(dest);
        return err;
    }
    *out_dir = dest;
    return NHP_OK;
}

/* Applies every enabled patch in *inst's stack order onto `target_dir`
 * (plain dir, not Quilt-tracked -- "patch -pN --batch") from the
 * instance's patches/ folder. Only ids in include_ids if non-NULL. */
static nhp_err apply_enabled_patches(const inst_instance *inst, const char *target_dir,
                                     const char *const *include_ids, size_t include_count)
{
    char *src_dir = inst_source_full_path(inst);
    if (src_dir == NULL) {
        return NHP_ENOMEM;
    }

    nhp_err err = NHP_OK;
    for (size_t i = 0; i < inst->patch_count && err == NHP_OK; i++) {
        const inst_patch *p = &inst->patches[i];
        if (!p->enabled) {
            continue;
        }
        if (include_ids != NULL) {
            bool included = false;
            for (size_t j = 0; j < include_count; j++) {
                if (str_eq(include_ids[j], p->id)) {
                    included = true;
                    break;
                }
            }
            if (!included) {
                continue;
            }
        }
        if (p->strip < 0) {
            err_set("%s has no stored strip level", p->id);
            err = NHP_EINVAL;
            break;
        }

        char *filename = find_patch_filename(src_dir, p->id);
        if (filename == NULL) {
            err_set("could not find %s's patch file under %s/patches", p->id, src_dir);
            err = NHP_ENOENT;
            break;
        }
        char *patches_dir = path_join(src_dir, "patches");
        char *abs_patch = (patches_dir != NULL) ? path_join(patches_dir, filename) : NULL;
        free(patches_dir);
        free(filename);
        if (abs_patch == NULL) {
            err = NHP_ENOMEM;
            break;
        }

        char strip_arg[16];
        (void)snprintf(strip_arg, sizeof(strip_arg), "-p%d", p->strip);
        const char *argv[] = {"patch",    strip_arg, "--batch", "--force", "-d",
                              target_dir, "-i",      abs_patch, NULL};
        proc_result result = {0};
        err = proc_run(argv, NULL, NULL, &result);
        if (err == NHP_OK && (result.signaled || result.exit_code != 0)) {
            err_set("applying %s to a clean copy failed, which shouldn't happen since the "
                    "instance itself applies it: %s",
                    p->id, (result.err_data != NULL) ? result.err_data : "unknown error");
            err = NHP_EIO;
        }
        proc_result_free(&result);
        free(abs_patch);
    }

    free(src_dir);
    return err;
}

/* docs/features.md 12.3 steps 1-2: clean base + every enabled patch
 * applied, in a disposable copy -- what the working folder "should"
 * contain. */
static nhp_err build_expected_copy(const inst_instance *inst, char **out_dir)
{
    char *dir = NULL;
    nhp_err err = make_clean_copy(inst, &dir);
    if (err != NHP_OK) {
        return err;
    }
    err = apply_enabled_patches(inst, dir, NULL, 0);
    if (err != NHP_OK) {
        (void)fs_remove_tree(dir);
        free(dir);
        return err;
    }
    *out_dir = dir;
    return NHP_OK;
}

/* ---- 12.2 / `combined` ---- */

void mkp_combined_result_free(mkp_combined_result *r)
{
    free(r->diff);
    r->diff = NULL;
    r->file_count = 0;
}

nhp_err mkp_combined(const inst_instance *inst, const char *const *include_ids,
                     size_t include_count, const char *extra_ignore, mkp_combined_result *out)
{
    memset(out, 0, sizeof(*out));

    char *tree_a = NULL;
    nhp_err err = make_clean_copy(inst, &tree_a);
    if (err != NHP_OK) {
        return err;
    }
    char *tree_b = fs_make_temp_dir("nhpm-combined");
    if (tree_b == NULL) {
        (void)fs_remove_tree(tree_a);
        free(tree_a);
        return NHP_ENOMEM;
    }
    err = fs_copy_tree(tree_a, tree_b);
    if (err == NHP_OK) {
        err = apply_enabled_patches(inst, tree_b, include_ids, include_count);
    }

    mkp_file *files = NULL;
    size_t file_count = 0;
    if (err == NHP_OK) {
        err = mkp_compare_trees(tree_a, tree_b, inst->source, extra_ignore, &files, &file_count);
    }

    char *diff_body = NULL;
    size_t selected_count = 0;
    char **selected = NULL;
    if (err == NHP_OK) {
        selected = (file_count > 0) ? malloc(file_count * sizeof(*selected)) : NULL;
        if (file_count > 0 && selected == NULL) {
            err = NHP_ENOMEM;
        }
        for (size_t i = 0; err == NHP_OK && i < file_count; i++) {
            if (files[i].status != MKP_BINARY && !files[i].hidden) {
                selected[selected_count++] = files[i].path;
            }
        }
    }
    if (err == NHP_OK) {
        err = mkp_diff_files(tree_a, tree_b, (const char *const *)selected, selected_count,
                             &diff_body);
    }

    if (err == NHP_OK && selected_count > 0) {
        char *diff_tmp_path = NULL;
        err = fs_write_temp_file("nhpm-combined-verify", diff_body, strlen(diff_body),
                                 &diff_tmp_path);
        if (err == NHP_OK) {
            quilt_dry_run dry;
            err = quilt_dry_run_patch(tree_a, diff_tmp_path, 1, &dry);
            if (err == NHP_OK) {
                if (dry.failed_hunks != 0 || dry.fuzzy_hunks != 0 || dry.no_file_found) {
                    err_set("the combined patch for %s failed its own verification dry run "
                            "(%d of %d hunks failed) -- this is an nhpm bug, not yours: %s",
                            inst->name, dry.failed_hunks, dry.total_hunks, dry.raw_output);
                    err = NHP_EIO;
                }
                quilt_dry_run_free(&dry);
            }
            (void)unlink(diff_tmp_path);
            free(diff_tmp_path);
        }
    }

    if (err == NHP_OK) {
        buf apply_buf;
        buf_init(&apply_buf);
        buf_append_fmt(&apply_buf, "cd nethack-%s && patch -p1 < %s-combined.diff", inst->source,
                       inst->name);
        char *apply_hint = buf_take(&apply_buf);

        buf contains;
        buf_init(&contains);
        buf_append_str(&contains, "Contains, in order:\n");
        size_t n = 0;
        for (size_t i = 0; i < inst->patch_count; i++) {
            const inst_patch *p = &inst->patches[i];
            bool included = p->enabled;
            if (included && include_ids != NULL) {
                included = false;
                for (size_t j = 0; j < include_count; j++) {
                    if (str_eq(include_ids[j], p->id)) {
                        included = true;
                        break;
                    }
                }
            }
            if (!included) {
                continue;
            }
            n++;
            lib_patch lp;
            char *display_name = NULL;
            if (lib_find(p->id, &lp) == NHP_OK) {
                display_name = str_dup(lp.name);
                lib_patch_free(&lp);
            }
            bool is_local = strncmp(p->id, "local-", 6) == 0;
            buf_append_fmt(&contains, "  %zu. %-28s %-8s %s   sha256 %.8s...\n", n,
                           (display_name != NULL) ? display_name : p->id, p->version,
                           is_local ? "(local)" : "(nhpmdb)",
                           (p->sha256[0] != '\0') ? p->sha256 : "unknown ");
            free(display_name);
        }
        char *contains_block = buf_take(&contains);

        char *now = time_now_iso8601();
        mkp_header_info hdr = {
            .kind = "combined patch",
            .name = inst->name,
            .version = "",
            .author = "",
            .nethack_version = inst->source,
            .apply_hint = apply_hint,
            .short_desc = "",
            .long_desc = "",
            .contains_block = contains_block,
        };
        char *header = mkp_format_header(&hdr, (now != NULL) ? now : "");
        free(now);
        free(apply_hint);
        free(contains_block);

        buf full;
        buf_init(&full);
        (void)buf_append_str(&full, header);
        (void)buf_append_str(&full, diff_body);
        free(header);
        out->diff = buf_take(&full);
        out->file_count = selected_count;
    }

    free(selected);
    free(diff_body);
    mkp_file_list_free(files, file_count);
    (void)fs_remove_tree(tree_a);
    (void)fs_remove_tree(tree_b);
    free(tree_a);
    free(tree_b);
    return err;
}

/* ---- 12.3 steps 1-2 / `changes` ---- */

nhp_err mkp_changes(const inst_instance *inst, const char *extra_ignore, mkp_file **out_files,
                    size_t *out_count)
{
    *out_files = NULL;
    *out_count = 0;

    char *expected = NULL;
    nhp_err err = build_expected_copy(inst, &expected);
    if (err != NHP_OK) {
        return err;
    }
    char *live = inst_source_full_path(inst);
    if (live == NULL) {
        (void)fs_remove_tree(expected);
        free(expected);
        return NHP_ENOMEM;
    }

    err = mkp_compare_trees(expected, live, inst->source, extra_ignore, out_files, out_count);

    (void)fs_remove_tree(expected);
    free(expected);
    free(live);
    return err;
}

nhp_err mkp_changes_diff(const inst_instance *inst, const char *const *paths, size_t count,
                         char **out_diff)
{
    *out_diff = NULL;

    char *expected = NULL;
    nhp_err err = build_expected_copy(inst, &expected);
    if (err != NHP_OK) {
        return err;
    }
    char *live = inst_source_full_path(inst);
    if (live == NULL) {
        (void)fs_remove_tree(expected);
        free(expected);
        return NHP_ENOMEM;
    }

    err = mkp_diff_files(expected, live, paths, count, out_diff);

    (void)fs_remove_tree(expected);
    free(expected);
    free(live);
    return err;
}

/* ---- 12.3 steps 3-4 / `capture` ---- */

typedef struct backup_entry {
    char *path;
    bool existed;
} backup_entry;

static void free_backup(backup_entry *entries, size_t count, const char *backup_dir)
{
    for (size_t i = 0; i < count; i++) {
        free(entries[i].path);
    }
    free(entries);
    if (backup_dir != NULL) {
        (void)fs_remove_tree(backup_dir);
    }
}

/* Backs up `paths[0..count)`'s content from `live_dir` into `backup_dir`
 * (created fresh), recording whether each existed (docs/features.md
 * 12.3 step 3.1, docs/security.md's "capturing changes is reversible"). */
static nhp_err backup_files(const char *live_dir, const char *backup_dir, const char *const *paths,
                            size_t count, backup_entry **out_entries)
{
    *out_entries = NULL;
    nhp_err err = fs_mkdir_p(backup_dir, 0700);
    if (err != NHP_OK) {
        return err;
    }

    backup_entry *entries = calloc(count, sizeof(*entries));
    if (entries == NULL) {
        return NHP_ENOMEM;
    }

    for (size_t i = 0; i < count && err == NHP_OK; i++) {
        entries[i].path = str_dup(paths[i]);
        if (entries[i].path == NULL) {
            err = NHP_ENOMEM;
            break;
        }
        char *live_path = path_join(live_dir, paths[i]);
        entries[i].existed = (live_path != NULL) && fs_exists(live_path);
        if (entries[i].existed) {
            char *data = NULL;
            size_t len = 0;
            err = fs_read_file(live_path, &data, &len);
            if (err == NHP_OK) {
                char *dst = path_join(backup_dir, paths[i]);
                char *dst_dir = (dst != NULL) ? dirname_of(dst) : NULL;
                err = (dst != NULL && dst_dir != NULL) ? NHP_OK : NHP_ENOMEM;
                if (err == NHP_OK) {
                    err = fs_mkdir_p(dst_dir, 0700);
                }
                if (err == NHP_OK) {
                    err = fs_write_atomic(dst, data, len);
                }
                free(dst);
                free(dst_dir);
            }
            free(data);
        }
        free(live_path);
    }

    if (err != NHP_OK) {
        free_backup(entries, count, NULL);
        return err;
    }
    *out_entries = entries;
    return NHP_OK;
}

/* Undoes backup_files()'s effect on the live folder: restores a backed-
 * up file's content, or removes one that didn't exist before (so a
 * capture that failed partway never leaves a new file behind). */
static void restore_backup(const char *live_dir, const char *backup_dir,
                           const backup_entry *entries, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        char *live_path = path_join(live_dir, entries[i].path);
        if (live_path == NULL) {
            continue;
        }
        if (entries[i].existed) {
            char *backup_path = path_join(backup_dir, entries[i].path);
            char *data = NULL;
            size_t len = 0;
            if (backup_path != NULL && fs_read_file(backup_path, &data, &len) == NHP_OK) {
                (void)fs_write_atomic(live_path, data, len);
            }
            free(data);
            free(backup_path);
        } else {
            (void)unlink(live_path);
        }
        free(live_path);
    }
}

static char *timestamp_for_filename(void)
{
    char *iso = time_now_iso8601();
    if (iso == NULL) {
        return NULL;
    }
    for (char *p = iso; *p != '\0'; p++) {
        if (*p == ':') {
            *p = '-';
        }
    }
    return iso;
}

nhp_err mkp_capture(inst_instance *inst, const mkp_patch_meta *meta,
                    const char *const *selected_files, size_t count, char **out_id)
{
    *out_id = NULL;
    if (count == 0) {
        err_set("no changes selected to capture");
        return NHP_ENOENT;
    }
    for (size_t i = 0; i < count; i++) {
        if (!path_is_safe(selected_files[i])) {
            err_set("'%s' is not a safe path for a generated patch", selected_files[i]);
            return NHP_EINVAL;
        }
    }

    char *live_dir = inst_source_full_path(inst);
    if (live_dir == NULL) {
        return NHP_ENOMEM;
    }

    char *expected = NULL;
    nhp_err err = build_expected_copy(inst, &expected);
    if (err != NHP_OK) {
        free(live_dir);
        return err;
    }

    char *diff_body = NULL;
    err = mkp_diff_files(expected, live_dir, selected_files, count, &diff_body);
    (void)fs_remove_tree(expected);
    free(expected);
    if (err != NHP_OK) {
        free(live_dir);
        return err;
    }
    if (diff_body[0] == '\0') {
        err_set("no changes found (press h on the Changes screen if files were hidden as build "
                "output)");
        free(diff_body);
        free(live_dir);
        return NHP_ENOENT;
    }

    const char *version =
        (meta->version != NULL && meta->version[0] != '\0') ? meta->version : "1.0";
    const char *target =
        (meta->target != NULL && meta->target[0] != '\0') ? meta->target : inst->source;

    buf apply_buf;
    buf_init(&apply_buf);
    buf_append_fmt(&apply_buf, "patch -p1 < %s-%s.diff", meta->name, version);
    char *apply_hint = buf_take(&apply_buf);
    char *now = time_now_iso8601();
    mkp_header_info hdr = {
        .kind = "patch",
        .name = meta->name,
        .version = version,
        .author = (meta->author != NULL) ? meta->author : "",
        .nethack_version = target,
        .apply_hint = apply_hint,
        .short_desc = (meta->short_desc != NULL) ? meta->short_desc : "",
        .long_desc = (meta->long_desc != NULL) ? meta->long_desc : "",
        .contains_block = "",
    };
    char *header = mkp_format_header(&hdr, (now != NULL) ? now : "");
    free(now);
    free(apply_hint);

    buf full;
    buf_init(&full);
    (void)buf_append_str(&full, header);
    (void)buf_append_str(&full, diff_body);
    free(header);
    free(diff_body);
    char *full_text = buf_take(&full);

    char *snapshot_name = NULL;
    err = inst_snapshot_create(inst->name, "before capture", &snapshot_name);
    if (err != NHP_OK) {
        free(full_text);
        free(live_dir);
        return err;
    }

    char *inst_dir_path = inst_dir(inst->name);
    char *backup_stamp = timestamp_for_filename();
    char *backup_dir = NULL;
    if (inst_dir_path != NULL && backup_stamp != NULL) {
        char *backups_root = path_join(inst_dir_path, "backup");
        backup_dir = (backups_root != NULL) ? path_join(backups_root, backup_stamp) : NULL;
        free(backups_root);
    }
    free(inst_dir_path);
    free(backup_stamp);
    if (backup_dir == NULL) {
        free(full_text);
        free(live_dir);
        free(snapshot_name);
        return NHP_ENOMEM;
    }

    backup_entry *entries = NULL;
    err = backup_files(live_dir, backup_dir, selected_files, count, &entries);
    if (err != NHP_OK) {
        free(full_text);
        free(live_dir);
        free(snapshot_name);
        free(backup_dir);
        return err;
    }

    char *diff_file_path = NULL;
    err = fs_write_temp_file("nhpm-capture", full_text, strlen(full_text), &diff_file_path);
    if (err != NHP_OK) {
        goto fail_before_touching_folder;
    }

    {
        const char *argv[] = {"patch", "-R",     "-p1", "--batch",      "--force",
                              "-d",    live_dir, "-i",  diff_file_path, NULL};
        proc_result result = {0};
        err = proc_run(argv, NULL, NULL, &result);
        bool failed = (err == NHP_OK) && (result.signaled || result.exit_code != 0);
        if (failed) {
            err_set("could not reverse-apply the capture to the working folder: %s",
                    (result.err_data != NULL) ? result.err_data : "unknown error");
            err = NHP_EIO;
        }
        proc_result_free(&result);
    }
    if (err != NHP_OK) {
        restore_backup(live_dir, backup_dir, entries, count);
        goto fail_before_touching_folder;
    }

    char *new_id = NULL;
    err = lib_import_local(diff_file_path, meta->name, hdr.author, target, version, &new_id);
    if (err != NHP_OK) {
        restore_backup(live_dir, backup_dir, entries, count);
        goto fail_before_touching_folder;
    }

    char sha[65];
    sha256_hex(full_text, strlen(full_text), sha);
    size_t new_index = 0;
    stack_conflict conflict = {0};
    err = stack_add(inst, new_id, version, meta->name, diff_file_path, sha, true, &new_index,
                    &conflict);
    if (err != NHP_OK) {
        if (err == NHP_ECONFLICT) {
            stack_conflict_free(&conflict);
        }
        size_t idx;
        if (inst_find_patch_index(inst, new_id, &idx) == NHP_OK) {
            stack_conflict c2 = {0};
            (void)stack_remove(inst, idx, &c2);
            stack_conflict_free(&c2);
        }
        (void)lib_delete_patch(new_id);
        restore_backup(live_dir, backup_dir, entries, count);
        (void)inst_snapshot_restore(inst->name, snapshot_name);
        free(new_id);
        goto fail_before_touching_folder;
    }

    *out_id = new_id;
    (void)unlink(diff_file_path);
    free(diff_file_path);
    free_backup(entries, count, backup_dir);
    free(backup_dir);
    free(full_text);
    free(live_dir);
    free(snapshot_name);
    return NHP_OK;

fail_before_touching_folder:
    free(diff_file_path);
    free_backup(entries, count, backup_dir);
    free(backup_dir);
    free(full_text);
    free(live_dir);
    free(snapshot_name);
    return err;
}

/* ---- 12.6 / `mkpatch`: a patch from any modified folder ---- */

static char *synthesize_new_file_diff(const char *path, const char *data, size_t len)
{
    size_t line_count = 0;
    bool ends_with_newline = (len > 0 && data[len - 1] == '\n');
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n') {
            line_count++;
        }
    }
    if (!ends_with_newline && len > 0) {
        line_count++;
    }

    buf out;
    buf_init(&out);
    buf_append_fmt(&out, "diff -Naur a/%s b/%s\n--- /dev/null\n+++ b/%s\n@@ -0,0 +1,%zu @@\n", path,
                   path, path, line_count);
    size_t i = 0;
    while (i < len) {
        size_t start = i;
        while (i < len && data[i] != '\n') {
            i++;
        }
        (void)buf_append(&out, "+", 1);
        (void)buf_append(&out, data + start, i - start);
        if (i < len) {
            (void)buf_append(&out, "\n", 1);
            i++;
        } else {
            (void)buf_append_str(&out, "\n\\ No newline at end of file\n");
        }
    }
    return buf_take(&out);
}

static bool path_list_contains(const char *const *paths, size_t count, const char *path)
{
    for (size_t i = 0; i < count; i++) {
        if (str_eq(paths[i], path)) {
            return true;
        }
    }
    return false;
}

static nhp_err diff_from_git_ref(const char *folder, const char *ref, const char *nethack_version,
                                 const char *extra_ignore, const char *const *selected_files,
                                 size_t count, char **out_diff)
{
    char *git_marker = path_join(folder, ".git");
    bool is_git = (git_marker != NULL) && fs_exists(git_marker);
    free(git_marker);
    if (!is_git) {
        err_set("%s is not a git repository", folder);
        return NHP_EINVAL;
    }

    buf out;
    buf_init(&out);

    {
        size_t argc = 4 + count;
        const char **argv = malloc((argc + 1) * sizeof(*argv));
        if (argv == NULL) {
            return NHP_ENOMEM;
        }
        argv[0] = "git";
        argv[1] = "diff";
        argv[2] = ref;
        argv[3] = "--";
        for (size_t i = 0; i < count; i++) {
            argv[4 + i] = selected_files[i];
        }
        argv[argc] = NULL;

        proc_result result = {0};
        nhp_err err = proc_run(argv, folder, NULL, &result);
        free(argv);
        if (err == NHP_OK && (result.signaled || result.exit_code > 1)) {
            err_set("git diff %s failed: %s", ref,
                    (result.err_data != NULL) ? result.err_data : "unknown error");
            err = NHP_EIO;
        }
        if (err != NHP_OK) {
            proc_result_free(&result);
            buf_free(&out);
            return err;
        }
        if (result.out_data != NULL) {
            (void)buf_append_str(&out, result.out_data);
        }
        proc_result_free(&result);
    }

    {
        const char *argv[] = {"git", "ls-files", "--others", "--exclude-standard", NULL};
        proc_result result = {0};
        nhp_err err = proc_run(argv, folder, NULL, &result);
        if (err == NHP_OK && (result.signaled || result.exit_code != 0)) {
            err_set("git ls-files failed: %s",
                    (result.err_data != NULL) ? result.err_data : "unknown error");
            err = NHP_EIO;
        }
        if (err != NHP_OK) {
            proc_result_free(&result);
            buf_free(&out);
            return err;
        }

        char *copy = str_dup((result.out_data != NULL) ? result.out_data : "");
        proc_result_free(&result);
        if (copy == NULL) {
            buf_free(&out);
            return NHP_ENOMEM;
        }
        char *saveptr = NULL;
        const char *line = strtok_r(copy, "\n", &saveptr);
        while (line != NULL) {
            bool include = (count > 0) ? path_list_contains(selected_files, count, line)
                                       : (!mkp_is_ignored(line, nethack_version, extra_ignore) &&
                                          mkp_looks_like_source(line));
            if (include) {
                char *full = path_join(folder, line);
                char *data = NULL;
                size_t len = 0;
                if (full != NULL && fs_read_file(full, &data, &len) == NHP_OK &&
                    !mkp_looks_binary(data, len)) {
                    char *block = synthesize_new_file_diff(line, data, len);
                    if (block != NULL) {
                        (void)buf_append_str(&out, block);
                    }
                    free(block);
                }
                free(data);
                free(full);
            }
            line = strtok_r(NULL, "\n", &saveptr);
        }
        free(copy);
    }

    *out_diff = buf_take(&out);
    return NHP_OK;
}

static nhp_err resolve_base_tree(const mkp_base_spec *base, const char *folder_version,
                                 char **out_tree, char **out_temp_to_remove)
{
    *out_tree = NULL;
    *out_temp_to_remove = NULL;

    switch (base->kind) {
        case MKP_BASE_AUTO: {
            if (folder_version == NULL || folder_version[0] == '\0') {
                err_set("could not determine the folder's version to find a matching base "
                        "source -- specify --from explicitly");
                return NHP_EPARSE;
            }
            bsrc_source src;
            nhp_err err = bsrc_find(folder_version, &src);
            if (err != NHP_OK) {
                err_set("no base source for version %s -- import one, or specify --from",
                        folder_version);
                return err;
            }
            bsrc_source_free(&src);
            char *tree = bsrc_tree_path(folder_version);
            if (tree == NULL) {
                return NHP_ENOMEM;
            }
            *out_tree = tree;
            return NHP_OK;
        }
        case MKP_BASE_FOLDER:
            *out_tree = str_dup(base->path_or_ref);
            return (*out_tree != NULL) ? NHP_OK : NHP_ENOMEM;
        case MKP_BASE_TARBALL: {
            char *dest = fs_make_temp_dir("nhpm-mkpatch-base");
            if (dest == NULL) {
                return NHP_ENOMEM;
            }
            nhp_err err = bsrc_extract_tarball_checked(base->path_or_ref, dest);
            if (err != NHP_OK) {
                (void)fs_remove_tree(dest);
                free(dest);
                return err;
            }
            *out_tree = dest;
            *out_temp_to_remove = str_dup(dest);
            return (*out_temp_to_remove != NULL) ? NHP_OK : NHP_ENOMEM;
        }
        case MKP_BASE_GIT_REF:
        default:
            err_set("MKP_BASE_GIT_REF has no tree to resolve -- handled separately");
            return NHP_EINVAL;
    }
}

/* Shared by mkp_from_folder()/mkp_from_folder_changes(): resolves the base
 * tree (never MKP_BASE_GIT_REF) and runs mkp_compare_trees(), handing back
 * resolve_base_tree()'s outputs uncleaned for reuse; caller frees them. */
static nhp_err compare_folder_to_base(const char *folder, const mkp_base_spec *base,
                                      const char *folder_version, const char *extra_ignore,
                                      mkp_file **out_files, size_t *out_count, char **out_tree_a,
                                      char **out_temp_to_remove)
{
    *out_files = NULL;
    *out_count = 0;
    *out_tree_a = NULL;
    *out_temp_to_remove = NULL;

    nhp_err err = resolve_base_tree(base, folder_version, out_tree_a, out_temp_to_remove);
    if (err != NHP_OK) {
        return err;
    }
    err =
        mkp_compare_trees(*out_tree_a, folder, folder_version, extra_ignore, out_files, out_count);
    if (err != NHP_OK) {
        if (*out_temp_to_remove != NULL) {
            (void)fs_remove_tree(*out_temp_to_remove);
            free(*out_temp_to_remove);
            *out_temp_to_remove = NULL;
        }
        free(*out_tree_a);
        *out_tree_a = NULL;
    }
    return err;
}

/* docs/features.md 12.6's comparison step, split out so a caller (Library's
 * "m") can tick files/review diffs like mkp_changes() does for an instance.
 * Not for MKP_BASE_GIT_REF (compared via "git diff" text instead). */
nhp_err mkp_from_folder_changes(const char *folder, const mkp_base_spec *base,
                                const char *extra_ignore, mkp_file **out_files, size_t *out_count)
{
    *out_files = NULL;
    *out_count = 0;
    if (base->kind == MKP_BASE_GIT_REF) {
        err_set("a git ref base is compared directly with 'git diff', not file by file -- "
                "there's nothing to tick");
        return NHP_EINVAL;
    }

    ver_version v;
    bool have_version = ver_read_patchlevel(folder, &v) == NHP_OK;
    char *folder_version = have_version ? ver_format(&v) : NULL;
    if (have_version) {
        ver_free(&v);
    }

    char *tree_a = NULL;
    char *temp_to_remove = NULL;
    nhp_err err = compare_folder_to_base(folder, base, folder_version, extra_ignore, out_files,
                                         out_count, &tree_a, &temp_to_remove);
    free(folder_version);
    if (temp_to_remove != NULL) {
        (void)fs_remove_tree(temp_to_remove);
        free(temp_to_remove);
    }
    free(tree_a);
    return err;
}

/* Same comparison as mkp_from_folder_changes(), but a diff of exactly
 * `paths` (docs/features.md 12.5's "Enter shows the full diff"). Not for
 * MKP_BASE_GIT_REF, same reason mkp_from_folder_changes() isn't. */
nhp_err mkp_from_folder_diff(const char *folder, const mkp_base_spec *base,
                             const char *const *paths, size_t count, char **out_diff)
{
    *out_diff = NULL;
    if (base->kind == MKP_BASE_GIT_REF) {
        err_set("a git ref base is compared directly with 'git diff', not file by file");
        return NHP_EINVAL;
    }

    ver_version v;
    bool have_version = ver_read_patchlevel(folder, &v) == NHP_OK;
    char *folder_version = have_version ? ver_format(&v) : NULL;
    if (have_version) {
        ver_free(&v);
    }

    char *tree_a = NULL;
    char *temp_to_remove = NULL;
    nhp_err err = resolve_base_tree(base, folder_version, &tree_a, &temp_to_remove);
    free(folder_version);
    if (err == NHP_OK) {
        err = mkp_diff_files(tree_a, folder, paths, count, out_diff);
    }
    if (temp_to_remove != NULL) {
        (void)fs_remove_tree(temp_to_remove);
        free(temp_to_remove);
    }
    free(tree_a);
    return err;
}

nhp_err mkp_from_folder(const char *folder, const mkp_base_spec *base, const mkp_patch_meta *meta,
                        const char *extra_ignore, const char *const *selected_files, size_t count,
                        char **out_id)
{
    *out_id = NULL;
    for (size_t i = 0; i < count; i++) {
        if (!path_is_safe(selected_files[i])) {
            err_set("'%s' is not a safe path for a generated patch", selected_files[i]);
            return NHP_EINVAL;
        }
    }

    ver_version v;
    bool have_version = ver_read_patchlevel(folder, &v) == NHP_OK;
    char *folder_version = have_version ? ver_format(&v) : NULL;
    if (have_version) {
        ver_free(&v);
    }
    const char *target = (meta->target != NULL && meta->target[0] != '\0')
                             ? meta->target
                             : ((folder_version != NULL) ? folder_version : "");

    char *diff_body = NULL;
    nhp_err err;

    if (base->kind == MKP_BASE_GIT_REF) {
        err = diff_from_git_ref(folder, base->path_or_ref, folder_version, extra_ignore,
                                selected_files, count, &diff_body);
    } else {
        mkp_file *files = NULL;
        size_t file_count = 0;
        char *tree_a = NULL;
        char *temp_to_remove = NULL;
        err = compare_folder_to_base(folder, base, folder_version, extra_ignore, &files,
                                     &file_count, &tree_a, &temp_to_remove);
        if (err == NHP_OK) {
            const char *const *use_paths = selected_files;
            size_t use_count = count;
            char **computed = NULL;
            if (count == 0) {
                computed = (file_count > 0) ? malloc(file_count * sizeof(*computed)) : NULL;
                size_t n = 0;
                for (size_t i = 0; i < file_count; i++) {
                    if (files[i].default_selected) {
                        computed[n++] = files[i].path;
                    }
                }
                use_paths = (const char *const *)computed;
                use_count = n;
            }
            err = mkp_diff_files(tree_a, folder, use_paths, use_count, &diff_body);
            free(computed);
        }
        mkp_file_list_free(files, file_count);
        if (temp_to_remove != NULL) {
            (void)fs_remove_tree(temp_to_remove);
            free(temp_to_remove);
        }
        free(tree_a);
    }
    free(folder_version);

    if (err != NHP_OK) {
        return err;
    }
    if (diff_body[0] == '\0') {
        err_set("no changes found between %s and the given base", folder);
        free(diff_body);
        return NHP_ENOENT;
    }

    const char *version =
        (meta->version != NULL && meta->version[0] != '\0') ? meta->version : "1.0";
    buf apply_buf;
    buf_init(&apply_buf);
    buf_append_fmt(&apply_buf, "patch -p1 < %s-%s.diff", meta->name, version);
    char *apply_hint = buf_take(&apply_buf);
    char *now = time_now_iso8601();
    mkp_header_info hdr = {
        .kind = "patch",
        .name = meta->name,
        .version = version,
        .author = (meta->author != NULL) ? meta->author : "",
        .nethack_version = target,
        .apply_hint = apply_hint,
        .short_desc = (meta->short_desc != NULL) ? meta->short_desc : "",
        .long_desc = (meta->long_desc != NULL) ? meta->long_desc : "",
        .contains_block = "",
    };
    char *header = mkp_format_header(&hdr, (now != NULL) ? now : "");
    free(now);
    free(apply_hint);

    buf full;
    buf_init(&full);
    (void)buf_append_str(&full, header);
    (void)buf_append_str(&full, diff_body);
    free(header);
    free(diff_body);
    char *full_text = buf_take(&full);

    char *diff_file_path = NULL;
    err = fs_write_temp_file("nhpm-mkpatch", full_text, strlen(full_text), &diff_file_path);
    free(full_text);
    if (err != NHP_OK) {
        return err;
    }

    err = lib_import_local(diff_file_path, meta->name, hdr.author, target, version, out_id);
    (void)unlink(diff_file_path);
    free(diff_file_path);
    return err;
}

/* ---- 12.7 / `export-patch` ---- */

/* If `data` starts with an nhpm header ("nhpm combined patch\n" /
 * "nhpm patch\n"), returns a pointer past it to the real diff; else
 * returns `data` unchanged, so it's safe on a plain downloaded patch. */
static const char *skip_existing_header(const char *data)
{
    if (strncmp(data, "nhpm ", 8) != 0) {
        return data;
    }
    const char *blank = strstr(data, "\n\n");
    while (blank != NULL) {
        const char *after = blank + 2;
        if (strncmp(after, "diff ", 5) == 0 || strncmp(after, "--- ", 4) == 0 ||
            strncmp(after, "---\t", 4) == 0) {
            return after;
        }
        blank = strstr(after, "\n\n");
    }
    return data; /* no diff-looking line found -- leave it as-is */
}

nhp_err mkp_export_patch(const char *id, const char *version, const char *out_path)
{
    lib_patch patch;
    nhp_err err = lib_find(id, &patch);
    if (err != NHP_OK) {
        return err;
    }

    const char *use_version = version;
    if (use_version == NULL && patch.version_count > 0) {
        use_version = patch.versions[patch.version_count - 1].version;
    }
    if (use_version == NULL) {
        err_set("%s has no downloaded versions", id);
        lib_patch_free(&patch);
        return NHP_ENOENT;
    }

    char *diff_path = NULL;
    err = lib_version_diff_path(id, use_version, &diff_path);
    if (err != NHP_OK) {
        lib_patch_free(&patch);
        return err;
    }
    char *data = NULL;
    size_t len = 0;
    err = fs_read_file(diff_path, &data, &len);
    free(diff_path);
    if (err != NHP_OK) {
        lib_patch_free(&patch);
        return err;
    }

    const char *body = skip_existing_header(data);

    buf apply_buf;
    buf_init(&apply_buf);
    buf_append_fmt(&apply_buf, "patch -p1 < %s-%s.diff", patch.name, use_version);
    char *apply_hint = buf_take(&apply_buf);
    char *now = time_now_iso8601();
    mkp_header_info hdr = {
        .kind = "patch",
        .name = patch.name,
        .version = use_version,
        .author = patch.author,
        .nethack_version = patch.target,
        .apply_hint = apply_hint,
        .short_desc = "",
        .long_desc = "",
        .contains_block = "",
    };
    char *header = mkp_format_header(&hdr, (now != NULL) ? now : "");
    free(now);
    free(apply_hint);

    buf full;
    buf_init(&full);
    (void)buf_append_str(&full, header);
    (void)buf_append_str(&full, body);
    free(header);
    char *full_text = buf_take(&full);

    err = fs_write_atomic(out_path, full_text, strlen(full_text));
    free(full_text);
    free(data);
    lib_patch_free(&patch);
    return err;
}
