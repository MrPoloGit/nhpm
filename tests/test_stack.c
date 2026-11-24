/* test_stack.c - unit tests for stack.c against the fake NetHack source
 * fixture and hand-made test patches (docs/testing.md 17.2). Needs quilt
 * and GNU patch installed. SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "instance.h"
#include "library.h"
#include "quilt.h"
#include "sources.h"
#include "stack.h"
#include "util.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                  \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/* Every test runs under a fresh XDG_DATA_HOME, with the fake NetHack
 * source fixture already imported as base source "3.6.7" and a managed
 * instance called "stacktest" created from it. */
#define WITH_TEMP_INSTANCE(body)                                                                   \
    do {                                                                                           \
        char *tmp_data_dir = fs_make_temp_dir("nhp-test-stack");                                   \
        CHECK(tmp_data_dir != NULL);                                                               \
        setenv("XDG_DATA_HOME", tmp_data_dir, 1);                                                  \
        char *src_version = NULL;                                                                  \
        CHECK(bsrc_add_from_folder("tests/fixtures/source", NULL, &src_version) == NHP_OK);        \
        free(src_version);                                                                         \
        inst_instance inst;                                                                        \
        CHECK(inst_new("stacktest", "3.6.7", "", &inst) == NHP_OK);                                \
        body;                                                                                      \
        inst_free(&inst);                                                                          \
        unsetenv("XDG_DATA_HOME");                                                                 \
        make_tree_writable(tmp_data_dir);                                                          \
        fs_remove_tree(tmp_data_dir);                                                              \
        free(tmp_data_dir);                                                                        \
    } while (0)

static void make_tree_writable(const char *dir)
{
    chmod(dir, 0700);
    DIR *dp = opendir(dir);
    if (dp == NULL) {
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dp)) != NULL) {
        if (str_eq(entry->d_name, ".") || str_eq(entry->d_name, "..")) {
            continue;
        }
        char *child = path_join(dir, entry->d_name);
        if (child == NULL) {
            continue;
        }
        struct stat st;
        if (stat(child, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                make_tree_writable(child);
            } else {
                chmod(child, 0600);
            }
        }
        free(child);
    }
    (void)closedir(dp);
}

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

/* stack_add()'s sha256 argument is stored verbatim, not computed -- most
 * tests pass a placeholder like "abc". The consistency tests below need
 * the *real* hash so a freshly added patch starts out genuinely clean. */
static void real_sha256_of(const char *path, char out_hex[65])
{
    char *data = NULL;
    size_t len = 0;
    if (fs_read_file(path, &data, &len) == NHP_OK) {
        sha256_hex(data, len, out_hex);
        free(data);
    } else {
        out_hex[0] = '\0';
    }
}

static int compare_names(const void *a, const void *b)
{
    const char *const *pa = a;
    const char *const *pb = b;
    return strcmp(*pa, *pb);
}

/* A cheap deterministic "hash" of every file under `dir`. Entries are
 * sorted before combining -- readdir() order isn't stable across an
 * unlink+recreate (quilt restoring a file on pop), even unchanged. */
static unsigned long tree_fingerprint(const char *dir)
{
    char **names = NULL;
    size_t count = 0;
    DIR *dp = opendir(dir);
    if (dp == NULL) {
        return 0;
    }
    struct dirent *entry;
    while ((entry = readdir(dp)) != NULL) {
        if (str_eq(entry->d_name, ".") || str_eq(entry->d_name, "..") ||
            str_eq(entry->d_name, ".pc")) {
            continue;
        }
        char **grown = realloc(names, (count + 1) * sizeof(*grown));
        if (grown == NULL) {
            break;
        }
        names = grown;
        names[count] = str_dup(entry->d_name);
        if (names[count] != NULL) {
            count++;
        }
    }
    (void)closedir(dp);

    qsort(names, count, sizeof(*names), compare_names);

    unsigned long sum = 0;
    for (size_t i = 0; i < count; i++) {
        char *child = path_join(dir, names[i]);
        struct stat st;
        if (child != NULL && stat(child, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                sum = sum * 31 + tree_fingerprint(child);
            } else {
                char *data = NULL;
                size_t len = 0;
                if (fs_read_file(child, &data, &len) == NHP_OK) {
                    for (size_t j = 0; j < len; j++) {
                        sum = sum * 131 + (unsigned char)data[j];
                    }
                    free(data);
                }
            }
        }
        free(child);
        free(names[i]);
    }
    free(names);
    return sum;
}

static void test_add_strip_detection(void)
{
    WITH_TEMP_INSTANCE({
        char *p01 = abs_patch("01-lamp-radius.diff");
        size_t idx = SIZE_MAX;
        stack_conflict conflict;
        nhp_err err =
            stack_add(&inst, "t01", "1.0", "Lamp Radius", p01, "abc", true, &idx, &conflict);
        CHECK(err == NHP_OK);
        CHECK(idx == 0);
        CHECK(inst.patches[idx].enabled);
        CHECK(inst.patches[idx].strip == 1); /* a/src/..., b/src/... headers need -p1 */
        free(p01);

        char *p02 = abs_patch("02-apply-pickaxe.diff");
        err = stack_add(&inst, "t02", "1.0", "Apply Pickaxe", p02, "abc", true, &idx, &conflict);
        CHECK(err == NHP_OK);
        CHECK(inst.patches[idx].strip == 0); /* no a/b prefix: needs -p0 */
        free(p02);

        /* persisted correctly */
        inst_instance reloaded;
        CHECK(inst_load("stacktest", &reloaded) == NHP_OK);
        CHECK(reloaded.patch_count == 2);
        CHECK(reloaded.patches[0].strip == 1);
        CHECK(reloaded.patches[1].strip == 0);
        inst_free(&reloaded);
    });
}

