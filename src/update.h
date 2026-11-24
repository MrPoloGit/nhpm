/* update.h - checks GitHub for a newer nhpm release (nhpm-plan.md M8).
 * No new dependency: follows the "latest release" URL's redirect (net.c).
 * SPDX-License-Identifier: NGPL */
#ifndef NHPM_UPDATE_H
#define NHPM_UPDATE_H

#include "config.h"
#include "util.h"

/* Compares two nhpm version strings ("0.1", "v0.2.0", a leading "v" is
 * skipped on either side), numeric component by component, missing
 * trailing components treated as 0. Returns <0, 0 or >0 like strcmp. */
int update_version_compare(const char *a, const char *b);

typedef struct update_check_result {
    bool checked;          /* a check actually ran (fresh cache or network);
                            * false if skipped (disabled, or no cache yet) */
    bool update_available; /* only meaningful if checked */
    char *latest_version;  /* owned; NULL if checked is false, or the repo
                            * has no releases yet */
} update_check_result;

void update_check_result_free(update_check_result *r);

/* Reads only the on-disk cache (never touches the network) -- safe to
 * call from a fast path like `nhpm --version`. checked stays false if
 * there's no cache yet or it's older than max_age_days. */
void update_check_cached(long max_age_days, update_check_result *out);

/* Like update_check_cached(), but goes to the network on a cache miss
 * (or always, if `force` -- the CLI's `nhpm update-check`). `checked`
 * stays false if app->check_updates is false and force wasn't passed. */
nhp_err update_check(const cfg_app *app, bool force, update_check_result *out);

#endif /* NHPM_UPDATE_H */
