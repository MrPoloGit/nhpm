/* test_library.c - unit tests for library.c (no network, no terminal).
 * lib_download() and lib_fetch_detail() need the real site and are
 * exercised manually (docs/testing.md: CI never contacts it). SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "library.h"
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

#define WITH_TEMP_LIBRARY(body)                                                                    \
    do {                                                                                           \
        char *tmp_data_dir = fs_make_temp_dir("nhp-test-data");                                    \
        CHECK(tmp_data_dir != NULL);                                                               \
        setenv("XDG_DATA_HOME", tmp_data_dir, 1);                                                  \
        body;                                                                                      \
        unsetenv("XDG_DATA_HOME");                                                                 \
        fs_remove_tree(tmp_data_dir);                                                              \
        free(tmp_data_dir);                                                                        \
    } while (0)

static void test_looks_like_diff_unified(void)
{
    const char *diff = "--- a/src/pline.c\n"
                       "+++ b/src/pline.c\n"
                       "@@ -1,3 +1,3 @@\n"
                       "-old\n"
                       "+new\n"
                       " unchanged\n";
    CHECK(lib_looks_like_diff(diff, strlen(diff)));
}

static void test_looks_like_diff_context(void)
{
    const char *diff = "*** a/src/pline.c\n"
                       "--- b/src/pline.c\n"
                       "***************\n"
                       "*** 1,3 ****\n"
                       "- old\n"
                       "--- 1,3 ----\n"
                       "+ new\n";
    CHECK(lib_looks_like_diff(diff, strlen(diff)));
}

static void test_looks_like_diff_plain(void)
{
    const char *diff = "3,5c3,5\n"
                       "< old line\n"
                       "---\n"
                       "> new line\n"
                       "7d6\n"
                       "< removed\n"
                       "10a11\n"
                       "> added\n";
    CHECK(lib_looks_like_diff(diff, strlen(diff)));
}

static void test_looks_like_diff_html(void)
{
    char *html = NULL;
    size_t len = 0;
    CHECK(fs_read_file("tests/fixtures/browse_no_table.html", &html, &len) == NHP_OK);
    if (html != NULL) {
        CHECK(!lib_looks_like_diff(html, len));
        free(html);
    }
}

static void test_looks_like_diff_empty(void)
{
    CHECK(!lib_looks_like_diff("", 0));
}

static void test_looks_like_diff_crlf(void)
{
    const char *diff = "--- a/x.c\r\n+++ b/x.c\r\n@@ -1 +1 @@\r\n-a\r\n+b\r\n";
    CHECK(lib_looks_like_diff(diff, strlen(diff)));
}

static void test_looks_like_diff_random_text(void)
{
    const char *text = "Just a moment, this is not a patch at all, it's an error page.\n"
                       "Nothing here looks like a hunk header.\n";
    CHECK(!lib_looks_like_diff(text, strlen(text)));
}

static const char *const sample_diff = "--- a/src/potion.c\r\n"
                                       "+++ b/src/potion.c\r\n"
                                       "@@ -10,3 +10,3 @@\r\n"
                                       "-old line\r\n"
                                       "+new line\r\n"
                                       " unchanged\r\n";

static void write_sample_diff(const char *path)
{
    FILE *fp = fopen(path, "wb");
    CHECK(fp != NULL);
    if (fp != NULL) {
        size_t len = strlen(sample_diff);
        CHECK(fwrite(sample_diff, 1, len, fp) == len);
        (void)fclose(fp);
    }
}

static void test_import_local_round_trip(void)
{
    WITH_TEMP_LIBRARY({
        char *tmp_dir = fs_make_temp_dir("nhp-test-src");
        CHECK(tmp_dir != NULL);
        char *diff_path = path_join(tmp_dir, "my.diff");
        write_sample_diff(diff_path);

        char *id = NULL;
        nhp_err err =
            lib_import_local(diff_path, "My Local Patch", "Me", "NetHack 3.6.7", NULL, &id);
        CHECK(err == NHP_OK);
        CHECK(id != NULL && str_eq(id, "local-my-local-patch"));

        lib_patch found;
        CHECK(lib_find(id, &found) == NHP_OK);
        CHECK(str_eq(found.id, id));
        CHECK(str_eq(found.name, "My Local Patch"));
        CHECK(str_eq(found.author, "Me"));
        CHECK(str_eq(found.target, "NetHack 3.6.7"));
        CHECK(found.version_count == 1);
        if (found.version_count == 1) {
            CHECK(str_eq(found.versions[0].version, "1.0"));
            CHECK(!found.versions[0].via_wayback);
            CHECK(strlen(found.versions[0].sha256) == 64);
            /* the CRLF sample must have been normalized to LF on import */
            CHECK(strstr(found.versions[0].files_touched, "src/potion.c") != NULL);
        }
        lib_patch_free(&found);

        bool ok = false;
        CHECK(lib_verify_version(id, "1.0", &ok) == NHP_OK);
        CHECK(ok);

        /* the stored file itself must be CRLF-normalized, not a verbatim
         * copy of the CRLF source */
        char *stored_path = path_join(tmp_dir, "unused");
        free(stored_path);
        {
            char *data_dir = cfg_xdg_data_dir();
            char *lib_dir = path_join(data_dir, "library");
            char *patch_dir = path_join(lib_dir, id);
            char *version_dir = path_join(patch_dir, "1.0");
            char *patch_diff_path = path_join(version_dir, "patch.diff");
            char *stored = NULL;
            size_t stored_len = 0;
            CHECK(fs_read_file(patch_diff_path, &stored, &stored_len) == NHP_OK);
            if (stored != NULL) {
                CHECK(memchr(stored, '\r', stored_len) == NULL);
                free(stored);
            }
            free(data_dir);
            free(lib_dir);
            free(patch_dir);
            free(version_dir);
            free(patch_diff_path);
        }

        /* re-importing the same version is rejected */
        char *id2 = NULL;
        err = lib_import_local(diff_path, "My Local Patch", "Me", "NetHack 3.6.7", NULL, &id2);
        CHECK(err == NHP_EEXIST);
        free(id2);

        /* a second version under an explicit version string */
        char *id3 = NULL;
        err = lib_import_local(diff_path, "My Local Patch", "Me", "NetHack 3.6.7", "1.1", &id3);
        CHECK(err == NHP_OK);
        CHECK(id3 != NULL && str_eq(id3, id)); /* same patch, same slug */
        free(id3);

        lib_patch *patches = NULL;
        size_t count = 0;
        CHECK(lib_list(&patches, &count) == NHP_OK);
        CHECK(count == 1);
        if (count == 1) {
            CHECK(patches[0].version_count == 2);
        }
        lib_list_free(patches, count);

        CHECK(lib_delete_version(id, "1.1") == NHP_OK);
        CHECK(lib_delete_version(id, "1.1") == NHP_ENOENT); /* already gone */

        CHECK(lib_delete_patch(id) == NHP_OK);
        lib_patch gone;
        CHECK(lib_find(id, &gone) == NHP_ENOENT);

        free(id);
        free(diff_path);
        fs_remove_tree(tmp_dir);
        free(tmp_dir);
    });
}