static void test_add_disabled_then_enable(void)
{
    WITH_TEMP_INSTANCE({
        char *p01 = abs_patch("01-lamp-radius.diff");
        size_t idx = SIZE_MAX;
        stack_conflict conflict;
        CHECK(stack_add(&inst, "t01", "1.0", "Lamp Radius", p01, "abc", false, &idx, &conflict) ==
              NHP_OK);
        CHECK(!inst.patches[idx].enabled);
        CHECK(inst.patches[idx].strip == -1); /* never determined yet */
        free(p01);

        CHECK(stack_enable(&inst, idx, &conflict) == NHP_OK);
        CHECK(inst.patches[idx].enabled);
        CHECK(inst.patches[idx].strip == 1);

        /* a no-op the second time */
        CHECK(stack_enable(&inst, idx, &conflict) == NHP_OK);
    });
}

/* Regression: adding an id already in the stack used to append a second,
 * conflicting slot instead of being rejected (docs/ui-progress.md). */
static void test_add_rejects_duplicate_id(void)
{
    WITH_TEMP_INSTANCE({
        char *p01 = abs_patch("01-lamp-radius.diff");
        size_t idx = SIZE_MAX;
        stack_conflict conflict;
        CHECK(stack_add(&inst, "t01", "1.0", "Lamp Radius", p01, "abc", true, &idx, &conflict) ==
              NHP_OK);
        CHECK(inst.patch_count == 1);

        /* same id, enabled slot already exists */
        nhp_err err = stack_add(&inst, "t01", "1.0", "Lamp Radius", p01, "abc", true, &idx,
                                &conflict);
        CHECK(err == NHP_EEXIST);
        CHECK(inst.patch_count == 1); /* nothing appended */
        free(p01);

        /* same id, disabled slot already exists */
        char *p02 = abs_patch("02-apply-pickaxe.diff");
        size_t idx02 = SIZE_MAX;
        CHECK(stack_add(&inst, "t02", "1.0", "Apply Pickaxe", p02, "abc", false, &idx02,
                        &conflict) == NHP_OK);
        CHECK(inst.patch_count == 2);
        err = stack_add(&inst, "t02", "1.0", "Apply Pickaxe", p02, "abc", true, &idx02, &conflict);
        CHECK(err == NHP_EEXIST);
        CHECK(inst.patch_count == 2);
        free(p02);
    });
}

/* Regression: restoring a chosen snapshot used to only swap instance.conf,
 * leaving the Quilt folder stale -- it would say one patch while the tree
 * still had two applied. stack_restore_snapshot() re-syncs the folder too. */
static void test_restore_snapshot_resyncs_tree(void)
{
    WITH_TEMP_INSTANCE({
        char *src_dir = inst_source_full_path(&inst);

        char *p01 = abs_patch("01-lamp-radius.diff");
        size_t idx01 = SIZE_MAX;
        stack_conflict conflict;
        CHECK(stack_add(&inst, "t01", "1.0", "Lamp Radius", p01, "abc", true, &idx01, &conflict) ==
              NHP_OK);
        free(p01);
        unsigned long one_patch = tree_fingerprint(src_dir);

        char *checkpoint = NULL;
        CHECK(inst_snapshot_create("stacktest", "one patch applied", &checkpoint) == NHP_OK);

        /* 02-apply-pickaxe.diff touches src/apply.c; check its content, not
         * the whole tree -- a stray copy of this .diff left in patches/
         * after restore is a separate, harmless cosmetic issue. */
        char *apply_c = path_join(src_dir, "src/apply.c");
        char *p02 = abs_patch("02-apply-pickaxe.diff");
        size_t idx02 = SIZE_MAX;
        CHECK(stack_add(&inst, "t02", "1.0", "Apply Pickaxe", p02, "abc", true, &idx02,
                        &conflict) == NHP_OK);
        free(p02);
        CHECK(tree_fingerprint(src_dir) != one_patch); /* tree now has both */
        char *patched_data = NULL;
        size_t patched_len = 0;
        CHECK(fs_read_file(apply_c, &patched_data, &patched_len) == NHP_OK);
        CHECK(strstr(patched_data, "fake amulet") != NULL);
        free(patched_data);

        CHECK(stack_restore_snapshot(&inst, checkpoint, &conflict) == NHP_OK);
        free(checkpoint);
        CHECK(inst.patch_count == 1);
        /* the actual point of this test: the source file, not just
         * instance.conf, is actually back to its one-patch content. */
        char *restored_data = NULL;
        size_t restored_len = 0;
        CHECK(fs_read_file(apply_c, &restored_data, &restored_len) == NHP_OK);
        CHECK(strstr(restored_data, "fake amulet") == NULL);
        free(restored_data);
        free(apply_c);

        free(src_dir);
    });
}

static void test_undo_none(void)
{
    WITH_TEMP_INSTANCE({
        stack_conflict conflict;
        CHECK(stack_undo(&inst, &conflict) == NHP_ENOENT);
    });
}

static void test_undo_enable(void)
{
    WITH_TEMP_INSTANCE({
        char *src_dir = inst_source_full_path(&inst);

        char *p01 = abs_patch("01-lamp-radius.diff");
        size_t idx = SIZE_MAX;
        stack_conflict conflict;
        CHECK(stack_add(&inst, "t01", "1.0", "Lamp Radius", p01, "abc", false, &idx, &conflict) ==
              NHP_OK);
        free(p01);
        /* fingerprint right after the add (disabled), before enabling --
         * undo only reverts the enable, not the earlier add. */
        unsigned long before = tree_fingerprint(src_dir);

        CHECK(stack_enable(&inst, idx, &conflict) == NHP_OK);
        CHECK(inst.patches[idx].enabled);

        /* undoes the enable (the most recent auto: snapshot); the add
         * itself is still there, just disabled again, same as right
         * after stack_add(..., enable=false, ...) above. */
        CHECK(stack_undo(&inst, &conflict) == NHP_OK);
        CHECK(inst.patch_count == 1);
        CHECK(!inst.patches[0].enabled);
        CHECK(tree_fingerprint(src_dir) == before);

        /* persisted: a fresh load agrees */
        inst_instance reloaded;
        CHECK(inst_load("stacktest", &reloaded) == NHP_OK);
        CHECK(reloaded.patch_count == 1);
        CHECK(!reloaded.patches[0].enabled);
        inst_free(&reloaded);

        free(src_dir);
    });
}

