/* stack.h - enable/disable/reorder with rollback, sync with instance.conf.
 * SPDX-License-Identifier: NGPL */
#ifndef NHPM_STACK_H
#define NHPM_STACK_H

#include "instance.h"
#include "util.h"

#include <stddef.h>

/* One file a conflicting patch touches (docs/features.md section 11.6's
 * conflict dialog). */
typedef struct stack_conflict_file {
    char *path;
    int hunks_total;
    int hunks_failed;
} stack_conflict_file;

typedef struct stack_conflict {
    char *patch_id; /* the patch that actually failed -- not always the one
                     * the caller asked to enable; a later patch can depend
                     * on it (docs 11.5, "enabling Pk breaks Pm") */
    char *patch_version;
    bool is_requested_patch; /* false if this is a later patch Pm, not Pk */
    stack_conflict_file *files;
    size_t file_count;
    char *quilt_output; /* raw output from the failing push, for a "view"
                         * action beyond the per-file hunk counts */
} stack_conflict;

void stack_conflict_free(stack_conflict *c);

/* Enables the patch at `index` (docs 11.5's "Enable Pk"): dry-runs it, then
 * re-pushes the whole stack. On success *inst is updated and saved; on
 * conflict nothing changes and *out_conflict is filled in (caller frees it). */
nhp_err stack_enable(inst_instance *inst, size_t index, stack_conflict *out_conflict);

/* Disables the patch at `index` (docs section 11.5's "Disable Pk"): same
 * all-or-nothing contract as stack_enable(). A no-op if already
 * disabled. */
nhp_err stack_disable(inst_instance *inst, size_t index, stack_conflict *out_conflict);

/* Moves the patch at `from` to position `to` (both 0-based indices into
 * inst->patches) and re-pushes; same all-or-nothing contract
 * (docs section 11.5's "Move"). */
nhp_err stack_move(inst_instance *inst, size_t from, size_t to, stack_conflict *out_conflict);

/* Copies `library_patch_path` into <src>/patches/, adds a disabled entry to
 * inst->patches, then calls stack_enable() if `enable` (docs 11.5's "Add
 * from library"). A failed enable leaves it added but disabled either way. */
nhp_err stack_add(inst_instance *inst, const char *id, const char *version, const char *name,
                  const char *library_patch_path, const char *sha256, bool enable,
                  size_t *out_new_index, stack_conflict *out_conflict);

/* Disables the patch at `index` first if enabled (same contract as
 * stack_disable(), can fail with NHP_ECONFLICT), then deletes its file
 * and instance.conf entry (docs 11.5's "Remove"). */
nhp_err stack_remove(inst_instance *inst, size_t index, stack_conflict *out_conflict);

/* docs 11.6's Force action: snapshots, then force-pushes the patch at
 * `index`, leaving failed hunks as `.rej` files listed in *out_conflict
 * for the caller to edit (docs 13.7), then call stack_force_finish(). */
nhp_err stack_force_begin(inst_instance *inst, size_t index, stack_conflict *out_conflict);

/* "quilt refresh" on the patch stack_force_begin() forced on, marks it
 * enabled = true and modified = true, and saves instance.conf. */
nhp_err stack_force_finish(inst_instance *inst, size_t index);

/* docs 10.4: whether *inst's source folder matches instance.conf (series
 * file, "quilt applied" output, and patch sha256s all agree). `detail` is
 * a "" (never NULL) summary of whatever doesn't, even a missing file. */
typedef struct stack_drift {
    bool series_matches;
    bool quilt_applied_matches;
    bool hashes_match;
    bool ok;
    char *detail;
} stack_drift;

nhp_err stack_check_consistency(const inst_instance *inst, stack_drift *out);
void stack_drift_free(stack_drift *d);

/* docs 10.4's "Restore": rewrites `series` from inst->patches and
 * re-pushes, so the folder matches instance.conf exactly. On conflict it
 * rolls back to its prior (drifted) state; *inst itself is never touched. */
nhp_err stack_restore(const inst_instance *inst, stack_conflict *out_conflict);

/* docs 10.4's "Adopt": the reverse of Restore -- updates *inst (and saves
 * instance.conf) from the folder's actual state (enabled flags, sha256,
 * strip). A patch whose file is gone is dropped from *inst entirely. */
nhp_err stack_adopt(inst_instance *inst);

/* docs 12.4's "E" (Edit patch): snapshots, then pops down to and including
 * the patch at `index`, leaving it on top so its file can be edited by
 * hand or via $EDITOR. NHP_EINVAL if the patch isn't enabled. */
nhp_err stack_edit_begin(inst_instance *inst, size_t index, stack_conflict *out_conflict);

/* The "R" (refresh) half: "quilt refresh"es the patch at `index` and
 * re-pushes everything above it. On success its sha256/.modified are
 * updated; *out_diff_path (caller frees) goes to lib_add_version(). */
nhp_err stack_edit_refresh(inst_instance *inst, size_t index, stack_conflict *out_conflict,
                           char **out_diff_path);

/* docs 12.4's "N" (New patch): snapshots, then "quilt new"s an empty
 * patch, appends an enabled entry to inst->patches, and saves
 * instance.conf. *out_index is its index; NHP_EEXIST if it exists. */
nhp_err stack_new_patch(inst_instance *inst, const char *id, const char *name, size_t *out_index);

/* docs 12.4's "files opened with o are tracked automatically": runs
 * "quilt add" on `path` so a later stack_edit_refresh() picks up a
 * brand-new file's changes. Pure quilt bookkeeping; doesn't touch *inst. */
nhp_err stack_track_file(const inst_instance *inst, const char *path);

/* One disabled patch's dry-run result from stack_check_disabled() below.
 * `index` is into inst->patches; `strip` is the level used (the patch's
 * own, or detected on the spot if it never had one). */
typedef struct stack_check_result {
    size_t index;
    int strip;
    int hunks_total;
    int hunks_failed;
} stack_check_result;

void stack_check_result_free(stack_check_result *results);

/* docs 11.7's "c": read-only dry-run of every disabled patch against the
 * current tree. *out_results (caller frees) has one entry per disabled
 * patch, in patch-list order; *out_count is 0 if there are none. */
nhp_err stack_check_disabled(const inst_instance *inst, stack_check_result **out_results,
                             size_t *out_count);

/* One file touched by more than one enabled patch (docs 11.9). `path` is
 * relative to the source tree; `patch_ids` has `patch_count` entries,
 * into inst->patches' ids, in patch-list order. */
typedef struct stack_shared_file {
    char *path;
    char **patch_ids;
    size_t patch_count;
} stack_shared_file;

void stack_shared_file_free(stack_shared_file *f);
void stack_shared_files_free(stack_shared_file *files, size_t count);

/* docs 11.9: cross-references every enabled patch's library
 * `files_touched` and reports files more than one lists; best-effort,
 * read-only. *out_files has one entry per shared file; 0 if there are none. */
nhp_err stack_find_shared_files(const inst_instance *inst, stack_shared_file **out_files,
                                size_t *out_count);

/* docs 11.8's "u": restores the most recent "auto-"-labeled snapshot and
 * re-syncs the Quilt folder to match. On success *inst is reloaded.
 * NHP_ENOENT if there's no such snapshot. */
nhp_err stack_undo(inst_instance *inst, stack_conflict *out_conflict);

#endif /* NHPM_STACK_H */
