/* test_mkpatch.c - unit tests for mkpatch.c, plus integration tests
 * against the fake NetHack source fixture (docs/testing.md 17.1-17.2).
 * Needs quilt and GNU patch installed, same as test_stack.c. SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "instance.h"
#include "library.h"
#include "mkpatch.h"
#include "quilt.h"
#include "sources.h"
#include "stack.h"
#include "util.h"

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

static void make_tree_writable(const char *dir)
{
    const char *argv[] = {"chmod", "-R", "u+w", dir, NULL};
    proc_result result = {0};
    (void)proc_run(argv, NULL, NULL, &result);
    proc_result_free(&result);
}

/* A plain "patch -p1" into `dir`, kept as a free function rather than a
 * literal passed into WITH_TEMP_INSTANCE()'s braces -- a top-level comma
 * there would be read as splitting the macro's arguments. */
static bool apply_diff_p1(const char *dir, const char *diff_path)
{
    const char *argv[] = {"patch", "-p1", "--batch", "--force", "-d", dir, "-i", diff_path, NULL};
    proc_result pr = {0};
    nhp_err err = proc_run(argv, NULL, NULL, &pr);
    bool ok = (err == NHP_OK) && !pr.signaled && pr.exit_code == 0;
    proc_result_free(&pr);
    return ok;
}

/* Likewise: builds an mkp_patch_meta through a function call (whose own
 * commas are protected by its parentheses) instead of a brace literal,
 * so it's safe to use directly inside a WITH_TEMP_INSTANCE() body. */
static mkp_patch_meta make_meta(const char *name, const char *version, const char *author,
                                const char *target, const char *short_desc, const char *long_desc)
{
    mkp_patch_meta m;
    m.name = name;
    m.version = version;
    m.author = author;
    m.target = target;
    m.short_desc = short_desc;
    m.long_desc = long_desc;
    return m;
}

/* Every test runs under a fresh XDG_DATA_HOME, with the fake NetHack
 * fixture imported as base source "3.6.7" and instance "mkptest"
 * created from it -- mirrors test_stack.c's fixture setup. */
#define WITH_TEMP_INSTANCE(body)                                                                   \
    do {                                                                                           \
        char *tmp_data_dir = fs_make_temp_dir("nhp-test-mkpatch");                                 \
        CHECK(tmp_data_dir != NULL);                                                               \
        setenv("XDG_DATA_HOME", tmp_data_dir, 1);                                                  \
        char *src_version = NULL;                                                                  \
        CHECK(bsrc_add_from_folder("tests/fixtures/source", NULL, &src_version) == NHP_OK);        \
        free(src_version);                                                                         \
        inst_instance inst;                                                                        \
        CHECK(inst_new("mkptest", "3.6.7", "", &inst) == NHP_OK);                                  \
        body;                                                                                      \
        inst_free(&inst);                                                                          \
        unsetenv("XDG_DATA_HOME");                                                                 \
        make_tree_writable(tmp_data_dir);                                                          \
        (void)fs_remove_tree(tmp_data_dir);                                                        \
        free(tmp_data_dir);                                                                        \
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

static char *abs_fixture_source(void)
{
    char cwd[2048];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        return NULL;
    }
    buf b;
    buf_init(&b);
    buf_append_fmt(&b, "%s/tests/fixtures/source", cwd);
    return buf_take(&b);
}

/* ---- 17.1: ignore rules ---- */

static void test_ignore_general(void)
{
    CHECK(mkp_is_ignored("src/foo.o", "", ""));
    CHECK(mkp_is_ignored("foo.orig", "", ""));
    CHECK(mkp_is_ignored("foo.rej", "", ""));
    CHECK(mkp_is_ignored(".pc/foo", "", ""));
    CHECK(mkp_is_ignored("patches/foo.diff", "", ""));
    CHECK(mkp_is_ignored("series", "", ""));
    CHECK(mkp_is_ignored(".git/HEAD", "", ""));
    CHECK(!mkp_is_ignored("src/potion.c", "", ""));
    CHECK(!mkp_is_ignored("dat/potion.txt", "", ""));
}