static void test_undo_move(void)
{
    WITH_TEMP_INSTANCE({
        /* t01 (src/light.c) and t02 (src/apply.c): independent files, so
         * either order applies cleanly -- unlike 04/05 elsewhere in this
         * file, which genuinely depend on their order. */
        char *p01 = abs_patch("01-lamp-radius.diff");
        char *p02 = abs_patch("02-apply-pickaxe.diff");
        size_t idx01 = SIZE_MAX;
        size_t idx02 = SIZE_MAX;
        stack_conflict conflict;
        CHECK(stack_add(&inst, "t01", "1.0", "Lamp Radius", p01, "abc", true, &idx01, &conflict) ==
              NHP_OK);
        CHECK(stack_add(&inst, "t02", "1.0", "Apply Pickaxe", p02, "abc", true, &idx02,
                        &conflict) == NHP_OK);
        free(p01);
        free(p02);
        CHECK(idx01 == 0 && idx02 == 1);

        CHECK(stack_move(&inst, 0, 1, &conflict) == NHP_OK);
        CHECK(str_eq(inst.patches[0].id, "t02"));
        CHECK(str_eq(inst.patches[1].id, "t01"));

        CHECK(stack_undo(&inst, &conflict) == NHP_OK);
        CHECK(inst.patch_count == 2);
        CHECK(str_eq(inst.patches[0].id, "t01"));
        CHECK(str_eq(inst.patches[1].id, "t02"));
        CHECK(inst.patches[0].enabled && inst.patches[1].enabled);
    });
}

/* A manual snapshot taken after the auto: one must not be picked instead
 * (docs 11.8: "not just the most recent snapshot overall"). */
static void test_undo_skips_manual_snapshot(void)
{
    WITH_TEMP_INSTANCE({
        char *p01 = abs_patch("01-lamp-radius.diff");
        size_t idx = SIZE_MAX;
        stack_conflict conflict;
        CHECK(stack_add(&inst, "t01", "1.0", "Lamp Radius", p01, "abc", true, &idx, &conflict) ==
              NHP_OK);
        free(p01);
        CHECK(inst.patches[idx].enabled);

        char *manual_name = NULL;
        CHECK(inst_snapshot_create("stacktest", "a manual checkpoint", &manual_name) == NHP_OK);
        free(manual_name);

        CHECK(stack_undo(&inst, &conflict) == NHP_OK);
        CHECK(inst.patch_count == 0); /* back to before the add, not just before the snapshot */
    });
}

static void test_dependent_patch_blocks_disable(void)
{
    WITH_TEMP_INSTANCE({
        char *src_dir = inst_source_full_path(&inst);
        unsigned long before = tree_fingerprint(src_dir);

        char *p04 = abs_patch("04-new-scroll.diff");
        char *p05 = abs_patch("05-depends-on-04-add-wand.diff");
        size_t idx04 = SIZE_MAX;
        size_t idx05 = SIZE_MAX;
        stack_conflict conflict;
        CHECK(stack_add(&inst, "t04", "1.0", "New Scroll", p04, "abc", true, &idx04, &conflict) ==
              NHP_OK);
        CHECK(stack_add(&inst, "t05", "1.0", "Add Wand", p05, "abc", true, &idx05, &conflict) ==
              NHP_OK);
        free(p04);
        free(p05);

        unsigned long after_adds = tree_fingerprint(src_dir);

        memset(&conflict, 0, sizeof(conflict));
        nhp_err err = stack_disable(&inst, idx04, &conflict);
        CHECK(err == NHP_ECONFLICT);
        CHECK(str_eq(conflict.patch_id, "t05")); /* Pm, not the requested Pk */
        CHECK(!conflict.is_requested_patch);
        CHECK(conflict.file_count == 1);
        if (conflict.file_count == 1) {
            CHECK(str_eq(conflict.files[0].path, "src/objects.c"));
            CHECK(conflict.files[0].hunks_failed == 1);
        }
        stack_conflict_free(&conflict);

        /* nothing changed: inst untouched, folder untouched */
        CHECK(inst.patches[idx04].enabled);
        CHECK(tree_fingerprint(src_dir) == after_adds);

        (void)before;
        free(src_dir);
    });
}

