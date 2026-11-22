/* instance.h - instances: create, load, save, clone, rename, delete,
 * snapshots, export/import, lock.
 * SPDX-License-Identifier: NGPL */
#ifndef NHPM_INSTANCE_H
#define NHPM_INSTANCE_H

#include "util.h"

#include <stddef.h>
#include <sys/types.h>

typedef enum inst_kind {
    INST_MANAGED,
    INST_LINKED,
} inst_kind;

/* One entry in the patch stack. Enabling/disabling/reordering (and keeping
 * this in sync with Quilt) is stack.c's job (M4); instance.c just reads and
 * writes this as data. */
typedef struct inst_patch {
    char *id;
    char *version;
    bool enabled;
    int strip;     /* -1 if not yet determined */
    char *sha256;  /* of the instance's copy; "" if unknown */
    bool modified; /* hand-fixed after a conflict, docs/features.md 10.2 */
} inst_patch;

typedef struct inst_instance {
    char *name;
    inst_kind kind;
    char *source;         /* base source version, e.g. "3.6.7" */
    char *source_path;    /* "src" (relative) for managed; an absolute path for linked */
    char *created;        /* ISO 8601 UTC */
    char *modified;       /* bumped on every save, for any reason -- not what
                           * "is the build out of date" means (docs/features.md
                           * section 10.6); that's stack_modified below */
    char *stack_modified; /* ISO 8601 UTC; bumped only by stack.c (M4)'s
                           * add/remove/move/enable/disable, never by any
                           * other save. "" if the stack never changed. */
    char *last_built;     /* "" if never built */
    char *last_played;    /* "" if never played */
    char *notes;

    char *recipe;      /* build recipe name, e.g. "nethack-3.6-linux" (section 10.6) */
    char *make_vars;   /* extra `make` variables, e.g. "CFLAGS+=-O2"; "" if none */
    char *install_dir; /* relative to the instance dir; default "install" */
    char *game_name;   /* default "nethack" */

    inst_patch *patches; /* in stack order */
    size_t patch_count;
} inst_instance;

void inst_free(inst_instance *inst);

/* Frees an inst_patch's own fields (not the struct itself -- for a single
 * element of a `patches` array, e.g. one stack.c (M4) is building up
 * before committing it to an inst_instance). */
void inst_patch_free_contents(inst_patch *p);

/* Deep-copies one inst_patch. NHP_ENOMEM leaves *dst zeroed. */
nhp_err inst_patch_copy(const inst_patch *src, inst_patch *dst);

/* The instance's own directory: ~/.local/share/nhpm/instances/<name>.
 * The caller frees it; NULL on OOM. Doesn't check that it exists. */
char *inst_dir(const char *name);

/* Lists every instance. */
nhp_err inst_list(inst_instance **out_instances, size_t *out_count);
void inst_list_free(inst_instance *instances, size_t count);

/* Loads one instance's instance.conf. NHP_ENOENT if there's no such
 * instance. */
nhp_err inst_load(const char *name, inst_instance *out);

/* Saves *inst (updating its `modified` timestamp first): copies the
 * current instance.conf to instance.conf.bak, then writes the new one
 * atomically (docs/features.md section 10.3). */
nhp_err inst_save(inst_instance *inst);

/* Creates a managed instance: copies base source `source_version`'s tree
 * into <instance>/src, sets up its dirs, writes an empty-patch-list
 * instance.conf. NHP_EEXIST/NHP_ENOENT as expected; `recipe` "" -> "custom". */
nhp_err inst_new(const char *name, const char *source_version, const char *recipe,
                 inst_instance *out);

/* Creates a new linked instance pointing at the existing folder `folder`
 * (its version is read the same way as sources.c: patchlevel.h, falling
 * back to "unknown"). nhpm never deletes or recreates `folder`. */
nhp_err inst_link(const char *name, const char *folder, const char *recipe, inst_instance *out);

/* Copies the whole instance under a new name (source, patches, settings).
 * If `copy_saves`, the install/ folder (save games) is copied too. */
nhp_err inst_clone(const char *src_name, const char *new_name, bool copy_saves);

/* Renames the instance's folder and updates instance.conf. */
nhp_err inst_rename(const char *old_name, const char *new_name);

/* Deletes the instance. If `keep_saves`, install/ is moved to
 * ~/.local/share/nhpm/orphaned-saves/<name>-<date>/ first; otherwise
 * it's deleted along with everything else. */
nhp_err inst_delete(const char *name, bool keep_saves);

typedef struct inst_snapshot {
    char *filename; /* e.g. "2026-11-15T20-14-03-before-building.conf" */
    char *label;    /* "" if none was given */
    char *created;  /* ISO 8601 UTC */
} inst_snapshot;

void inst_snapshot_free(inst_snapshot *s);

/* Saves a copy of the instance's current instance.conf into snapshots/,
 * named from the current time (and `label`, if given). */
nhp_err inst_snapshot_create(const char *name, const char *label, char **out_filename);

nhp_err inst_snapshot_list(const char *name, inst_snapshot **out_snapshots, size_t *out_count);
void inst_snapshot_list_free(inst_snapshot *snapshots, size_t count);

nhp_err inst_snapshot_delete(const char *name, const char *filename);

/* Restores instance.conf's data from a snapshot, after taking a fresh one
 * first (so this is undoable). Doesn't sync the Quilt stack -- that's
 * stack.c's job (M4) and isn't done here yet. */
nhp_err inst_snapshot_restore(const char *name, const char *filename);

/* Writes `out_path` as a .tar.gz holding instance.conf and the contents of
 * the instance's src/patches/ directory (not the NetHack source itself,
 * not save games -- docs/features.md section 10.5). */
nhp_err inst_export(const char *name, const char *out_path);

/* Reads a .nhinst (inst_export()) into a new managed instance `new_name`,
 * using base source `source_version` or the exported one if NULL/"" --
 * it must already exist. Caller still pushes patches via stack.c. */
nhp_err inst_import(const char *nhinst_path, const char *new_name, const char *source_version,
                    inst_instance *out);

/* The instance's source folder, as an absolute path: `source_path` itself
 * for a linked instance, or inst_dir(name)/source_path for a managed one.
 * The caller frees it; NULL on OOM. Doesn't check that it exists. */
char *inst_source_full_path(const inst_instance *inst);

/* A structural load-time check (10.4): does the source folder exist
 * where instance.conf says it should. Does NOT compare the patch list
 * against the actual Quilt stack/series -- that's stack.c's job. */
typedef struct inst_check_result {
    bool source_path_exists;
    bool ok;
} inst_check_result;

nhp_err inst_check(const inst_instance *inst, inst_check_result *out);

/* Finds `id`'s current position in inst->patches. Indices shift after
 * every add/remove/move, so callers look this up fresh rather than
 * caching one. NHP_ENOENT if there's no patch with that id. */
nhp_err inst_find_patch_index(const inst_instance *inst, const char *id, size_t *out_index);

/* flock()-based locking (docs/features.md section 10.8): one nhpm at a
 * time holds an instance open for changes. */
typedef struct inst_lock {
    int fd;
} inst_lock;

/* NHP_ELOCKED (with *out_other_pid set, if known) if another nhpm
 * already holds the lock. */
nhp_err inst_lock_acquire(const char *name, inst_lock *out, pid_t *out_other_pid);
void inst_lock_release(inst_lock *lock);

#endif /* NHPM_INSTANCE_H */
