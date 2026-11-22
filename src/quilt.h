/* quilt.h - run quilt/patch as subprocesses, parse output.
 * SPDX-License-Identifier: NGPL */
#ifndef NHPM_QUILT_H
#define NHPM_QUILT_H

#include "proc.h"
#include "util.h"

#include <stddef.h>

/* Checks that `quilt` and a real GNU patch are reachable on PATH.
 * NHP_ENOENT (with an err_set() message) if either isn't. macOS's
 * bundled `patch` isn't GNU patch, so `gpatch` is also accepted. */
nhp_err quilt_check_available(void);

/* "quilt applied" in `src_dir`: currently applied patches' filenames
 * (QUILT_PATCHES prefix stripped), bottom of the stack first. Nothing
 * applied yet is not an error -- *out_count is just 0. */
nhp_err quilt_applied(const char *src_dir, char ***out_names, size_t *out_count);

/* "quilt files <name>", the files that patch touches, relative to
 * `src_dir`. */
nhp_err quilt_files(const char *src_dir, const char *name, char ***out_files, size_t *out_count);

void quilt_name_list_free(char **names, size_t count);

/* "quilt push <name>" (NULL for the next patch in series; "-f" if
 * `force`). *out is always filled in; a conflict is a nonzero
 * out->exit_code, not a nhp_err -- NHP_EIO only if quilt couldn't run. */
nhp_err quilt_push(const char *src_dir, const char *name, bool force, proc_result *out);

/* "quilt push -a". */
nhp_err quilt_push_all(const char *src_dir, proc_result *out);

/* "quilt pop <name>" (NULL for just the top patch): pops down to and
 * including whatever is above `name`, leaving `name` as the new top. If
 * `name` is already the top, quilt reports failure ("No patch removed"). */
nhp_err quilt_pop(const char *src_dir, const char *name, proc_result *out);

/* "quilt pop -a". */
nhp_err quilt_pop_all(const char *src_dir, proc_result *out);

/* "quilt refresh <name>". */
nhp_err quilt_refresh(const char *src_dir, const char *name, proc_result *out);

/* "quilt new <name>": creates a brand-new, empty patch named `name`
 * (docs/features.md 12.4's "N") and pushes it on top of the stack --
 * trivially, since there's nothing in it yet to conflict with. */
nhp_err quilt_new(const char *src_dir, const char *name, proc_result *out);

/* "quilt add <file>": snapshots it into quilt's .pc/ bookkeeping under
 * the current top patch, so a later "quilt refresh" picks up changes to
 * it. Only needed for a file not already part of that patch's own diff. */
nhp_err quilt_add(const char *src_dir, const char *file, proc_result *out);

/* One target file's hunk tally within a dry run (docs/features.md section
 * 11.6's conflict dialog shows one line like this per file). */
typedef struct quilt_dry_run_file {
    char *path;
    int hunks_total;
    int hunks_failed; /* includes hunks ignored for want of the file itself */
    bool no_file_found;
} quilt_dry_run_file;

typedef struct quilt_dry_run {
    int total_hunks;
    int failed_hunks;
    int fuzzy_hunks;
    bool no_file_found; /* every file: "can't find file to patch" -- this
                         * strip level is entirely wrong, not a conflict
                         * (docs/features.md section 11.4) */
    char *raw_output;
    quilt_dry_run_file *files;
    size_t file_count;
} quilt_dry_run;

/* Not a quilt command: runs "patch --dry-run --batch --force --verbose"
 * directly and parses the per-hunk lines nhpm cares about (11.4);
 * --verbose is needed so total_hunks counts cleanly-applied hunks too. */
nhp_err quilt_dry_run_patch(const char *src_dir, const char *patch_path, int strip,
                            quilt_dry_run *out);
void quilt_dry_run_free(quilt_dry_run *d);

#endif /* NHPM_QUILT_H */