static void test_import_rejects_non_diff(void)
{
    WITH_TEMP_LIBRARY({
        char *tmp_dir = fs_make_temp_dir("nhp-test-src");
        char *bad_path = path_join(tmp_dir, "not-a-diff.txt");
        FILE *fp = fopen(bad_path, "w");
        fprintf(fp, "this is just some text\n");
        (void)fclose(fp);

        char *id = NULL;
        nhp_err err = lib_import_local(bad_path, "Bad", "Me", "NetHack 3.6.7", NULL, &id);
        CHECK(err == NHP_EPARSE);
        CHECK(id == NULL);

        free(bad_path);
        fs_remove_tree(tmp_dir);
        free(tmp_dir);
    });
}

static void test_find_and_delete_missing(void)
{
    WITH_TEMP_LIBRARY({
        lib_patch p;
        CHECK(lib_find("local-nope", &p) == NHP_ENOENT);
        CHECK(lib_find("485", &p) == NHP_ENOENT);
        CHECK(lib_delete_patch("local-nope") == NHP_ENOENT);
        CHECK(lib_delete_version("local-nope", "1.0") == NHP_ENOENT);

        bool ok = false;
        CHECK(lib_verify_version("local-nope", "1.0", &ok) == NHP_ENOENT);

        lib_patch *patches = NULL;
        size_t count = 0;
        CHECK(lib_list(&patches, &count) == NHP_OK);
        CHECK(count == 0); /* no library directory yet: an empty list */
        lib_list_free(patches, count);
    });
}

static void test_lib_make_local_id(void)
{
    char *id = lib_make_local_id("My Local Patch");
    CHECK(id != NULL && str_eq(id, "local-my-local-patch"));
    free(id);
}

/* features.md 12.4's "N"/"R": stack_new_patch() never touches the
 * library, so lib_add_version() creates the entry itself for the first
 * version captured; later versions bump instead. */
static void test_add_version_creates_new_local_patch(void)
{
    WITH_TEMP_LIBRARY({
        char *tmp_dir = fs_make_temp_dir("nhp-test-src");
        CHECK(tmp_dir != NULL);
        char *diff_path = path_join(tmp_dir, "my.diff");
        write_sample_diff(diff_path);

        char *version = NULL;
        nhp_err err = lib_add_version("local-brand-new", "Brand New", "Me", "NetHack 3.6.7", "",
                                      diff_path, &version);
        CHECK(err == NHP_OK);
        CHECK(version != NULL && str_eq(version, "1.0"));
        free(version);

        lib_patch found;
        CHECK(lib_find("local-brand-new", &found) == NHP_OK);
        CHECK(str_eq(found.name, "Brand New"));
        CHECK(str_eq(found.author, "Me"));
        CHECK(str_eq(found.target, "NetHack 3.6.7"));
        CHECK(found.version_count == 1);
        lib_patch_free(&found);

        /* a second refresh bumps instead of re-creating patch.meta */
        char *version2 = NULL;
        err = lib_add_version("local-brand-new", "", "", "", "", diff_path, &version2);
        CHECK(err == NHP_OK);
        CHECK(version2 != NULL && str_eq(version2, "1.1"));
        free(version2);

        lib_patch found2;
        CHECK(lib_find("local-brand-new", &found2) == NHP_OK);
        CHECK(str_eq(found2.name, "Brand New")); /* unchanged by the bump */
        CHECK(found2.version_count == 2);
        lib_patch_free(&found2);

        free(diff_path);
        fs_remove_tree(tmp_dir);
        free(tmp_dir);
    });
}