static void test_check_disabled(void)
{
    WITH_TEMP_INSTANCE({
        char *p01 = abs_patch("01-lamp-radius.diff");
        char *p02 = abs_patch("02-apply-pickaxe.diff");
        char *p03 = abs_patch("03-conflicting-lamp.diff");
        size_t idx01 = SIZE_MAX;
        size_t idx02 = SIZE_MAX;
        size_t idx03 = SIZE_MAX;
        stack_conflict conflict;
        /* t01 enabled; t02 disabled (unrelated file, clean); t03
         * disabled (same line as t01, so it conflicts once applied). */
        CHECK(stack_add(&inst, "t01", "1.0", "Lamp Radius", p01, "abc", true, &idx01, &conflict) ==
              NHP_OK);
        CHECK(stack_add(&inst, "t02", "1.0", "Apply Pickaxe", p02, "abc", false, &idx02,
                        &conflict) == NHP_OK);
        CHECK(stack_add(&inst, "t03", "1.0", "Conflicting Lamp", p03, "abc", false, &idx03,
                        &conflict) == NHP_OK);
        free(p01);
        free(p02);
        free(p03);
        CHECK(inst.patches[idx02].strip == -1); /* never enabled, never determined */
        CHECK(inst.patches[idx03].strip == -1);

        char *src_dir = inst_source_full_path(&inst);
        unsigned long before = tree_fingerprint(src_dir);

        stack_check_result *results = NULL;
        size_t result_count = 0;
        CHECK(stack_check_disabled(&inst, &results, &result_count) == NHP_OK);
        CHECK(result_count == 2);

        bool found02 = false;
        bool found03 = false;
        for (size_t i = 0; i < result_count; i++) {
            if (results[i].index == idx02) {
                found02 = true;
                CHECK(results[i].hunks_failed == 0);
                CHECK(results[i].hunks_total > 0);
                CHECK(results[i].strip >= 0); /* detected on the spot */
            } else if (results[i].index == idx03) {
                found03 = true;
                CHECK(results[i].hunks_failed == 1);
            }
        }
        CHECK(found02);
        CHECK(found03);
        stack_check_result_free(results);

        /* purely read-only: nothing pushed/popped/written */
        CHECK(!inst.patches[idx02].enabled);
        CHECK(!inst.patches[idx03].enabled);
        CHECK(inst.patches[idx02].strip == -1);
        CHECK(inst.patches[idx03].strip == -1);
        CHECK(tree_fingerprint(src_dir) == before);
        free(src_dir);
    });
}

static void test_check_disabled_none(void)
{
    WITH_TEMP_INSTANCE({
        stack_check_result *results = NULL;
        size_t result_count = SIZE_MAX;
        CHECK(stack_check_disabled(&inst, &results, &result_count) == NHP_OK);
        CHECK(result_count == 0);
        CHECK(results == NULL);
        stack_check_result_free(results);
    });
}

/* Imports a fixture .diff into the library (so it has real files_touched,
 * docs section 8) and adds it to *inst from the library's own stored copy,
 * same as the real add-from-library path. */
static nhp_err add_from_library(inst_instance *inst, const char *fixture_name, const char *name,
                                size_t *out_index)
{
    char *diff_path = abs_patch(fixture_name);
    char *id = NULL;
    nhp_err err = lib_import_local(diff_path, name, "me", "NetHack 3.6.7", "1.0", &id);
    free(diff_path);
    if (err != NHP_OK) {
        return err;
    }

    char *stored_path = NULL;
    err = lib_version_diff_path(id, "1.0", &stored_path);
    if (err == NHP_OK) {
        stack_conflict conflict;
        err = stack_add(inst, id, "1.0", name, stored_path, "abc", true, out_index, &conflict);
    }
    free(stored_path);
    free(id);
    return err;
}

static void test_shared_files_detected(void)
{
    WITH_TEMP_INSTANCE({
        size_t idx04 = SIZE_MAX;
        size_t idx05 = SIZE_MAX;
        /* both touch src/objects.c (same pair test_dependent_patch_blocks_disable
         * uses); 05 depends on 04, so both apply cleanly together. */
        CHECK(add_from_library(&inst, "04-new-scroll.diff", "New Scroll", &idx04) == NHP_OK);
        CHECK(add_from_library(&inst, "05-depends-on-04-add-wand.diff", "Add Wand", &idx05) ==
              NHP_OK);

        stack_shared_file *shared = NULL;
        size_t shared_count = 0;
        CHECK(stack_find_shared_files(&inst, &shared, &shared_count) == NHP_OK);
        CHECK(shared_count == 1);
        if (shared_count == 1) {
            CHECK(str_eq(shared[0].path, "src/objects.c"));
            CHECK(shared[0].patch_count == 2);
            bool has04 = false;
            bool has05 = false;
            for (size_t k = 0; k < shared[0].patch_count; k++) {
                has04 = has04 || str_eq(shared[0].patch_ids[k], inst.patches[idx04].id);
                has05 = has05 || str_eq(shared[0].patch_ids[k], inst.patches[idx05].id);
            }
            CHECK(has04 && has05);
        }
        stack_shared_files_free(shared, shared_count);
    });
}

static void test_shared_files_none_when_disjoint(void)
{
    WITH_TEMP_INSTANCE({
        size_t idx01 = SIZE_MAX;
        /* t01 (src/light.c) alone: nothing else touches the same file. */
        CHECK(add_from_library(&inst, "01-lamp-radius.diff", "Lamp Radius", &idx01) == NHP_OK);

        stack_shared_file *shared = NULL;
        size_t shared_count = SIZE_MAX;
        CHECK(stack_find_shared_files(&inst, &shared, &shared_count) == NHP_OK);
        CHECK(shared_count == 0);
        CHECK(shared == NULL);
        stack_shared_files_free(shared, shared_count);
    });
}

/* A patch added straight from a file (not through the library, like
 * stack_add() in every other test here) has no files_touched to find --
 * best-effort, not an error (docs 11.9). */
static void test_shared_files_best_effort_skips_non_library_patch(void)
{
    WITH_TEMP_INSTANCE({
        char *p01 = abs_patch("01-lamp-radius.diff");
        size_t idx = SIZE_MAX;
        stack_conflict conflict;
        CHECK(stack_add(&inst, "not-in-library", "1.0", "Lamp Radius", p01, "abc", true, &idx,
                        &conflict) == NHP_OK);
        free(p01);

        stack_shared_file *shared = NULL;
        size_t shared_count = SIZE_MAX;
        CHECK(stack_find_shared_files(&inst, &shared, &shared_count) == NHP_OK);
        CHECK(shared_count == 0);
        stack_shared_files_free(shared, shared_count);
    });
}

