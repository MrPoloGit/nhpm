/* test_sources.c - unit tests for sources.c (no network, no terminal).
 * bsrc_add_from_git() needs the real network and is exercised manually
 * instead (docs/testing.md: CI never contacts it). SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "proc.h"
#include "sources.h"
#include "util.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                  \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

#define WITH_TEMP_SOURCES(body)                                                                    \
    do {                                                                                           \
        char *tmp_data_dir = fs_make_temp_dir("nhp-test-data");                                    \
        CHECK(tmp_data_dir != NULL);                                                               \
        setenv("XDG_DATA_HOME", tmp_data_dir, 1);                                                  \
        body;                                                                                      \
        unsetenv("XDG_DATA_HOME");                                                                 \
        char *chmod_argv_cmd = path_join(tmp_data_dir, "nhpm/sources");                            \
        if (chmod_argv_cmd != NULL) {                                                              \
            make_tree_writable(chmod_argv_cmd);                                                    \
            free(chmod_argv_cmd);                                                                  \
        }                                                                                          \
        fs_remove_tree(tmp_data_dir);                                                              \
        free(tmp_data_dir);                                                                        \
    } while (0)

/* source trees are made read-only on import; restore write permission
 * (recursively) before the temp dir cleanup, so fs_remove_tree() can
 * actually delete it. */
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

static void test_add_from_folder(void)
{
    WITH_TEMP_SOURCES({
        char *version = NULL;
        nhp_err err = bsrc_add_from_folder("tests/fixtures/source343", NULL, &version);
        CHECK(err == NHP_OK);
        CHECK(version != NULL && str_eq(version, "3.4.3"));

        bsrc_source found;
        CHECK(bsrc_find("3.4.3", &found) == NHP_OK);
        CHECK(str_eq(found.version, "3.4.3"));
        CHECK(str_eq(found.origin, "folder:tests/fixtures/source343"));
        CHECK(!found.is_variant);
        CHECK(strlen(found.added) > 0);
        bsrc_source_free(&found);

        /* the imported tree is read-only */
        char *tree = bsrc_tree_path("3.4.3");
        char *header = path_join(tree, "include/patchlevel.h");
        CHECK(fs_write_atomic(header, "tampered", 8) != NHP_OK);
        char *data = NULL;
        size_t len = 0;
        CHECK(fs_read_file(header, &data, &len) == NHP_OK);
        CHECK(strstr(data, "VERSION_MAJOR") != NULL);
        free(data);
        free(header);
        free(tree);

        /* re-adding the same version is rejected, but *out_version is
         * still set to the version that conflicted -- a UI caller
         * offering "replace it?" needs that without parsing err_get(). */
        char *version2 = NULL;
        err = bsrc_add_from_folder("tests/fixtures/source343", NULL, &version2);
        CHECK(err == NHP_EEXIST);
        CHECK(version2 != NULL && str_eq(version2, "3.4.3"));
        free(version2);

        free(version);
    });
}

static void test_add_from_folder_with_hint(void)
{
    WITH_TEMP_SOURCES({
        /* a folder without include/patchlevel.h at all needs a hint */
        char *tmp_dir = fs_make_temp_dir("nhp-test-variant");
        char *version = NULL;
        nhp_err err = bsrc_add_from_folder(tmp_dir, "SLASH'EM 0.0.8", &version);
        CHECK(err == NHP_OK);
        CHECK(version != NULL);

        bsrc_source found;
        CHECK(bsrc_find(version, &found) == NHP_OK);
        CHECK(found.is_variant);
        bsrc_source_free(&found);

        free(version);
        fs_remove_tree(tmp_dir);
        free(tmp_dir);
    });
}

static void test_add_from_folder_no_version(void)
{
    WITH_TEMP_SOURCES({
        char *tmp_dir = fs_make_temp_dir("nhp-test-noversion");
        char *version = NULL;
        nhp_err err = bsrc_add_from_folder(tmp_dir, NULL, &version);
        CHECK(err == NHP_EPARSE);
        CHECK(version == NULL);
        fs_remove_tree(tmp_dir);
        free(tmp_dir);
    });
}

