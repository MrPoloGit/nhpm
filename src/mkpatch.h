/* mkpatch.h - combined patches, capturing changes, patches from a folder
 * or git, ignore rules, header writing, verification.
 * SPDX-License-Identifier: NGPL */
#ifndef NHPM_MKPATCH_H
#define NHPM_MKPATCH_H

#include "instance.h"
#include "util.h"

#include <stddef.h>

/* ---- 12.1: telling real changes apart from build output ---- */

typedef enum mkp_status {
    MKP_MODIFIED,
    MKP_ADDED,
    MKP_DELETED,
    MKP_BINARY, /* differs, but can't be included in a text diff */
} mkp_status;

typedef struct mkp_file {
    char *path; /* relative to the tree, e.g. "src/potion.c" */
    mkp_status status;
    bool default_selected; /* 12.1 rule 3: M/D default on, A only if it
                            * looks like source, B is never selectable;
                            * always false when `hidden` is true */
    bool hidden;           /* matched mkp_is_build_output() -- a guess that
                            * might be wrong, so a caller can reveal it
                            * (12.5's "h") instead of it being dropped */
} mkp_file;

void mkp_file_list_free(mkp_file *files, size_t count);

/* True if `path` is nhpm's/Quilt's own bookkeeping (.pc/, patches/,
 * series, .nhpm/, .git/) -- never a real edit, so a tree walk always
 * excludes it outright, with no "show hidden" override. */
bool mkp_is_structural(const char *path);

/* True if `path` looks like build output nhpm *guesses* to hide
 * (docs/features.md 12.1 rule 1's list, plus `extra_patterns`). A guess
 * that can be wrong, so mkp_compare_trees() tags it `hidden`, not dropped. */
bool mkp_is_build_output(const char *path, const char *nethack_version, const char *extra_patterns);

/* mkp_is_structural() || mkp_is_build_output() -- for a caller that
 * doesn't need the two tiers told apart. */
bool mkp_is_ignored(const char *path, const char *nethack_version, const char *extra_patterns);

/* docs/features.md 12.1 rule 3: whether a *new* file should be ticked by
 * default -- .c/.h/.des/.lua anywhere, or .txt under dat/ or doc/. */
bool mkp_looks_like_source(const char *path);

/* True if `data` (length `len`) has a NUL in its first 8 KB
 * (docs/features.md 12.1 rule 2). */
bool mkp_looks_binary(const void *data, size_t len);

/* Walks `tree_a`/`tree_b` (absolute, unmodified), skipping symlinks/specials
 * and mkp_is_structural() entirely, and classifies every differing file.
 * Build-output is tagged `hidden` (docs/features.md 12.5's "h"), not dropped. */
nhp_err mkp_compare_trees(const char *tree_a, const char *tree_b, const char *nethack_version,
                          const char *extra_ignore, mkp_file **out_files, size_t *out_count);

/* Unified diff of paths[0..count) between tree_a/tree_b, as "diff -Naur"
 * (docs/features.md 12.2 step 3; deterministic, docs/testing.md 17.1).
 * Paths checked with path_is_safe() (docs/security.md). "" if unchanged. */
nhp_err mkp_diff_files(const char *tree_a, const char *tree_b, const char *const *paths,
                       size_t count, char **out_diff);

/* ---- the header block every generated diff starts with (section 12) ---- */

typedef struct mkp_header_info {
    const char *kind; /* "combined patch" or "patch" */
    const char *name;
    const char *version;         /* "" to omit the Version: line */
    const char *author;          /* "" to omit the Author: line */
    const char *nethack_version; /* "" to omit the NetHack: line */
    const char *apply_hint;      /* the Apply: line's value */
    const char *short_desc;      /* "" to omit */
    const char *long_desc;       /* "" to omit */
    const char *contains_block;  /* "" to omit; pre-rendered "Contains, in
                                  * order:\n  1. ...\n" (combined patch) */
} mkp_header_info;

/* Renders the header block (ends in a blank line, ready to prepend to a
 * diff) -- deterministic except Created: (`now`'s ISO 8601 text);
 * everything else is byte-identical per docs/testing.md 17.1. */
char *mkp_format_header(const mkp_header_info *info, const char *now_iso8601);

/* ---- 12.2 / `combined`: one diff of an instance's enabled patches ---- */

typedef struct mkp_combined_result {
    char *diff; /* header + diff text, ready to write out */
    size_t file_count;
} mkp_combined_result;