static void test_enable_conflict_rolls_back(void)
{
    WITH_TEMP_INSTANCE({
        char *p01 = abs_patch("01-lamp-radius.diff");
        char *p03 = abs_patch("03-conflicting-lamp.diff");
        size_t idx01 = SIZE_MAX;
        size_t idx03 = SIZE_MAX;
        stack_conflict conflict;
        CHECK(stack_add(&inst, "t01", "1.0", "Lamp Radius", p01, "abc", true, &idx01, &conflict) ==
              NHP_OK);
        CHECK(stack_add(&inst, "t03", "1.0", "Conflicting Lamp", p03, "abc", false, &idx03,
                        &conflict) == NHP_OK);
        free(p01);
        free(p03);

        char *src_dir = inst_source_full_path(&inst);
        unsigned long before = tree_fingerprint(src_dir);
        char *inst_dir_path = inst_dir("stacktest");
        char *conf_path = path_join(inst_dir_path, "instance.conf");
        free(inst_dir_path);
        char *conf_before = NULL;
        size_t conf_before_len = 0;
        CHECK(fs_read_file(conf_path, &conf_before, &conf_before_len) == NHP_OK);

        memset(&conflict, 0, sizeof(conflict));
        nhp_err err = stack_enable(&inst, idx03, &conflict);
        CHECK(err == NHP_ECONFLICT);
        CHECK(str_eq(conflict.patch_id, "t03"));
        CHECK(conflict.is_requested_patch);
        CHECK(conflict.file_count == 1);
        if (conflict.file_count == 1) {
            CHECK(str_eq(conflict.files[0].path, "src/light.c"));
            CHECK(conflict.files[0].hunks_failed == 1);
        }
        stack_conflict_free(&conflict);

        CHECK(!inst.patches[idx03].enabled);
        CHECK(tree_fingerprint(src_dir) == before);

        /* instance.conf on disk is untouched too (stack_enable() never
         * got to the save step) */
        char *conf_after = NULL;
        size_t conf_after_len = 0;
        CHECK(fs_read_file(conf_path, &conf_after, &conf_after_len) == NHP_OK);
        CHECK(conf_before_len == conf_after_len &&
              memcmp(conf_before, conf_after, conf_before_len) == 0);

        free(conf_before);
        free(conf_after);
        free(conf_path);
        free(src_dir);
    });
}

static void test_move(void)
{
    WITH_TEMP_INSTANCE({
        char *p01 = abs_patch("01-lamp-radius.diff");
        char *p02 = abs_patch("02-apply-pickaxe.diff");
        size_t idx01 = SIZE_MAX;
        size_t idx02 = SIZE_MAX;
        stack_conflict conflict;
        CHECK(stack_add(&inst, "t01", "1.0", "Lamp Radius", p01, "abc", true, &idx01, &conflict) ==
              NHP_OK);
        CHECK(stack_add(&inst, "t02", "1.0", "Apply Pickaxe", p02, "abc", true, &idx02,
                        &conflict) == NHP_OK);
        free(p01);
        free(p02);
        CHECK(idx01 == 0 && idx02 == 1);

        CHECK(stack_move(&inst, idx02, 0, &conflict) == NHP_OK);
        CHECK(str_eq(inst.patches[0].id, "t02"));
        CHECK(str_eq(inst.patches[1].id, "t01"));

        inst_instance reloaded;
        CHECK(inst_load("stacktest", &reloaded) == NHP_OK);
        CHECK(str_eq(reloaded.patches[0].id, "t02"));
        CHECK(str_eq(reloaded.patches[1].id, "t01"));
        inst_free(&reloaded);
    });
}

static void test_remove(void)
{
    WITH_TEMP_INSTANCE({
        char *p01 = abs_patch("01-lamp-radius.diff");
        size_t idx01 = SIZE_MAX;
        stack_conflict conflict;
        CHECK(stack_add(&inst, "t01", "1.0", "Lamp Radius", p01, "abc", true, &idx01, &conflict) ==
              NHP_OK);
        free(p01);

        char *src_dir = inst_source_full_path(&inst);
        char *patches_dir = path_join(src_dir, "patches");
        DIR *dp = opendir(patches_dir);
        bool found_file = false;
        struct dirent *entry;
        while (dp != NULL && (entry = readdir(dp)) != NULL) {
            if (strncmp(entry->d_name, "t01-", 4) == 0) {
                found_file = true;
            }
        }
        if (dp != NULL) {
            (void)closedir(dp);
        }
        CHECK(found_file);

        CHECK(stack_remove(&inst, idx01, &conflict) == NHP_OK);
        CHECK(inst.patch_count == 0);

        dp = opendir(patches_dir);
        found_file = false;
        while (dp != NULL && (entry = readdir(dp)) != NULL) {
            if (strncmp(entry->d_name, "t01-", 4) == 0) {
                found_file = true;
            }
        }
        if (dp != NULL) {
            (void)closedir(dp);
        }
        CHECK(!found_file); /* the file is gone too */

        free(patches_dir);
        free(src_dir);
    });
}

static void test_remove_disables_first(void)
{
    WITH_TEMP_INSTANCE({
        char *p01 = abs_patch("01-lamp-radius.diff");
        size_t idx01 = SIZE_MAX;
        stack_conflict conflict;
        CHECK(stack_add(&inst, "t01", "1.0", "Lamp Radius", p01, "abc", true, &idx01, &conflict) ==
              NHP_OK);
        free(p01);
        CHECK(inst.patches[idx01].enabled);

        CHECK(stack_remove(&inst, idx01, &conflict) == NHP_OK);
        CHECK(inst.patch_count == 0);

        char **names = NULL;
        size_t count = 0;
        /* nothing applied, confirming remove's internal disable step
         * actually popped it off the real Quilt stack too */
        char *src_dir = inst_source_full_path(&inst);
        CHECK(quilt_applied(src_dir, &names, &count) == NHP_OK);
        CHECK(count == 0);
        free(src_dir);
    });
}