static void test_ignore_version_specific(void)
{
    CHECK(mkp_is_ignored("include/onames.h", "3.6.7", ""));
    CHECK(mkp_is_ignored("dat/foo.lev", "3.6.0", ""));
    CHECK(!mkp_is_ignored("include/onames.h", "3.4.3", ""));
    CHECK(!mkp_is_ignored("include/onames.h", "", ""));
}

static void test_ignore_extra_patterns(void)
{
    CHECK(mkp_is_ignored("foo.bak", "", "*.bak *.tmp"));
    CHECK(mkp_is_ignored("sub/bar.tmp", "", "*.bak *.tmp"));
    CHECK(!mkp_is_ignored("foo.c", "", "*.bak *.tmp"));
}

/* docs/features.md 12.5's "h" only ever reveals a *build-output* guess,
 * never nhpm's/Quilt's own bookkeeping -- the two tiers mkp_is_ignored()
 * ORs together have to stay told apart for that to work. */
static void test_structural_vs_build_output(void)
{
    CHECK(mkp_is_structural(".pc/foo"));
    CHECK(mkp_is_structural("patches/foo.diff"));
    CHECK(mkp_is_structural("series"));
    CHECK(mkp_is_structural(".git/HEAD"));
    CHECK(!mkp_is_structural("src/foo.o"));
    CHECK(!mkp_is_structural("include/onames.h"));

    CHECK(mkp_is_build_output("src/foo.o", "", ""));
    CHECK(mkp_is_build_output("foo.orig", "", ""));
    CHECK(mkp_is_build_output("include/onames.h", "3.6.7", ""));
    CHECK(mkp_is_build_output("foo.bak", "", "*.bak"));
    CHECK(!mkp_is_build_output(".pc/foo", "", ""));
    CHECK(!mkp_is_build_output("series", "", ""));
    CHECK(!mkp_is_build_output("src/potion.c", "", ""));
}

static void test_looks_like_source(void)
{
    CHECK(mkp_looks_like_source("src/potion.c"));
    CHECK(mkp_looks_like_source("include/potion.h"));
    CHECK(mkp_looks_like_source("dat/wizard.des"));
    CHECK(mkp_looks_like_source("dat/myscript.lua"));
    CHECK(mkp_looks_like_source("dat/readme.txt"));
    CHECK(mkp_looks_like_source("doc/readme.txt"));
    CHECK(!mkp_looks_like_source("readme.txt"));
    CHECK(!mkp_looks_like_source("src/potion.c.save"));
    CHECK(!mkp_looks_like_source("win/share/tiles.bmp"));
}

static void test_looks_binary(void)
{
    const char text[] = "hello, world\nsecond line\n";
    CHECK(!mkp_looks_binary(text, strlen(text)));
    CHECK(!mkp_looks_binary("", 0));

    char binary[32];
    memset(binary, 'x', sizeof(binary));
    binary[10] = '\0';
    CHECK(mkp_looks_binary(binary, sizeof(binary)));
}

/* ---- 17.1: header writing (deterministic given the same input) ---- */

static void test_header_deterministic(void)
{
    mkp_header_info info = {
        .kind = "patch",
        .name = "my-fix",
        .version = "1.0",
        .author = "someone",
        .nethack_version = "3.6.7",
        .apply_hint = "patch -p1 < my-fix-1.0.diff",
        .short_desc = "a short description",
        .long_desc = "a longer one",
        .contains_block = "",
    };
    char *a = mkp_format_header(&info, "2026-11-15T21:40:12Z");
    char *b = mkp_format_header(&info, "2026-11-15T21:40:12Z");
    CHECK(a != NULL && b != NULL && str_eq(a, b));
    CHECK(strstr(a, "nhpm patch\n") == a);
    CHECK(strstr(a, "Name:      my-fix\n") != NULL);
    CHECK(strstr(a, "Version:   1.0\n") != NULL);
    CHECK(strstr(a, "Author:    someone\n") != NULL);
    CHECK(strstr(a, "NetHack:   3.6.7\n") != NULL);
    CHECK(strstr(a, "Created:   2026-11-15T21:40:12Z by nhpm 0.1\n") != NULL);
    CHECK(strstr(a, "Apply:     patch -p1 < my-fix-1.0.diff\n") != NULL);
    CHECK(strstr(a, "a short description") != NULL);
    CHECK(strstr(a, "a longer one") != NULL);
    free(a);
    free(b);

    mkp_header_info combined = {
        .kind = "combined patch",
        .name = "367-colorful",
        .version = "",
        .author = "",
        .nethack_version = "3.6.7",
        .apply_hint = "cd nethack-3.6.7 && patch -p1 < 367-colorful-combined.diff",
        .short_desc = "",
        .long_desc = "",
        .contains_block = "Contains, in order:\n  1. menucolors 1.5\n",
    };
    char *c = mkp_format_header(&combined, "2026-11-15T21:40:12Z");
    CHECK(strstr(c, "nhpm combined patch\n") == c);
    CHECK(strstr(c, "Version:") == NULL); /* omitted when "" */
    CHECK(strstr(c, "Author:") == NULL);
    CHECK(strstr(c, "Contains, in order:\n  1. menucolors 1.5\n") != NULL);
    free(c);
}