void mkp_combined_result_free(mkp_combined_result *r);

/* Builds *inst's combined patch (docs/features.md 12.2): clean base + every
 * enabled patch (or `include_ids`), verified with a zero-fuzz dry run.
 * NHP_EIO either way, but distinguishes apply failure from verify failure. */
nhp_err mkp_combined(const inst_instance *inst, const char *const *include_ids,
                     size_t include_count, const char *extra_ignore, mkp_combined_result *out);

/* ---- 12.3 steps 1-2 / `changes`: list uncaptured edits ---- */

/* Lists what's different between *inst's working source and "clean base
 * + every enabled patch" (docs/features.md 12.3 step 1-2) -- the
 * Changes screen's own list, and the backend `changes` command. */
nhp_err mkp_changes(const inst_instance *inst, const char *extra_ignore, mkp_file **out_files,
                    size_t *out_count);

/* Same comparison as mkp_changes(), but a diff of exactly `paths`
 * instead of a classified list (docs/features.md 12.5's "Enter shows
 * the full diff"). `paths` normally comes from an mkp_changes() call. */
nhp_err mkp_changes_diff(const inst_instance *inst, const char *const *paths, size_t count,
                         char **out_diff);

/* ---- metadata shared by capture and mkpatch-from-folder ---- */

typedef struct mkp_patch_meta {
    const char *name;
    const char *version;    /* "" defaults to "1.0" */
    const char *author;     /* "" for unknown */
    const char *target;     /* "" to fall back to the instance's/folder's version */
    const char *short_desc; /* "" for none */
    const char *long_desc;  /* "" for none */
} mkp_patch_meta;

/* ---- 12.3 steps 3-4 / `capture`: save edits as a new patch ---- */

/* Captures `selected_files`' edits as a new patch (12.3): snapshots
 * *inst, writes the diff, pushes it via stack_add(), and saves it to the
 * library as local-<slug>. Restores the snapshot on any later failure. */
nhp_err mkp_capture(inst_instance *inst, const mkp_patch_meta *meta,
                    const char *const *selected_files, size_t count, char **out_id);

/* ---- 12.6 / `mkpatch`: a patch from any modified NetHack folder ---- */

typedef enum mkp_base_kind {
    MKP_BASE_AUTO,    /* an existing base source matching the folder's own version */
    MKP_BASE_FOLDER,  /* another folder of clean source */
    MKP_BASE_TARBALL, /* a tarball of clean source */
    MKP_BASE_GIT_REF, /* `folder` is a git repo; compare against this commit/tag/branch */
} mkp_base_kind;

typedef struct mkp_base_spec {
    mkp_base_kind kind;
    const char *path_or_ref; /* folder/tarball path, or the git ref; "" for MKP_BASE_AUTO */
} mkp_base_spec;

/* Compares `folder` (read-only, never modified) against `base` and
 * saves the result like mkp_capture() does (local-<slug>).
 * `selected_files`/`count` may be NULL/0 to take every default-selected file. */
nhp_err mkp_from_folder(const char *folder, const mkp_base_spec *base, const mkp_patch_meta *meta,
                        const char *extra_ignore, const char *const *selected_files, size_t count,
                        char **out_id);

/* The comparison step alone, for showing the same per-file review as
 * mkp_changes() before committing to mkp_from_folder(). NHP_EINVAL for
 * MKP_BASE_GIT_REF, which never builds an mkp_file list to tick. */
nhp_err mkp_from_folder_changes(const char *folder, const mkp_base_spec *base,
                                const char *extra_ignore, mkp_file **out_files, size_t *out_count);

/* Same comparison as mkp_from_folder_changes(), but a diff of exactly
 * `paths` instead of a classified list (12.5's "Enter shows the full
 * diff"). Same NHP_EINVAL for MKP_BASE_GIT_REF. */
nhp_err mkp_from_folder_diff(const char *folder, const mkp_base_spec *base,
                             const char *const *paths, size_t count, char **out_diff);

/* ---- 12.7 / `export-patch`: write a library patch as a plain .diff ---- */

/* Writes library patch `id`'s stored diff to `out_path`, with a fresh
 * header from the library's own metadata (idempotent either way).
 * `version` may be NULL for the newest downloaded/saved version. */
nhp_err mkp_export_patch(const char *id, const char *version, const char *out_path);

#endif /* NHPM_MKPATCH_H */
