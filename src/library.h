/* library.h - downloaded patches: add, list, delete versions, verify.
 * SPDX-License-Identifier: NGPL */
#ifndef NHPM_LIBRARY_H
#define NHPM_LIBRARY_H

#include "config.h"
#include "site.h"
#include "util.h"

#include <stddef.h>

typedef struct lib_version {
    char *version;
    char *downloaded; /* ISO 8601 UTC */
    char *url;        /* the URL actually downloaded from */
    bool via_wayback;
    char *sha256;        /* lowercase hex, of the stored patch.diff */
    char *files_touched; /* space-separated paths, best-effort; may be "" */
} lib_version;

typedef struct lib_patch {
    char *id; /* the site's ID ("485") or "local-<slug>" */
    char *name;
    char *author;
    char *target;          /* e.g. "NetHack 3.6.0"; "" for local patches */
    char *source_page;     /* "" for local patches */
    lib_version *versions; /* as found on disk, oldest first */
    size_t version_count;
} lib_patch;

void lib_patch_free(lib_patch *p);

/* Lists every patch in ~/.local/share/nhpm/library, each with all of its
 * downloaded versions. */
nhp_err lib_list(lib_patch **out_patches, size_t *out_count);
void lib_list_free(lib_patch *patches, size_t count);

/* Finds one patch by ID. NHP_ENOENT if it's not in the library. */
nhp_err lib_find(const char *id, lib_patch *out);

/* Fetches patch `id`'s detail page, using (and refreshing, every 7 days) the
 * ~/.cache/nhpm/pages/<id>.html cache (docs/features.md section 7.2). */
nhp_err lib_fetch_detail(const char *id, site_detail *out);

/* True if `data` (length `len`) looks like a patch: a unified, context or
 * plain diff, as opposed to e.g. an HTML error page or an empty response
 * (docs/features.md section 7.4, step 4). */
bool lib_looks_like_diff(const char *data, size_t len);

/* Downloads patch `id` into the library (7.4): follows the download
 * link (Wayback fallback if `app->wayback_fallback`), extracts and
 * saves it as a new version. NHP_EEXIST if existing; NHP_EPARSE if not a diff. */
nhp_err lib_download(const char *id, const cfg_app *app, char **out_version);

/* Imports a local .diff file that isn't on the site. `id` becomes
 * "local-<slug of name>" (the caller owns *out_id). `version` may be NULL
 * for the default "1.0". NHP_EEXIST if that version is already there. */
nhp_err lib_import_local(const char *path, const char *name, const char *author, const char *target,
                         const char *version, char **out_id);

/* docs 12.4: adds a new version to library patch `id` (1.0 -> 1.1); if
 * `id` is new, `name`/`author`/`target` seed patch.meta (`id` must be
 * local-). NHP_ENOENT if missing and not local; NHP_EEXIST if version exists. */
nhp_err lib_add_version(const char *id, const char *name, const char *author, const char *target,
                        const char *version, const char *path, char **out_version);

/* "My Local Patch" -> "local-my-local-patch" (docs 8). Exposed for a
 * caller that needs a brand-new local patch's id before anything is
 * captured yet, e.g. stack_new_patch(). The caller frees the result. */
char *lib_make_local_id(const char *name);

/* docs 12.4's "L": forks a downloaded patch's edited copy into its own
 * library entry (new id "local-<original_id>-<slug>-my", version
 * "<original>-N") so editing it doesn't rewrite history. Caller frees both. */
nhp_err lib_fork_as_local(const char *original_id, const char *original_version, const char *name,
                          const char *author, const char *target, const char *path, char **out_id,
                          char **out_version);

/* Removes one version's folder. Doesn't affect instances that already use
 * it -- they keep their own copy (docs/features.md section 8). */
nhp_err lib_delete_version(const char *id, const char *version);

/* Removes every version of `id`. */
nhp_err lib_delete_patch(const char *id);

/* The absolute path to one version's patch.diff -- for a caller (stack.c
 * via main.c's CLI, or the UI) that needs to copy it into an instance.
 * NHP_ENOENT if that id or version isn't in the library. */
nhp_err lib_version_diff_path(const char *id, const char *version, char **out_path);

/* Re-hashes a version's patch.diff and compares it with the stored sha256. */
nhp_err lib_verify_version(const char *id, const char *version, bool *out_ok);

#endif /* NHPM_LIBRARY_H */
