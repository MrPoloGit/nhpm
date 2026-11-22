/* sources.h - base sources: import from folder/tarball/git, verify, remove.
 * SPDX-License-Identifier: NGPL */
#ifndef NHPM_SOURCES_H
#define NHPM_SOURCES_H

#include "net.h"
#include "util.h"
#include "version.h"

#include <stddef.h>

typedef struct bsrc_source {
    char *version; /* directory name under sources/, e.g. "3.6.7" */
    char *origin;  /* "folder:<path>", "tarball:<path>", or "git:<url>#<tag>" */
    char *added;   /* ISO 8601 UTC */
    char *sha256;  /* of the original tarball, if any; "" otherwise */
    bool is_variant;
} bsrc_source;

void bsrc_source_free(bsrc_source *s);

/* Lists every base source in ~/.local/share/nhpm/sources. */
nhp_err bsrc_list(bsrc_source **out_sources, size_t *out_count);
void bsrc_list_free(bsrc_source *sources, size_t count);

/* Finds one base source by version. NHP_ENOENT if it's not there. */
nhp_err bsrc_find(const char *version, bsrc_source *out);

/* The read-only source tree's path, i.e. sources/<version>/tree -- what a
 * managed instance's src/ gets copied from. The caller frees it. NULL on
 * OOM; does not check that the source actually exists. */
char *bsrc_tree_path(const char *version);

/* Copies `folder` into sources/<version>/tree (read-only), reading the
 * version from <folder>/include/patchlevel.h, or `version_hint` if that
 * fails. NHP_EEXIST if that version exists (*out_version still set). */
nhp_err bsrc_add_from_folder(const char *folder, const char *version_hint, char **out_version);

/* Extracts `tarball_path` (path-checking every entry first, per
 * docs/security.md) to a temporary folder, then behaves like
 * bsrc_add_from_folder() on the result. */
nhp_err bsrc_add_from_tarball(const char *tarball_path, const char *version_hint,
                              char **out_version);

/* Checks every entry in `tarball_path` is safe (no absolute paths, no
 * ".." components), then extracts into `dest_dir`. Exposed for
 * mkpatch.c's tarball base (12.6), which needs the check without importing it. */
nhp_err bsrc_extract_tarball_checked(const char *tarball_path, const char *dest_dir);

/* `git clone --depth 1 --branch <tag>
 * https://github.com/NetHack/NetHack`, then behaves like
 * bsrc_add_from_folder() on the result (docs/features.md section 9.2). */
nhp_err bsrc_add_from_git(const char *tag, char **out_version);

/* Removes the base source for `version`. Whether it's in use is for the
 * caller to check first (9.5: allowed after confirmation). Instances keep
 * their own copy, so this never breaks an existing one. */
nhp_err bsrc_remove(const char *version);

/* ---- known versions: a hand-verified catalog of download URLs/git
 * tags (9.2), separate from the manual add-from-folder/tarball/git flow.
 * Deliberately short -- extending it means verifying a new row, not guessing. */

typedef enum bsrc_known_kind {
    BSRC_KNOWN_TARBALL, /* `location` is a tarball URL */
    BSRC_KNOWN_GIT,     /* `location` is a tag in the official NetHack/NetHack repo */
} bsrc_known_kind;

typedef struct bsrc_known_version {
    const char *version;
    bsrc_known_kind kind;
    const char *location;
} bsrc_known_version;

extern const bsrc_known_version BSRC_KNOWN_VERSIONS[];
extern const size_t BSRC_KNOWN_VERSION_COUNT;

/* Fetches `entry` (tarball to a temp file, or a direct git clone) and
 * registers it as a base source, behaving like bsrc_add_from_tarball()/
 * bsrc_add_from_git(). `progress`/`user_data` go to net_download(); ignored for git. */
nhp_err bsrc_add_known(const bsrc_known_version *entry, net_progress_fn progress, void *user_data,
                       char **out_version);

#endif /* NHPM_SOURCES_H */