/* ---- 17.1: comparing and diffing two plain trees (no quilt needed) ---- */

static nhp_err write_file(const char *dir, const char *rel, const char *content)
{
    char *full = path_join(dir, rel);
    if (full == NULL) {
        return NHP_ENOMEM;
    }
    char *slash = strrchr(full, '/');
    if (slash != NULL) {
        *slash = '\0';
        (void)fs_mkdir_p(full, 0700);
        *slash = '/';
    }
    nhp_err err = fs_write_atomic(full, content, strlen(content));
    free(full);
    return err;
}

static void test_compare_and_diff_files(void)
{
    char *tree_a = fs_make_temp_dir("nhp-test-mkpatch-a");
    char *tree_b = fs_make_temp_dir("nhp-test-mkpatch-b");
    CHECK(tree_a != NULL && tree_b != NULL);

    CHECK(write_file(tree_a, "src/potion.c", "line one\nline two\n") == NHP_OK);
    CHECK(write_file(tree_b, "src/potion.c", "line one\nline CHANGED\n") == NHP_OK);
    CHECK(write_file(tree_a, "src/objects.c", "unchanged\n") == NHP_OK);
    CHECK(write_file(tree_b, "src/objects.c", "unchanged\n") == NHP_OK);
    CHECK(write_file(tree_a, "src/dead.c", "going away\n") == NHP_OK);
    CHECK(write_file(tree_b, "src/new.c", "brand new\n") == NHP_OK);
    CHECK(write_file(tree_b, "src/new.c.save", "backup junk\n") == NHP_OK);
    CHECK(write_file(tree_a, "src/foo.o", "pretend object file\n") == NHP_OK);
    CHECK(write_file(tree_b, "src/foo.o", "pretend object file, changed\n") == NHP_OK);

    mkp_file *files = NULL;
    size_t count = 0;
    nhp_err err = mkp_compare_trees(tree_a, tree_b, "3.6.7", "", &files, &count);
    CHECK(err == NHP_OK);

    bool saw_modified = false, saw_deleted = false, saw_added_unticked = false, saw_hidden = false;
    for (size_t i = 0; i < count; i++) {
        if (str_eq(files[i].path, "src/potion.c")) {
            CHECK(files[i].status == MKP_MODIFIED && files[i].default_selected && !files[i].hidden);
            saw_modified = true;
        } else if (str_eq(files[i].path, "src/dead.c")) {
            CHECK(files[i].status == MKP_DELETED && files[i].default_selected && !files[i].hidden);
            saw_deleted = true;
        } else if (str_eq(files[i].path, "src/new.c.save")) {
            CHECK(files[i].status == MKP_ADDED && !files[i].default_selected && !files[i].hidden);
            saw_added_unticked = true;
        } else if (str_eq(files[i].path, "src/foo.o")) {
            /* general ignore list: *.o is build output, not dropped
             * outright any more (12.5's "h") -- tagged hidden, ticked
             * off by default regardless of status. */
            CHECK(files[i].status == MKP_MODIFIED && files[i].hidden && !files[i].default_selected);
            saw_hidden = true;
        } else {
            CHECK(!str_eq(files[i].path, "src/objects.c")); /* unchanged -- not reported at all */
        }
    }
    CHECK(saw_modified && saw_deleted && saw_added_unticked && saw_hidden);

    const char *selected[] = {"src/potion.c", "src/dead.c"};
    char *diff = NULL;
    err = mkp_diff_files(tree_a, tree_b, selected, 2, &diff);
    CHECK(err == NHP_OK && diff != NULL);
    CHECK(strstr(diff, "a/src/potion.c") != NULL);
    CHECK(strstr(diff, "a/src/dead.c") != NULL);
    CHECK(strstr(diff, "new.c") == NULL); /* not in the selection */
    CHECK(strstr(diff, "line CHANGED") != NULL);

    /* deterministic given the same input */
    char *diff2 = NULL;
    CHECK(mkp_diff_files(tree_a, tree_b, selected, 2, &diff2) == NHP_OK);
    CHECK(diff2 != NULL && str_eq(diff, diff2));
    free(diff2);

    /* an unsafe path is rejected rather than silently skipped */
    const char *unsafe[] = {"../etc/passwd"};
    char *bad_diff = NULL;
    CHECK(mkp_diff_files(tree_a, tree_b, unsafe, 1, &bad_diff) == NHP_EINVAL);

    free(diff);
    mkp_file_list_free(files, count);
    (void)fs_remove_tree(tree_a);
    (void)fs_remove_tree(tree_b);
    free(tree_a);
    free(tree_b);
}

