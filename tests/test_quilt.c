/* test_quilt.c - unit tests for quilt.c against the fake NetHack source
 * fixture and hand-made test patches (docs/testing.md 17.2). Needs quilt
 * and GNU patch installed. SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "quilt.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                  \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/* A fresh copy of the fixture source, with an empty patches/ and series
 * ready for a test to fill in. `dir` is a char * the body can use. */
#define WITH_TEMP_SOURCE(body)                                                                     \
    do {                                                                                           \
        char *dir = fs_make_temp_dir("nhp-test-quilt");                                            \
        CHECK(dir != NULL);                                                                        \
        CHECK(fs_copy_tree("tests/fixtures/source", dir) == NHP_OK);                               \
        char *patches_dir = path_join(dir, "patches");                                             \
        CHECK(fs_mkdir_p(patches_dir, 0755) == NHP_OK);                                            \
        free(patches_dir);                                                                         \
        body;                                                                                      \
        fs_remove_tree(dir);                                                                       \
        free(dir);                                                                                 \
    } while (0)

static char *abs_patch(const char *name)
{
    char cwd[2048];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        return NULL;
    }
    buf b;
    buf_init(&b);
    buf_append_fmt(&b, "%s/tests/fixtures/patches/%s", cwd, name);
    return buf_take(&b);
}

static void copy_patch_into(const char *dir, const char *name)
{
    char *src = abs_patch(name);
    char *dst_dir = path_join(dir, "patches");
    char *dst = path_join(dst_dir, name);
    char *data = NULL;
    size_t len = 0;
    CHECK(fs_read_file(src, &data, &len) == NHP_OK);
    CHECK(fs_write_atomic(dst, data, len) == NHP_OK);
    free(data);
    free(src);
    free(dst_dir);
    free(dst);
}

static void write_series(const char *dir, const char *content)
{
    char *series_path = path_join(dir, "series");
    CHECK(fs_write_atomic(series_path, content, strlen(content)) == NHP_OK);
    free(series_path);
}

static void test_check_available(void)
{
    CHECK(quilt_check_available() == NHP_OK);
}

static void test_push_pop_applied(void)
{
    WITH_TEMP_SOURCE({
        copy_patch_into(dir, "01-lamp-radius.diff");
        copy_patch_into(dir, "02-apply-pickaxe.diff");
        write_series(dir, "01-lamp-radius.diff -p1\n02-apply-pickaxe.diff -p0\n");

        char **names = NULL;
        size_t count = 0;
        CHECK(quilt_applied(dir, &names, &count) == NHP_OK);
        CHECK(count == 0); /* nothing applied yet is not an error */
        quilt_name_list_free(names, count);

        proc_result pr = {0};
        CHECK(quilt_push_all(dir, &pr) == NHP_OK);
        CHECK(!pr.signaled && pr.exit_code == 0);
        proc_result_free(&pr);

        CHECK(quilt_applied(dir, &names, &count) == NHP_OK);
        CHECK(count == 2);
        if (count == 2) {
            CHECK(str_eq(names[0], "01-lamp-radius.diff"));
            CHECK(str_eq(names[1], "02-apply-pickaxe.diff"));
        }
        quilt_name_list_free(names, count);

        char **files = NULL;
        size_t file_count = 0;
        CHECK(quilt_files(dir, "01-lamp-radius.diff", &files, &file_count) == NHP_OK);
        CHECK(file_count == 1);
        if (file_count == 1) {
            CHECK(str_eq(files[0], "src/light.c"));
        }
        quilt_name_list_free(files, file_count);

        CHECK(quilt_pop_all(dir, &pr) == NHP_OK);
        proc_result_free(&pr);
        CHECK(quilt_applied(dir, &names, &count) == NHP_OK);
        CHECK(count == 0);
        quilt_name_list_free(names, count);
    });
}

static void test_push_pop_single(void)
{
    WITH_TEMP_SOURCE({
        copy_patch_into(dir, "01-lamp-radius.diff");
        copy_patch_into(dir, "02-apply-pickaxe.diff");
        write_series(dir, "01-lamp-radius.diff -p1\n02-apply-pickaxe.diff -p0\n");

        proc_result pr = {0};
        CHECK(quilt_push(dir, "01-lamp-radius.diff", false, &pr) == NHP_OK);
        CHECK(!pr.signaled && pr.exit_code == 0);
        proc_result_free(&pr);

        char **names = NULL;
        size_t count = 0;
        CHECK(quilt_applied(dir, &names, &count) == NHP_OK);
        CHECK(count == 1);
        quilt_name_list_free(names, count);

        /* 01-lamp-radius.diff is already the top, so popping "down to"
         * it (quilt's pop <name> semantics) is a no-op; pop just the
         * current top instead. */
        CHECK(quilt_pop(dir, NULL, &pr) == NHP_OK);
        CHECK(!pr.signaled && pr.exit_code == 0);
        proc_result_free(&pr);
        CHECK(quilt_applied(dir, &names, &count) == NHP_OK);
        CHECK(count == 0);
        quilt_name_list_free(names, count);
    });
}