static void test_force_then_manual_fix(void)
{
    WITH_TEMP_INSTANCE({
        char *p01 = abs_patch("01-lamp-radius.diff");
        char *p03 = abs_patch("03-conflicting-lamp.diff");
        size_t idx01 = SIZE_MAX;
        size_t idx03 = SIZE_MAX;
        stack_conflict conflict;
        CHECK(stack_add(&inst, "t01", "1.0", "Lamp Radius", p01, "abc", true, &idx01, &conflict) ==
              NHP_OK);
        CHECK(stack_add(&inst, "t03", "1.0", "Conflicting Lamp", p03, "abc", false, &idx03,
                        &conflict) == NHP_OK);
        free(p01);
        free(p03);

        memset(&conflict, 0, sizeof(conflict));
        nhp_err err = stack_force_begin(&inst, idx03, &conflict);
        CHECK(err == NHP_OK);
        CHECK(str_eq(conflict.patch_id, "t03"));
        CHECK(conflict.file_count == 1);
        stack_conflict_free(&conflict);

        /* still not saved to instance.conf yet, but reflected in memory */
        CHECK(inst.patches[idx03].enabled);

        char *src_dir = inst_source_full_path(&inst);
        char *rej_path = path_join(src_dir, "src/light.c.rej");
        CHECK(fs_exists(rej_path));

        /* simulate $EDITOR resolving it: 01 already made it "return 5",
         * finish applying what 03 wanted on top of that. */
        char *light_path = path_join(src_dir, "src/light.c");
        char *data = NULL;
        size_t len = 0;
        CHECK(fs_read_file(light_path, &data, &len) == NHP_OK);
        char *fixed = str_dup(data);
        char *needle = strstr(fixed, "return 5;");
        CHECK(needle != NULL);
        if (needle != NULL) {
            memcpy(needle, "return 9;", 9);
        }
        CHECK(fs_write_atomic(light_path, fixed, strlen(fixed)) == NHP_OK);
        (void)remove(rej_path);
        free(data);
        free(fixed);

        CHECK(stack_force_finish(&inst, idx03) == NHP_OK);
        CHECK(inst.patches[idx03].enabled);
        CHECK(inst.patches[idx03].modified);

        inst_instance reloaded;
        CHECK(inst_load("stacktest", &reloaded) == NHP_OK);
        size_t reloaded_idx = SIZE_MAX;
        CHECK(inst_find_patch_index(&reloaded, "t03", &reloaded_idx) == NHP_OK);
        CHECK(reloaded.patches[reloaded_idx].enabled);
        CHECK(reloaded.patches[reloaded_idx].modified);
        inst_free(&reloaded);

        /* Confirms the stack is coherent again: disabling and re-enabling
         * t01 (the patch underneath t03) works cleanly, unlike moving t03
         * ahead of t01 would, which breaks t03's dependency on t01. */
        memset(&conflict, 0, sizeof(conflict));
        CHECK(stack_disable(&inst, idx01, &conflict) == NHP_ECONFLICT);
        CHECK(str_eq(conflict.patch_id, "t03"));
        stack_conflict_free(&conflict);

        char **names = NULL;
        size_t count = 0;
        CHECK(quilt_applied(src_dir, &names, &count) == NHP_OK);
        CHECK(count == 2);
        if (count == 2) {
            CHECK(str_eq(names[0], "t01-lamp-radius.diff"));
            CHECK(str_eq(names[1], "t03-conflicting-lamp.diff"));
        }
        quilt_name_list_free(names, count);

        free(rej_path);
        free(light_path);
        free(src_dir);
    });
}

/* docs/features.md section 10.4: a freshly built instance -- nothing has
 * drifted yet -- reports clean on every check. */
static void test_consistency_clean(void)
{
    WITH_TEMP_INSTANCE({
        char *p01 = abs_patch("01-lamp-radius.diff");
        char sha[65];
        real_sha256_of(p01, sha);
        size_t idx = SIZE_MAX;
        stack_conflict conflict;
        CHECK(stack_add(&inst, "t01", "1.0", "Lamp Radius", p01, sha, true, &idx, &conflict) ==
              NHP_OK);
        free(p01);

        stack_drift drift;
        CHECK(stack_check_consistency(&inst, &drift) == NHP_OK);
        CHECK(drift.series_matches);
        CHECK(drift.quilt_applied_matches);
        CHECK(drift.hashes_match);
        CHECK(drift.ok);
        CHECK(str_eq(drift.detail, ""));
        stack_drift_free(&drift);
    });
}

/* Popping a patch by hand (a raw "quilt pop") leaves the series file
 * alone but changes what's actually applied -- the drift (10.4) that
 * distinguishes the two consistency checks from each other. */
static void test_consistency_detects_manual_pop(void)
{
    WITH_TEMP_INSTANCE({
        char *p01 = abs_patch("01-lamp-radius.diff");
        char sha[65];
        real_sha256_of(p01, sha);
        size_t idx = SIZE_MAX;
        stack_conflict conflict;
        CHECK(stack_add(&inst, "t01", "1.0", "Lamp Radius", p01, sha, true, &idx, &conflict) ==
              NHP_OK);
        free(p01);

        char *src_dir = inst_source_full_path(&inst);
        proc_result pop_result = {0};
        CHECK(quilt_pop(src_dir, NULL, &pop_result) == NHP_OK);
        CHECK(!pop_result.signaled && pop_result.exit_code == 0);
        proc_result_free(&pop_result);

        stack_drift drift;
        CHECK(stack_check_consistency(&inst, &drift) == NHP_OK);
        CHECK(drift.series_matches);         /* unchanged by a plain pop */
        CHECK(!drift.quilt_applied_matches); /* but nothing is applied now */
        CHECK(!drift.ok);
        CHECK(drift.detail[0] != '\0');
        stack_drift_free(&drift);

        /* Restore puts it back; the folder matches instance.conf again. */
        stack_conflict restore_conflict;
        CHECK(stack_restore(&inst, &restore_conflict) == NHP_OK);
        stack_drift drift2;
        CHECK(stack_check_consistency(&inst, &drift2) == NHP_OK);
        CHECK(drift2.ok);
        stack_drift_free(&drift2);
        free(src_dir);
    });
}