/* ---- 17.2: combined patch, against the fixture source ---- */

static void test_combined_patch(void)
{
    WITH_TEMP_INSTANCE({
        /* Imported into the library first: stack_add()'s `name` is only
         * used to slugify the filename, so mkp_combined()'s "Contains"
         * list can only recover a display name via lib_find(). */
        char *p01 = abs_patch("01-lamp-radius.diff");
        char *p02 = abs_patch("02-apply-pickaxe.diff");
        char *id01 = NULL;
        char *id02 = NULL;
        CHECK(lib_import_local(p01, "Lamp Radius", "", "3.6.1", NULL, &id01) == NHP_OK);
        CHECK(lib_import_local(p02, "Apply Pickaxe", "", "3.6.1", NULL, &id02) == NHP_OK);
        size_t idx = SIZE_MAX;
        stack_conflict conflict;
        CHECK(stack_add(&inst, id01, "1.0", "Lamp Radius", p01, "sha1", true, &idx, &conflict) ==
              NHP_OK);
        CHECK(stack_add(&inst, id02, "1.0", "Apply Pickaxe", p02, "sha2", true, &idx, &conflict) ==
              NHP_OK);
        free(p01);
        free(p02);

        mkp_combined_result result;
        nhp_err err = mkp_combined(&inst, NULL, 0, "", &result);
        CHECK(err == NHP_OK);
        CHECK(result.diff != NULL && result.file_count > 0);
        CHECK(strstr(result.diff, "nhpm combined patch\n") == result.diff);
        CHECK(strstr(result.diff, "Lamp Radius") != NULL);
        CHECK(strstr(result.diff, "Apply Pickaxe") != NULL);
        free(id01);
        free(id02);

        /* the combined diff applies with patch -p1 to clean source and
         * reproduces the instance's own src/, byte for byte, for the
         * files it touches (docs/testing.md 17.2's own check). */
        char *clean = fs_make_temp_dir("nhp-test-mkpatch-verify");
        CHECK(clean != NULL);
        char *base_tree = bsrc_tree_path("3.6.7");
        CHECK(base_tree != NULL && fs_copy_tree(base_tree, clean) == NHP_OK);
        free(base_tree);
        make_tree_writable(clean);

        char *diff_path = NULL;
        CHECK(fs_write_temp_file("nhp-test-combined", result.diff, strlen(result.diff),
                                 &diff_path) == NHP_OK);
        CHECK(apply_diff_p1(clean, diff_path));

        char *src_dir = inst_source_full_path(&inst);
        char *want_path = (src_dir != NULL) ? path_join(src_dir, "src/light.c") : NULL;
        char *got_path = path_join(clean, "src/light.c");
        char *want_data = NULL;
        char *got_data = NULL;
        size_t want_len = 0;
        size_t got_len = 0;
        CHECK(want_path != NULL && fs_read_file(want_path, &want_data, &want_len) == NHP_OK);
        CHECK(got_path != NULL && fs_read_file(got_path, &got_data, &got_len) == NHP_OK);
        CHECK(want_len == got_len && memcmp(want_data, got_data, want_len) == 0);
        free(want_data);
        free(got_data);
        free(want_path);
        free(got_path);
        free(src_dir);
        free(diff_path);
        make_tree_writable(clean);
        (void)fs_remove_tree(clean);
        free(clean);
        mkp_combined_result_free(&result);
    });
}