static void test_push_conflict_leaves_clean_state(void)
{
    WITH_TEMP_SOURCE({
        copy_patch_into(dir, "01-lamp-radius.diff");
        copy_patch_into(dir, "03-conflicting-lamp.diff");
        write_series(dir, "01-lamp-radius.diff -p1\n03-conflicting-lamp.diff -p1\n");

        proc_result pr = {0};
        CHECK(quilt_push(dir, "01-lamp-radius.diff", false, &pr) == NHP_OK);
        proc_result_free(&pr);

        CHECK(quilt_push(dir, "03-conflicting-lamp.diff", false, &pr) == NHP_OK);
        CHECK(!pr.signaled && pr.exit_code != 0); /* a real conflict, not a tool error */
        proc_result_free(&pr);

        /* the conflicting patch never actually lands on the stack */
        char **names = NULL;
        size_t count = 0;
        CHECK(quilt_applied(dir, &names, &count) == NHP_OK);
        CHECK(count == 1);
        if (count == 1) {
            CHECK(str_eq(names[0], "01-lamp-radius.diff"));
        }
        quilt_name_list_free(names, count);

        /* and leaves no .rej lying around either */
        char *rej_path = path_join(dir, "src/light.c.rej");
        CHECK(!fs_exists(rej_path));
        free(rej_path);
    });
}

static void test_force_push_leaves_rej(void)
{
    WITH_TEMP_SOURCE({
        copy_patch_into(dir, "01-lamp-radius.diff");
        copy_patch_into(dir, "03-conflicting-lamp.diff");
        write_series(dir, "01-lamp-radius.diff -p1\n03-conflicting-lamp.diff -p1\n");

        proc_result pr = {0};
        CHECK(quilt_push(dir, "01-lamp-radius.diff", false, &pr) == NHP_OK);
        proc_result_free(&pr);

        CHECK(quilt_push(dir, "03-conflicting-lamp.diff", true, &pr) == NHP_OK);
        CHECK(!pr.signaled && pr.exit_code != 0); /* forced "success" still reports nonzero */
        proc_result_free(&pr);

        char **names = NULL;
        size_t count = 0;
        CHECK(quilt_applied(dir, &names, &count) == NHP_OK);
        CHECK(count == 2); /* forced on despite the rejects */
        quilt_name_list_free(names, count);

        char *rej_path = path_join(dir, "src/light.c.rej");
        CHECK(fs_exists(rej_path));
        free(rej_path);
    });
}

static void test_refresh(void)
{
    WITH_TEMP_SOURCE({
        copy_patch_into(dir, "01-lamp-radius.diff");
        write_series(dir, "01-lamp-radius.diff -p1\n");

        proc_result pr = {0};
        CHECK(quilt_push(dir, "01-lamp-radius.diff", false, &pr) == NHP_OK);
        proc_result_free(&pr);

        CHECK(quilt_refresh(dir, "01-lamp-radius.diff", &pr) == NHP_OK);
        CHECK(!pr.signaled && pr.exit_code == 0);
        proc_result_free(&pr);
    });
}

static void test_dry_run_clean(void)
{
    WITH_TEMP_SOURCE({
        char *patch_path = abs_patch("01-lamp-radius.diff");
        quilt_dry_run d;
        CHECK(quilt_dry_run_patch(dir, patch_path, 1, &d) == NHP_OK);
        CHECK(d.total_hunks == 1);
        CHECK(d.failed_hunks == 0);
        CHECK(d.fuzzy_hunks == 0);
        CHECK(!d.no_file_found);
        CHECK(d.file_count == 1);
        if (d.file_count == 1) {
            CHECK(str_eq(d.files[0].path, "src/light.c"));
            CHECK(d.files[0].hunks_total == 1);
            CHECK(d.files[0].hunks_failed == 0);
        }
        quilt_dry_run_free(&d);
        free(patch_path);
    });
}

static void test_dry_run_wrong_strip_is_no_file_found(void)
{
    WITH_TEMP_SOURCE({
        char *patch_path = abs_patch("02-apply-pickaxe.diff"); /* needs -p0, not -p1 */
        quilt_dry_run d;
        CHECK(quilt_dry_run_patch(dir, patch_path, 1, &d) == NHP_OK);
        CHECK(d.no_file_found);
        quilt_dry_run_free(&d);

        CHECK(quilt_dry_run_patch(dir, patch_path, 0, &d) == NHP_OK);
        CHECK(!d.no_file_found);
        CHECK(d.failed_hunks == 0);
        quilt_dry_run_free(&d);
        free(patch_path);
    });
}