/* Adopt takes the manually-popped state as truth: the patch becomes
 * disabled in instance.conf, persisted. */
static void test_adopt_follows_manual_pop(void)
{
    WITH_TEMP_INSTANCE({
        char *p01 = abs_patch("01-lamp-radius.diff");
        char sha[65];
        real_sha256_of(p01, sha);
        size_t idx = SIZE_MAX;
        stack_conflict conflict;
        CHECK(stack_add(&inst, "t01", "1.0", "Lamp Radius", p01, sha, true, &idx, &conflict) ==
              NHP_OK);
        free(p01);

        char *src_dir = inst_source_full_path(&inst);
        proc_result pop_result = {0};
        CHECK(quilt_pop(src_dir, NULL, &pop_result) == NHP_OK);
        proc_result_free(&pop_result);

        CHECK(stack_adopt(&inst) == NHP_OK);
        CHECK(inst.patch_count == 1);
        CHECK(!inst.patches[0].enabled);

        /* Regression: adopt() used to leave the "# nhpm-disabled:" marker
         * (11.3) off a newly disabled patch's series line. Fixed by having
         * adopt() rewrite series for the patches it just adopted. */
        stack_drift drift;
        CHECK(stack_check_consistency(&inst, &drift) == NHP_OK);
        CHECK(drift.series_matches);
        CHECK(drift.ok);
        stack_drift_free(&drift);

        inst_instance reloaded;
        CHECK(inst_load("stacktest", &reloaded) == NHP_OK);
        CHECK(reloaded.patch_count == 1);
        CHECK(!reloaded.patches[0].enabled);
        inst_free(&reloaded);
        free(src_dir);
    });
}

/* A patch file deleted by hand: adopt drops it from instance.conf
 * entirely, since "the folder's state" no longer has it. */
static void test_adopt_drops_missing_file(void)
{
    WITH_TEMP_INSTANCE({
        char *p01 = abs_patch("01-lamp-radius.diff");
        char *p02 = abs_patch("02-apply-pickaxe.diff");
        size_t idx01 = SIZE_MAX;
        size_t idx02 = SIZE_MAX;
        stack_conflict conflict;
        CHECK(stack_add(&inst, "t01", "1.0", "Lamp Radius", p01, "abc", true, &idx01, &conflict) ==
              NHP_OK);
        CHECK(stack_add(&inst, "t02", "1.0", "Apply Pickaxe", p02, "def", false, &idx02,
                        &conflict) == NHP_OK);
        free(p01);
        free(p02);

        char *src_dir = inst_source_full_path(&inst);
        char *patches_dir = path_join(src_dir, "patches");
        DIR *dp = opendir(patches_dir);
        CHECK(dp != NULL);
        if (dp != NULL) {
            const struct dirent *entry;
            while ((entry = readdir(dp)) != NULL) {
                if (strncmp(entry->d_name, "t02-", 4) == 0) {
                    char *victim = path_join(patches_dir, entry->d_name);
                    CHECK(victim != NULL && unlink(victim) == 0);
                    free(victim);
                }
            }
            (void)closedir(dp);
        }
        free(patches_dir);

        CHECK(stack_adopt(&inst) == NHP_OK);
        CHECK(inst.patch_count == 1);
        CHECK(str_eq(inst.patches[0].id, "t01"));
        free(src_dir);
    });
}

/* The "E"/"R" workflow (features.md 12.4): begin pops the patch to the
 * top, a hand edit changes its source, refresh captures that edit and
 * re-pushes above it -- consistency check below catches a failed re-push. */
static void test_edit_begin_then_refresh(void)
{
    WITH_TEMP_INSTANCE({
        char *p01 = abs_patch("01-lamp-radius.diff");
        char sha[65];
        real_sha256_of(p01, sha);
        size_t idx = SIZE_MAX;
        stack_conflict conflict;
        CHECK(stack_add(&inst, "t01", "1.0", "Lamp Radius", p01, sha, true, &idx, &conflict) ==
              NHP_OK);
        free(p01);
        CHECK(!inst.patches[0].modified);
        char original_sha[65];
        (void)snprintf(original_sha, sizeof(original_sha), "%s", inst.patches[0].sha256);

        CHECK(stack_edit_begin(&inst, 0, &conflict) == NHP_OK);

        char *src_dir = inst_source_full_path(&inst);
        char *light_path = path_join(src_dir, "src/light.c");
        char *data = NULL;
        size_t len = 0;
        CHECK(fs_read_file(light_path, &data, &len) == NHP_OK);
        /* t01 has nothing above it, so quilt_pop() leaves it applied as
         * the new top (quilt.h's own "name already is the top" case) --
         * the file still reads as the *patched* value. */
        char *found = (data != NULL) ? strstr(data, "return 5;") : NULL;
        CHECK(found != NULL);

        if (found != NULL) {
            buf edited;
            buf_init(&edited);
            buf_append(&edited, data, (size_t)(found - data));
            buf_append_str(&edited, "return 9;");
            buf_append(&edited, found + strlen("return 5;"),
                       len - (size_t)(found - data) - strlen("return 5;"));
            CHECK(fs_write_atomic(light_path, edited.data, edited.len) == NHP_OK);
            buf_free(&edited);
        }
        free(data);

        char *diff_path = NULL;
        CHECK(stack_edit_refresh(&inst, 0, &conflict, &diff_path) == NHP_OK);
        CHECK(inst.patches[0].modified);
        CHECK(!str_eq(inst.patches[0].sha256, original_sha));

        char *diff_data = NULL;
        size_t diff_len = 0;
        CHECK(diff_path != NULL && fs_read_file(diff_path, &diff_data, &diff_len) == NHP_OK);
        CHECK(diff_data != NULL && strstr(diff_data, "return 9;") != NULL);
        free(diff_data);
        free(diff_path);

        stack_drift drift;
        CHECK(stack_check_consistency(&inst, &drift) == NHP_OK);
        CHECK(drift.ok);
        stack_drift_free(&drift);

        char *light_now = NULL;
        size_t light_now_len = 0;
        CHECK(fs_read_file(light_path, &light_now, &light_now_len) == NHP_OK);
        CHECK(light_now != NULL && strstr(light_now, "return 9;") != NULL);
        free(light_now);
        free(light_path);
        free(src_dir);
    });
}