static void test_combined_excludes_disabled(void)
{
    WITH_TEMP_INSTANCE({
        char *p01 = abs_patch("01-lamp-radius.diff");
        char *p02 = abs_patch("02-apply-pickaxe.diff");
        char *id01 = NULL;
        char *id02 = NULL;
        CHECK(lib_import_local(p01, "Lamp Radius", "", "3.6.1", NULL, &id01) == NHP_OK);
        CHECK(lib_import_local(p02, "Apply Pickaxe", "", "3.6.1", NULL, &id02) == NHP_OK);
        size_t idx = SIZE_MAX;
        stack_conflict conflict;
        CHECK(stack_add(&inst, id01, "1.0", "Lamp Radius", p01, "sha1", true, &idx, &conflict) ==
              NHP_OK);
        CHECK(stack_add(&inst, id02, "1.0", "Apply Pickaxe", p02, "sha2", false, &idx, &conflict) ==
              NHP_OK); /* added disabled */
        free(p01);
        free(p02);

        mkp_combined_result result;
        CHECK(mkp_combined(&inst, NULL, 0, "", &result) == NHP_OK);
        CHECK(strstr(result.diff, "Lamp Radius") != NULL);
        CHECK(strstr(result.diff, "Apply Pickaxe") == NULL);
        mkp_combined_result_free(&result);
        free(id01);
        free(id02);
    });
}

/* A combined patch never includes build output, even though
 * mkp_compare_trees() now returns it tagged hidden instead of dropping
 * it (features.md 12.5's "h" needs the tag) -- mkp_combined() filters it. */
static void test_combined_excludes_hidden_build_output(void)
{
    WITH_TEMP_INSTANCE({
        char *p01 = abs_patch("01-lamp-radius.diff");
        char *id01 = NULL;
        CHECK(lib_import_local(p01, "Lamp Radius", "", "3.6.1", NULL, &id01) == NHP_OK);
        size_t idx = SIZE_MAX;
        stack_conflict conflict;
        CHECK(stack_add(&inst, id01, "1.0", "Lamp Radius", p01, "sha1", true, &idx, &conflict) ==
              NHP_OK);
        free(p01);

        char *src_dir = inst_source_full_path(&inst);
        CHECK(src_dir != NULL &&
              write_file(src_dir, "src/leftover.o", "pretend object file\n") == NHP_OK);

        mkp_combined_result result;
        CHECK(mkp_combined(&inst, NULL, 0, "", &result) == NHP_OK);
        CHECK(strstr(result.diff, "Lamp Radius") != NULL);
        CHECK(strstr(result.diff, "leftover.o") == NULL);
        mkp_combined_result_free(&result);
        free(id01);
        free(src_dir);
    });
}

/* ---- 17.2: changes / capture, against the fixture source ---- */

static void test_changes_empty_on_a_fresh_instance(void)
{
    WITH_TEMP_INSTANCE({
        mkp_file *files = NULL;
        size_t count = 0;
        CHECK(mkp_changes(&inst, "", &files, &count) == NHP_OK);
        CHECK(count == 0);
        mkp_file_list_free(files, count);
    });
}