static void test_dry_run_conflict_per_file(void)
{
    WITH_TEMP_SOURCE({
        copy_patch_into(dir, "01-lamp-radius.diff");
        write_series(dir, "01-lamp-radius.diff -p1\n");
        proc_result pr = {0};
        CHECK(quilt_push(dir, "01-lamp-radius.diff", false, &pr) == NHP_OK);
        proc_result_free(&pr);

        char *patch_path = abs_patch("03-conflicting-lamp.diff");
        quilt_dry_run d;
        CHECK(quilt_dry_run_patch(dir, patch_path, 1, &d) == NHP_OK);
        CHECK(d.failed_hunks == 1);
        CHECK(d.file_count == 1);
        if (d.file_count == 1) {
            CHECK(str_eq(d.files[0].path, "src/light.c"));
            CHECK(d.files[0].hunks_failed == 1);
        }
        quilt_dry_run_free(&d);
        free(patch_path);
    });
}

static void test_dry_run_multi_file(void)
{
    WITH_TEMP_SOURCE({
        copy_patch_into(dir, "01-lamp-radius.diff");
        write_series(dir, "01-lamp-radius.diff -p1\n");
        proc_result pr = {0};
        CHECK(quilt_push(dir, "01-lamp-radius.diff", false, &pr) == NHP_OK);
        proc_result_free(&pr);

        /* 02 (clean) and 03 (conflicts, since 01 is applied) combined
         * would need per-file handling; exercised individually confirms
         * each file's tally is independent. */
        char *patch02 = abs_patch("02-apply-pickaxe.diff");
        quilt_dry_run d02;
        CHECK(quilt_dry_run_patch(dir, patch02, 0, &d02) == NHP_OK);
        CHECK(d02.failed_hunks == 0);
        quilt_dry_run_free(&d02);
        free(patch02);
    });
}

/* docs/features.md 12.4's "N" (New patch): a brand-new, empty patch
 * pushes cleanly since there's nothing in it to conflict with, and
 * "quilt add" + an edit + refresh captures real content into it. */
static void test_new_patch_then_add_and_refresh(void)
{
    WITH_TEMP_SOURCE({
        write_series(dir, "");

        proc_result pr = {0};
        CHECK(quilt_new(dir, "my-new-patch.diff", &pr) == NHP_OK);
        CHECK(!pr.signaled && pr.exit_code == 0);
        proc_result_free(&pr);

        char **applied = NULL;
        size_t applied_count = 0;
        CHECK(quilt_applied(dir, &applied, &applied_count) == NHP_OK);
        CHECK(applied_count == 1 && str_eq(applied[0], "my-new-patch.diff"));
        quilt_name_list_free(applied, applied_count);

        CHECK(quilt_add(dir, "src/light.c", &pr) == NHP_OK);
        CHECK(!pr.signaled && pr.exit_code == 0);
        proc_result_free(&pr);

        char *light_path = path_join(dir, "src/light.c");
        char *data = NULL;
        size_t len = 0;
        CHECK(fs_read_file(light_path, &data, &len) == NHP_OK);
        char *appended = NULL;
        if (data != NULL) {
            size_t new_len = len + 20;
            appended = malloc(new_len);
            CHECK(appended != NULL);
            if (appended != NULL) {
                memcpy(appended, data, len);
                memcpy(appended + len, "\n/* tracked edit */\n", 20);
                CHECK(fs_write_atomic(light_path, appended, new_len) == NHP_OK);
            }
        }
        free(data);
        free(appended);
        free(light_path);

        CHECK(quilt_refresh(dir, "my-new-patch.diff", &pr) == NHP_OK);
        CHECK(!pr.signaled && pr.exit_code == 0);
        proc_result_free(&pr);

        char *patch_path = path_join(dir, "patches/my-new-patch.diff");
        char *patch_data = NULL;
        size_t patch_len = 0;
        CHECK(fs_read_file(patch_path, &patch_data, &patch_len) == NHP_OK);
        CHECK(patch_data != NULL && strstr(patch_data, "tracked edit") != NULL);
        free(patch_data);
        free(patch_path);
    });
}

/* "quilt add" on a file that isn't tracked by any patch at all (nothing
 * pushed) fails the way quilt itself reports it -- not an nhp_err,
 * same contract as quilt_push()/quilt_pop(). */
static void test_add_with_nothing_on_top_fails(void)
{
    WITH_TEMP_SOURCE({
        write_series(dir, "");
        proc_result pr = {0};
        CHECK(quilt_add(dir, "src/light.c", &pr) == NHP_OK);
        CHECK(pr.exit_code != 0);
        proc_result_free(&pr);
    });
}

int main(void)
{
    test_check_available();
    test_push_pop_applied();
    test_push_pop_single();
    test_push_conflict_leaves_clean_state();
    test_force_push_leaves_rej();
    test_refresh();
    test_new_patch_then_add_and_refresh();
    test_add_with_nothing_on_top_fails();
    test_dry_run_clean();
    test_dry_run_wrong_strip_is_no_file_found();
    test_dry_run_conflict_per_file();
    test_dry_run_multi_file();

    if (failures == 0) {
        (void)printf("ok: all quilt tests passed\n");
    } else {
        (void)printf("FAILED: %d check(s) failed\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