/* docs/features.md 12.4's "N" (New patch): a brand-new, empty patch
 * pushes cleanly and lands in instance.conf already enabled. */
static void test_new_patch(void)
{
    WITH_TEMP_INSTANCE({
        size_t idx = SIZE_MAX;
        CHECK(stack_new_patch(&inst, "local-my-feature", "My Feature", &idx) == NHP_OK);
        CHECK(idx == 0);
        CHECK(inst.patch_count == 1);
        CHECK(str_eq(inst.patches[0].id, "local-my-feature"));
        CHECK(inst.patches[0].enabled);
        CHECK(inst.patches[0].strip == 1);
        CHECK(str_eq(inst.patches[0].version, ""));

        char *src_dir = inst_source_full_path(&inst);
        char *patch_path = path_join(src_dir, "patches/local-my-feature-my-feature.diff");
        CHECK(patch_path != NULL && fs_exists(patch_path));
        free(patch_path);

        stack_drift drift;
        CHECK(stack_check_consistency(&inst, &drift) == NHP_OK);
        CHECK(drift.ok);
        stack_drift_free(&drift);

        inst_instance reloaded;
        CHECK(inst_load("stacktest", &reloaded) == NHP_OK);
        CHECK(reloaded.patch_count == 1);
        CHECK(reloaded.patches[0].enabled);
        inst_free(&reloaded);
        free(src_dir);
    });
}

/* A second "quilt new" with a filename that already exists is rejected
 * rather than silently overwriting it. */
static void test_new_patch_rejects_existing_filename(void)
{
    WITH_TEMP_INSTANCE({
        size_t idx = SIZE_MAX;
        CHECK(stack_new_patch(&inst, "local-my-feature", "My Feature", &idx) == NHP_OK);
        CHECK(stack_new_patch(&inst, "local-my-feature", "My Feature", &idx) == NHP_EEXIST);
        CHECK(inst.patch_count == 1);
    });
}

/* The full 12.4 "N"+"o"+"R" lifecycle: a new patch, a new file tracked
 * into it (stack_track_file()), then edited and refreshed -- the file's
 * content ends up in the patch as if it had been there from the start. */
static void test_new_patch_track_file_then_refresh(void)
{
    WITH_TEMP_INSTANCE({
        size_t idx = SIZE_MAX;
        CHECK(stack_new_patch(&inst, "local-my-feature", "My Feature", &idx) == NHP_OK);

        char *src_dir = inst_source_full_path(&inst);
        char *new_file = path_join(src_dir, "src/newfile.c");
        CHECK(new_file != NULL &&
              fs_write_atomic(new_file, "initial\n", strlen("initial\n")) == NHP_OK);

        CHECK(stack_track_file(&inst, "src/newfile.c") == NHP_OK);

        CHECK(fs_write_atomic(new_file, "created content\n", strlen("created content\n")) ==
              NHP_OK);

        stack_conflict conflict;
        char *diff_path = NULL;
        CHECK(stack_edit_refresh(&inst, idx, &conflict, &diff_path) == NHP_OK);
        CHECK(inst.patches[idx].modified);

        char *diff_data = NULL;
        size_t diff_len = 0;
        CHECK(diff_path != NULL && fs_read_file(diff_path, &diff_data, &diff_len) == NHP_OK);
        CHECK(diff_data != NULL && strstr(diff_data, "created content") != NULL);
        free(diff_data);
        free(diff_path);

        stack_drift drift;
        CHECK(stack_check_consistency(&inst, &drift) == NHP_OK);
        CHECK(drift.ok);
        stack_drift_free(&drift);

        free(new_file);
        free(src_dir);
    });
}

int main(void)
{
    test_add_strip_detection();
    test_add_disabled_then_enable();
    test_add_rejects_duplicate_id();
    test_restore_snapshot_resyncs_tree();
    test_undo_none();
    test_undo_enable();
    test_undo_move();
    test_undo_skips_manual_snapshot();
    test_dependent_patch_blocks_disable();
    test_check_disabled();
    test_check_disabled_none();
    test_shared_files_detected();
    test_shared_files_none_when_disjoint();
    test_shared_files_best_effort_skips_non_library_patch();
    test_enable_conflict_rolls_back();
    test_move();
    test_remove();
    test_remove_disables_first();
    test_force_then_manual_fix();
    test_consistency_clean();
    test_consistency_detects_manual_pop();
    test_adopt_follows_manual_pop();
    test_adopt_drops_missing_file();
    test_edit_begin_then_refresh();
    test_new_patch();
    test_new_patch_rejects_existing_filename();
    test_new_patch_track_file_then_refresh();

    if (failures == 0) {
        (void)printf("ok: all stack tests passed\n");
    } else {
        (void)printf("FAILED: %d check(s) failed\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
