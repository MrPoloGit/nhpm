/* test_update.c - unit tests for update.c's version compare and cache
 * (no network, no terminal -- docs/testing.md 17.1).
 * SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "update.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                  \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static void test_version_compare(void)
{
    CHECK(update_version_compare("0.1", "0.1") == 0);
    CHECK(update_version_compare("0.1", "0.1.0") == 0); /* missing component reads as 0 */
    CHECK(update_version_compare("v0.1", "0.1") == 0);  /* leading v is skipped on either side */
    CHECK(update_version_compare("0.1", "0.2.0") < 0);
    CHECK(update_version_compare("0.2.0", "0.1") > 0);
    CHECK(update_version_compare("1.0.0", "0.9.9") > 0);  /* major beats minor/patch */
    CHECK(update_version_compare("0.1.9", "0.1.10") < 0); /* numeric, not lexical, compare */
    CHECK(update_version_compare("", "0.0.1") < 0);       /* unparseable reads as all-zero */
}

static void test_cache_round_trip(void)
{
    char *tmp_home = fs_make_temp_dir("nhp-test-xdg");
    CHECK(tmp_home != NULL);
    setenv("XDG_CACHE_HOME", tmp_home, 1);

    /* No cache file yet: cached-only lookup finds nothing, regardless of
     * max_age_days. */
    update_check_result none;
    update_check_cached(3650, &none);
    CHECK(!none.checked);
    update_check_result_free(&none);

    /* Same shape update_check() writes: one line, the latest tag.
     * "v99999.0.0": a dev build's NHPM_VERSION is a git-describe hash
     * whose leading digits parse as a deceptively large "major". */
    char *cache_dir = path_join(tmp_home, "nhpm");
    CHECK(cache_dir != NULL);
    CHECK(fs_mkdir_p(cache_dir, 0700) == NHP_OK);
    char *cache_path = path_join(cache_dir, "update-check");
    CHECK(cache_path != NULL);
    CHECK(fs_write_atomic(cache_path, "v99999.0.0", strlen("v99999.0.0")) == NHP_OK);

    update_check_result fresh;
    update_check_cached(3650, &fresh);
    CHECK(fresh.checked);
    CHECK(fresh.latest_version != NULL && str_eq(fresh.latest_version, "v99999.0.0"));
    CHECK(fresh.update_available);
    update_check_result_free(&fresh);

    /* max_age_days = 0 makes every cache read look too stale to use. */
    update_check_result stale;
    update_check_cached(0, &stale);
    CHECK(!stale.checked);
    update_check_result_free(&stale);

    /* An empty cache file ("no releases found" from a real check) is a
     * valid, checked result with no latest_version and no update. */
    CHECK(fs_write_atomic(cache_path, "", 0) == NHP_OK);
    update_check_result no_releases;
    update_check_cached(3650, &no_releases);
    CHECK(no_releases.checked);
    CHECK(no_releases.latest_version == NULL);
    CHECK(!no_releases.update_available);
    update_check_result_free(&no_releases);

    free(cache_path);
    free(cache_dir);
    unsetenv("XDG_CACHE_HOME");
    fs_remove_tree(tmp_home);
    free(tmp_home);
}

int main(void)
{
    test_version_compare();
    test_cache_round_trip();

    if (failures > 0) {
        (void)fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    (void)printf("ok\n");
    return 0;
}