static void test_capture_roundtrip(void)
{
    WITH_TEMP_INSTANCE({
        char *src_dir = inst_source_full_path(&inst);
        CHECK(src_dir != NULL);
        char *light_path = path_join(src_dir, "src/light.c");
        char *before = NULL;
        size_t before_len = 0;
        CHECK(light_path != NULL && fs_read_file(light_path, &before, &before_len) == NHP_OK);

        buf edited;
        buf_init(&edited);
        buf_append(&edited, before, before_len);
        buf_append_str(&edited, "/* captured edit */\n");
        char *edited_text = buf_take(&edited);
        CHECK(fs_write_atomic(light_path, edited_text, strlen(edited_text)) == NHP_OK);

        mkp_file *files = NULL;
        size_t count = 0;
        CHECK(mkp_changes(&inst, "", &files, &count) == NHP_OK);
        CHECK(count == 1);
        CHECK(str_eq(files[0].path, "src/light.c"));
        CHECK(files[0].status == MKP_MODIFIED && files[0].default_selected);

        const char *selected[] = {"src/light.c"};
        mkp_patch_meta meta = make_meta("my-captured-fix", "", "tester", "", "a captured edit", "");
        char *out_id = NULL;
        nhp_err err = mkp_capture(&inst, &meta, selected, 1, &out_id);
        CHECK(err == NHP_OK);
        CHECK(out_id != NULL && strncmp(out_id, "local-", 6) == 0);

        /* the folder ends up exactly where the manual edit left it */
        char *after = NULL;
        size_t after_len = 0;
        CHECK(fs_read_file(light_path, &after, &after_len) == NHP_OK);
        CHECK(after_len == strlen(edited_text) && memcmp(after, edited_text, after_len) == 0);

        /* it's on top of the stack, applied */
        CHECK(inst.patch_count == 1);
        CHECK(str_eq(inst.patches[0].id, out_id));
        CHECK(inst.patches[0].enabled);
        char **applied = NULL;
        size_t applied_count = 0;
        CHECK(quilt_applied(src_dir, &applied, &applied_count) == NHP_OK);
        CHECK(applied_count == 1);
        quilt_name_list_free(applied, applied_count);

        /* saved to the library as local-... 1.0 */
        lib_patch lp;
        CHECK(lib_find(out_id, &lp) == NHP_OK);
        CHECK(lp.version_count == 1);
        CHECK(str_eq(lp.versions[0].version, "1.0"));
        lib_patch_free(&lp);

        /* no more changes left to capture */
        mkp_file *files2 = NULL;
        size_t count2 = 0;
        CHECK(mkp_changes(&inst, "", &files2, &count2) == NHP_OK);
        CHECK(count2 == 0);
        mkp_file_list_free(files2, count2);

        free(out_id);
        free(after);
        free(before);
        free(edited_text);
        free(light_path);
        free(src_dir);
        mkp_file_list_free(files, count);
    });
}

static void test_capture_nothing_selected(void)
{
    WITH_TEMP_INSTANCE({
        char *out_id = NULL;
        mkp_patch_meta meta = make_meta("x", "", "", "", "", "");
        CHECK(mkp_capture(&inst, &meta, NULL, 0, &out_id) == NHP_ENOENT);
        CHECK(out_id == NULL);
    });
}

/* ---- 17.2: export-patch ---- */

static void test_export_patch(void)
{
    WITH_TEMP_INSTANCE({
        char *src_dir = inst_source_full_path(&inst);
        char *light_path = path_join(src_dir, "src/light.c");
        char *data = NULL;
        size_t len = 0;
        CHECK(fs_read_file(light_path, &data, &len) == NHP_OK);
        buf edited;
        buf_init(&edited);
        buf_append(&edited, data, len);
        buf_append_str(&edited, "/* export me */\n");
        char *edited_text = buf_take(&edited);
        CHECK(fs_write_atomic(light_path, edited_text, strlen(edited_text)) == NHP_OK);

        const char *selected[] = {"src/light.c"};
        mkp_patch_meta meta = make_meta("exportable", "", "", "", "", "");
        char *out_id = NULL;
        CHECK(mkp_capture(&inst, &meta, selected, 1, &out_id) == NHP_OK);

        char *tmp_dir = fs_make_temp_dir("nhp-test-export");
        char *out_path = (tmp_dir != NULL) ? path_join(tmp_dir, "exported.diff") : NULL;
        CHECK(out_path != NULL && mkp_export_patch(out_id, NULL, out_path) == NHP_OK);

        char *exported = NULL;
        size_t exported_len = 0;
        CHECK(fs_read_file(out_path, &exported, &exported_len) == NHP_OK);
        CHECK(strstr(exported, "nhpm patch\n") == exported);
        CHECK(strstr(exported, "Name:      exportable\n") != NULL);
        CHECK(strstr(exported, "export me") != NULL);

        free(exported);
        free(out_path);
        (void)fs_remove_tree(tmp_dir);
        free(tmp_dir);
        free(out_id);
        free(edited_text);
        free(data);
        free(light_path);
        free(src_dir);
    });
}