static char *make_tarball_fixture(void)
{
    char *work_dir = fs_make_temp_dir("nhp-test-tarsrc");
    char *inner = path_join(work_dir, "nethack-367-src");
    char *include_dir = path_join(inner, "include");
    fs_mkdir_p(include_dir, 0700);

    char *src_header = path_join("tests/fixtures/source367", "include/patchlevel.h");
    char *data = NULL;
    size_t len = 0;
    fs_read_file(src_header, &data, &len);
    char *dst_header = path_join(include_dir, "patchlevel.h");
    fs_write_atomic(dst_header, data, len);
    free(data);

    char *readme = path_join(inner, "README");
    fs_write_atomic(readme, "dummy\n", 6);

    char *tarball_path = path_join(work_dir, "fixture.tar.gz");
    const char *argv[] = {"tar", "czf", tarball_path, "-C", work_dir, "nethack-367-src", NULL};
    proc_result result = {0};
    proc_run(argv, NULL, NULL, &result);
    bool ok = !result.signaled && result.exit_code == 0;
    proc_result_free(&result);

    free(inner);
    free(include_dir);
    free(src_header);
    free(dst_header);
    free(readme);
    free(work_dir);

    if (!ok) {
        free(tarball_path);
        return NULL;
    }
    return tarball_path;
}

static void test_add_from_tarball(void)
{
    WITH_TEMP_SOURCES({
        char *tarball_path = make_tarball_fixture();
        CHECK(tarball_path != NULL);
        if (tarball_path != NULL) {
            char *version = NULL;
            nhp_err err = bsrc_add_from_tarball(tarball_path, NULL, &version);
            CHECK(err == NHP_OK);
            CHECK(version != NULL && str_eq(version, "3.6.7"));

            /* the single top-level directory inside the tarball must have
             * been unwrapped: tree/README exists directly, not
             * tree/nethack-367-src/README */
            char *tree = bsrc_tree_path("3.6.7");
            char *readme = path_join(tree, "README");
            CHECK(fs_exists(readme));
            free(readme);
            free(tree);

            bsrc_source found;
            CHECK(bsrc_find("3.6.7", &found) == NHP_OK);
            CHECK(strlen(found.sha256) == 64); /* sha256 of the tarball itself */
            bsrc_source_free(&found);

            free(version);
            /* work_dir (parent of the tarball) cleanup */
            char *work_dir = str_dup(tarball_path);
            char *slash = strrchr(work_dir, '/');
            if (slash != NULL) {
                *slash = '\0';
            }
            fs_remove_tree(work_dir);
            free(work_dir);
        }
        free(tarball_path);
    });
}

static void test_add_from_tarball_rejects_unsafe_paths(void)
{
    /* tar(1) itself sanitizes ".." when *creating* an archive, so this
     * fixture was built directly with Python's tarfile module (which
     * doesn't) to get a genuinely unsafe entry to test against. */
    WITH_TEMP_SOURCES({
        char *version = NULL;
        nhp_err err =
            bsrc_add_from_tarball("tests/fixtures/evil_path_traversal.tar.gz", NULL, &version);
        CHECK(err == NHP_EPARSE);
        CHECK(version == NULL);
        CHECK(strstr(err_get(), "escaped.txt") != NULL);
        free(version);
    });
}

static void test_find_and_remove_missing(void)
{
    WITH_TEMP_SOURCES({
        bsrc_source s;
        CHECK(bsrc_find("9.9.9", &s) == NHP_ENOENT);
        CHECK(bsrc_remove("9.9.9") == NHP_ENOENT);

        bsrc_source *sources = NULL;
        size_t count = 0;
        CHECK(bsrc_list(&sources, &count) == NHP_OK);
        CHECK(count == 0);
        bsrc_list_free(sources, count);
    });
}

static void test_list_and_remove(void)
{
    WITH_TEMP_SOURCES({
        char *v1 = NULL;
        CHECK(bsrc_add_from_folder("tests/fixtures/source343", NULL, &v1) == NHP_OK);
        char *v2 = NULL;
        CHECK(bsrc_add_from_folder("tests/fixtures/source367", NULL, &v2) == NHP_OK);

        bsrc_source *sources = NULL;
        size_t count = 0;
        CHECK(bsrc_list(&sources, &count) == NHP_OK);
        CHECK(count == 2);
        bsrc_list_free(sources, count);

        CHECK(bsrc_remove("3.4.3") == NHP_OK);
        CHECK(bsrc_list(&sources, &count) == NHP_OK);
        CHECK(count == 1);
        bsrc_list_free(sources, count);

        bsrc_source gone;
        CHECK(bsrc_find("3.4.3", &gone) == NHP_ENOENT);

        free(v1);
        free(v2);
    });
}

int main(void)
{
    test_add_from_folder();
    test_add_from_folder_with_hint();
    test_add_from_folder_no_version();
    test_add_from_tarball();
    test_add_from_tarball_rejects_unsafe_paths();
    test_find_and_remove_missing();
    test_list_and_remove();

    if (failures == 0) {
        (void)printf("ok: all sources tests passed\n");
    } else {
        (void)printf("FAILED: %d check(s) failed\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