static void test_add_version_rejects_nonlocal_missing_id(void)
{
    WITH_TEMP_LIBRARY({
        char *tmp_dir = fs_make_temp_dir("nhp-test-src");
        CHECK(tmp_dir != NULL);
        char *diff_path = path_join(tmp_dir, "my.diff");
        write_sample_diff(diff_path);

        char *version = NULL;
        CHECK(lib_add_version("498", "Color Alchemy", "", "", "", diff_path, &version) ==
              NHP_ENOENT);
        CHECK(version == NULL);

        free(diff_path);
        fs_remove_tree(tmp_dir);
        free(tmp_dir);
    });
}

/* docs/features.md 12.4's "L": forking a downloaded patch's edited copy
 * into its own local- entry, keeping the original's own version history
 * untouched. */
static void test_fork_as_local(void)
{
    WITH_TEMP_LIBRARY({
        char *tmp_dir = fs_make_temp_dir("nhp-test-src");
        CHECK(tmp_dir != NULL);
        char *diff_path = path_join(tmp_dir, "edited.diff");
        write_sample_diff(diff_path);

        char *id = NULL;
        char *version = NULL;
        nhp_err err = lib_fork_as_local("498", "1.2", "Color Alchemy", "Someone Else",
                                        "NetHack 3.6.0", diff_path, &id, &version);
        CHECK(err == NHP_OK);
        CHECK(id != NULL && str_eq(id, "local-498-color-alchemy-my"));
        CHECK(version != NULL && str_eq(version, "1.2-1"));

        lib_patch found;
        CHECK(lib_find(id, &found) == NHP_OK);
        CHECK(str_eq(found.name, "Color Alchemy"));
        CHECK(str_eq(found.author, "Someone Else"));
        CHECK(str_eq(found.target, "NetHack 3.6.0"));
        CHECK(found.version_count == 1);
        if (found.version_count == 1) {
            CHECK(str_eq(found.versions[0].version, "1.2-1"));
        }
        lib_patch_free(&found);
        free(id);
        free(version);

        /* forking the same original edit again doesn't collide -- it
         * gets the next free "-N" suffix, keeping the earlier fork */
        char *id2 = NULL;
        char *version2 = NULL;
        err = lib_fork_as_local("498", "1.2", "Color Alchemy", "Someone Else", "NetHack 3.6.0",
                                diff_path, &id2, &version2);
        CHECK(err == NHP_OK);
        CHECK(id2 != NULL && str_eq(id2, "local-498-color-alchemy-my"));
        CHECK(version2 != NULL && str_eq(version2, "1.2-2"));

        lib_patch found2;
        CHECK(lib_find(id2, &found2) == NHP_OK);
        CHECK(found2.version_count == 2);
        lib_patch_free(&found2);
        free(id2);
        free(version2);

        free(diff_path);
        fs_remove_tree(tmp_dir);
        free(tmp_dir);
    });
}

static void test_fork_as_local_rejects_non_diff(void)
{
    WITH_TEMP_LIBRARY({
        char *tmp_dir = fs_make_temp_dir("nhp-test-src");
        CHECK(tmp_dir != NULL);
        char *bad_path = path_join(tmp_dir, "not-a-diff.txt");
        CHECK(fs_write_atomic(bad_path, "hello\n", 6) == NHP_OK);

        char *id = NULL;
        char *version = NULL;
        CHECK(lib_fork_as_local("498", "1.2", "Color Alchemy", "", "", bad_path, &id, &version) ==
              NHP_EPARSE);
        CHECK(id == NULL && version == NULL);

        free(bad_path);
        fs_remove_tree(tmp_dir);
        free(tmp_dir);
    });
}

int main(void)
{
    test_looks_like_diff_unified();
    test_looks_like_diff_context();
    test_looks_like_diff_plain();
    test_looks_like_diff_html();
    test_looks_like_diff_empty();
    test_looks_like_diff_crlf();
    test_looks_like_diff_random_text();
    test_import_local_round_trip();
    test_import_rejects_non_diff();
    test_find_and_delete_missing();
    test_lib_make_local_id();
    test_add_version_creates_new_local_patch();
    test_add_version_rejects_nonlocal_missing_id();
    test_fork_as_local();
    test_fork_as_local_rejects_non_diff();

    if (failures == 0) {
        (void)printf("ok: all library tests passed\n");
    } else {
        (void)printf("FAILED: %d check(s) failed\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