/* ---- 17.2: from a modified folder, and from git ---- */

static void test_from_folder_never_modifies_it(void)
{
    /* mkp_from_folder() saves to the library, so this needs its own
     * temp XDG_DATA_HOME -- WITH_TEMP_INSTANCE would also create an
     * instance this test has no use for. */
    char *tmp_data_dir = fs_make_temp_dir("nhp-test-mkpatch-data");
    CHECK(tmp_data_dir != NULL);
    setenv("XDG_DATA_HOME", tmp_data_dir, 1);

    char *fixture = abs_fixture_source();
    CHECK(fixture != NULL);

    char *copy = fs_make_temp_dir("nhp-test-mkpatch-folder");
    CHECK(copy != NULL && fs_copy_tree(fixture, copy) == NHP_OK);
    make_tree_writable(copy);

    char *light_path = path_join(copy, "src/light.c");
    char *data = NULL;
    size_t len = 0;
    CHECK(fs_read_file(light_path, &data, &len) == NHP_OK);
    buf edited;
    buf_init(&edited);
    buf_append(&edited, data, len);
    buf_append_str(&edited, "/* from-folder edit */\n");
    char *edited_text = buf_take(&edited);
    CHECK(fs_write_atomic(light_path, edited_text, strlen(edited_text)) == NHP_OK);

    char *fixture_light_before = NULL;
    size_t fixture_len_before = 0;
    char *fixture_light = path_join(fixture, "src/light.c");
    CHECK(fs_read_file(fixture_light, &fixture_light_before, &fixture_len_before) == NHP_OK);

    mkp_base_spec base = {.kind = MKP_BASE_FOLDER, .path_or_ref = fixture};
    mkp_patch_meta meta = {.name = "from-folder-fix",
                           .version = "",
                           .author = "",
                           .target = "",
                           .short_desc = "",
                           .long_desc = ""};
    char *out_id = NULL;
    nhp_err err = mkp_from_folder(copy, &base, &meta, "", NULL, 0, &out_id);
    CHECK(err == NHP_OK);
    CHECK(out_id != NULL && strncmp(out_id, "local-", 6) == 0);

    /* the base folder (read-only input) was never touched */
    char *fixture_light_after = NULL;
    size_t fixture_len_after = 0;
    CHECK(fs_read_file(fixture_light, &fixture_light_after, &fixture_len_after) == NHP_OK);
    CHECK(fixture_len_before == fixture_len_after &&
          memcmp(fixture_light_before, fixture_light_after, fixture_len_before) == 0);

    lib_patch lp;
    CHECK(lib_find(out_id, &lp) == NHP_OK);
    CHECK(lp.version_count == 1);
    lib_patch_free(&lp);

    free(out_id);
    free(fixture_light_before);
    free(fixture_light_after);
    free(fixture_light);
    free(edited_text);
    free(data);
    free(light_path);
    make_tree_writable(copy);
    (void)fs_remove_tree(copy);
    free(copy);
    free(fixture);
    unsetenv("XDG_DATA_HOME");
    make_tree_writable(tmp_data_dir);
    (void)fs_remove_tree(tmp_data_dir);
    free(tmp_data_dir);
}

static bool have_git(void)
{
    const char *argv[] = {"git", "--version", NULL};
    proc_result result = {0};
    nhp_err err = proc_run(argv, NULL, NULL, &result);
    bool ok = err == NHP_OK && !result.signaled && result.exit_code == 0;
    proc_result_free(&result);
    return ok;
}

