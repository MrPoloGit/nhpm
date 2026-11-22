/* build.h - build recipes, run build, stream output, launch game.
 * SPDX-License-Identifier: NGPL */
#ifndef NHPM_BUILD_H
#define NHPM_BUILD_H

#include "instance.h"
#include "proc.h"
#include "util.h"

/* True if never built, or stack_modified > last_built (ISO 8601, so
 * lexicographic; docs/features.md 10.6's "out of date" indicator).
 * Deliberately not `modified`, which bumps on any save, not just a stack change. */
bool build_is_stale(const inst_instance *inst);

/* Runs the instance's recipe (docs/features.md 10.6), logging to build.log
 * (streaming to `on_line` if set). On success updates last_built; on failure
 * leaves a prior install/ untouched, returns NHP_EIO. "custom" -> NHP_EINVAL (UI/M7). */
nhp_err build_run(inst_instance *inst, proc_line_fn on_line, void *user_data);

/* Runs <install_dir>/games/<game_name> interactively, inheriting the
 * terminal (docs/features.md 13.7 -- caller does def_prog_mode()/endwin()).
 * Updates last_played regardless of exit status. NHP_ENOENT if never built. */
nhp_err build_play(inst_instance *inst);

#endif /* NHPM_BUILD_H */