static void test_from_git_ref(void)
{
    if (!have_git()) {
        (void)fprintf(stderr, "skip: git not found on PATH\n");
        return;
    }
    char *tmp_data_dir = fs_make_temp_dir("nhp-test-mkpatch-git-data");
    CHECK(tmp_data_dir != NULL);
    setenv("XDG_DATA_HOME", tmp_data_dir, 1);

    char *fixture = abs_fixture_source();
    char *repo = fs_make_temp_dir("nhp-test-mkpatch-git");
    CHECK(fixture != NULL && repo != NULL && fs_copy_tree(fixture, repo) == NHP_OK);
    make_tree_writable(repo);
    free(fixture);

    const char *init_argv[] = {"git", "init", "-q", NULL};
    const char *config_name[] = {"git", "config", "user.email", "test@example.com", NULL};
    const char *config_email[] = {"git", "config", "user.name", "Test", NULL};
    const char *add_argv[] = {"git", "add", "-A", NULL};
    const char *commit_argv[] = {"git", "commit", "-q", "-m", "init", NULL};
    proc_result pr = {0};
    CHECK(proc_run(init_argv, repo, NULL, &pr) == NHP_OK && pr.exit_code == 0);
    proc_result_free(&pr);
    CHECK(proc_run(config_name, repo, NULL, &pr) == NHP_OK);
    proc_result_free(&pr);
    CHECK(proc_run(config_email, repo, NULL, &pr) == NHP_OK);
    proc_result_free(&pr);
    CHECK(proc_run(add_argv, repo, NULL, &pr) == NHP_OK && pr.exit_code == 0);
    proc_result_free(&pr);
    CHECK(proc_run(commit_argv, repo, NULL, &pr) == NHP_OK && pr.exit_code == 0);
    proc_result_free(&pr);

    char *light_path = path_join(repo, "src/light.c");
    char *data = NULL;
    size_t len = 0;
    CHECK(fs_read_file(light_path, &data, &len) == NHP_OK);
    buf edited;
    buf_init(&edited);
    buf_append(&edited, data, len);
    buf_append_str(&edited, "/* git edit */\n");
    char *edited_text = buf_take(&edited);
    CHECK(fs_write_atomic(light_path, edited_text, strlen(edited_text)) == NHP_OK);
    CHECK(write_file(repo, "dat/brand-new.txt", "untracked addition\n") == NHP_OK);

    mkp_base_spec base = {.kind = MKP_BASE_GIT_REF, .path_or_ref = "HEAD"};
    mkp_patch_meta meta = {.name = "from-git-fix",
                           .version = "",
                           .author = "",
                           .target = "",
                           .short_desc = "",
                           .long_desc = ""};
    char *out_id = NULL;
    nhp_err err = mkp_from_folder(repo, &base, &meta, "", NULL, 0, &out_id);
    CHECK(err == NHP_OK);
    CHECK(out_id != NULL);

    char *diff_path = NULL;
    CHECK(lib_version_diff_path(out_id, "1.0", &diff_path) == NHP_OK);
    char *diff_data = NULL;
    size_t diff_len = 0;
    CHECK(diff_path != NULL && fs_read_file(diff_path, &diff_data, &diff_len) == NHP_OK);
    CHECK(strstr(diff_data, "git edit") != NULL);
    CHECK(strstr(diff_data, "brand-new.txt") != NULL);
    CHECK(strstr(diff_data, "untracked addition") != NULL);

    free(diff_data);
    free(diff_path);
    free(out_id);
    free(edited_text);
    free(data);
    free(light_path);
    make_tree_writable(repo);
    (void)fs_remove_tree(repo);
    free(repo);
    unsetenv("XDG_DATA_HOME");
    make_tree_writable(tmp_data_dir);
    (void)fs_remove_tree(tmp_data_dir);
    free(tmp_data_dir);
}

int main(void)
{
    test_ignore_general();
    test_ignore_version_specific();
    test_ignore_extra_patterns();
    test_structural_vs_build_output();
    test_looks_like_source();
    test_looks_binary();
    test_header_deterministic();
    test_compare_and_diff_files();
    test_combined_patch();
    test_combined_excludes_disabled();
    test_combined_excludes_hidden_build_output();
    test_changes_empty_on_a_fresh_instance();
    test_capture_roundtrip();
    test_capture_nothing_selected();
    test_export_patch();
    test_from_folder_never_modifies_it();
    test_from_git_ref();

    if (failures > 0) {
        (void)fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    (void)printf("ok: all mkpatch tests passed\n");
    return 0;
}
