/* test_instance.c - unit tests for instance.c (no network, no terminal,
 * no quilt -- that's stack.c's job, M4).
 * SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "config.h"
#include "instance.h"
#include "sources.h"
#include "util.h"

#include <dirent.h>
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

/* Every test runs under a fresh XDG_DATA_HOME with base source 3.6.7
 * already imported, since instances are created from a base source. */
#define WITH_TEMP_INSTANCES(body)                                                                  \
    do {                                                                                           \
        char *tmp_data_dir = fs_make_temp_dir("nhp-test-data");                                    \
        CHECK(tmp_data_dir != NULL);                                                               \
        setenv("XDG_DATA_HOME", tmp_data_dir, 1);                                                  \
        char *src_version = NULL;                                                                  \
        CHECK(bsrc_add_from_folder("tests/fixtures/source367", NULL, &src_version) == NHP_OK);     \
        free(src_version);                                                                         \
        body;                                                                                      \
        unsetenv("XDG_DATA_HOME");                                                                 \
        make_tree_writable(tmp_data_dir);                                                          \
        fs_remove_tree(tmp_data_dir);                                                              \
        free(tmp_data_dir);                                                                        \
    } while (0)

/* base source trees are read-only; restore write permission recursively
 * before the temp dir cleanup, so fs_remove_tree() can delete it. */
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

static void test_new_and_load_round_trip(void)
{
    WITH_TEMP_INSTANCES({
        inst_instance inst;
        nhp_err err = inst_new("myinst", "3.6.7", "nethack-3.6-linux", &inst);
        CHECK(err == NHP_OK);
        CHECK(str_eq(inst.name, "myinst"));
        CHECK(inst.kind == INST_MANAGED);
        CHECK(str_eq(inst.source, "3.6.7"));
        CHECK(str_eq(inst.source_path, "src"));
        CHECK(str_eq(inst.recipe, "nethack-3.6-linux"));
        CHECK(str_eq(inst.custom_commands, ""));
        CHECK(str_eq(inst.install_dir, "install"));
        CHECK(str_eq(inst.game_name, "nethack"));
        CHECK(inst.patch_count == 0);
        CHECK(strlen(inst.created) > 0);

        free(inst.custom_commands);
        inst.custom_commands = str_dup("make all;make install PREFIX=/tmp/x");
        CHECK(inst_save(&inst) == NHP_OK);
        inst_free(&inst);

        /* the working copy must be writable, unlike the base source */
        char *dir = inst_dir("myinst");
        char *marker = path_join(dir, "src/include/patchlevel.h");
        char *data = NULL;
        size_t len = 0;
        CHECK(fs_read_file(marker, &data, &len) == NHP_OK);
        CHECK(fs_write_atomic(marker, data, len) == NHP_OK);
        free(data);
        free(marker);
        free(dir);

        CHECK(inst_load("myinst", &inst) == NHP_OK);
        CHECK(str_eq(inst.name, "myinst"));
        CHECK(str_eq(inst.recipe, "nethack-3.6-linux"));
        CHECK(str_eq(inst.custom_commands, "make all;make install PREFIX=/tmp/x"));
        inst_free(&inst);
    });
}

static void test_new_rejects_duplicate_and_bad_name(void)
{
    WITH_TEMP_INSTANCES({
        inst_instance inst;
        CHECK(inst_new("myinst", "3.6.7", "", &inst) == NHP_OK);
        inst_free(&inst);

        CHECK(inst_new("myinst", "3.6.7", "", &inst) == NHP_EEXIST);
        CHECK(inst_new("bad name!", "3.6.7", "", &inst) == NHP_EINVAL);
        CHECK(inst_new("../escape", "3.6.7", "", &inst) == NHP_EINVAL);
        CHECK(inst_new("missing", "9.9.9", "", &inst) == NHP_ENOENT);
    });
}

static void test_save_bak_and_round_trip(void)
{
    WITH_TEMP_INSTANCES({
        inst_instance inst;
        CHECK(inst_new("myinst", "3.6.7", "", &inst) == NHP_OK);

        char *dir = inst_dir("myinst");
        char *conf_path = path_join(dir, "instance.conf");
        char *bak_path = path_join(dir, "instance.conf.bak");
        CHECK(fs_exists(conf_path));
        CHECK(!fs_exists(bak_path)); /* no .bak yet: this is the first save */

        char *first_modified = str_dup(inst.modified);
        free(inst.notes);
        inst.notes = str_dup("first note");
        CHECK(inst_save(&inst) == NHP_OK);
        CHECK(fs_exists(bak_path)); /* now there's a .bak of the pre-edit file */
        inst_free(&inst);
        free(first_modified);

        CHECK(inst_load("myinst", &inst) == NHP_OK);
        CHECK(str_eq(inst.notes, "first note"));
        inst_free(&inst);

        free(dir);
        free(conf_path);
        free(bak_path);
    });
}

static void test_patch_list_round_trip(void)
{
    WITH_TEMP_INSTANCES({
        inst_instance inst;
        CHECK(inst_new("myinst", "3.6.7", "", &inst) == NHP_OK);

        inst.patches = calloc(2, sizeof(*inst.patches));
        CHECK(inst.patches != NULL);
        inst.patch_count = 2;
        inst.patches[0].id = str_dup("local-menucolors");
        inst.patches[0].version = str_dup("1.5");
        inst.patches[0].enabled = true;
        inst.patches[0].strip = 1;
        inst.patches[0].sha256 =
            str_dup("81be0000000000000000000000000000000000000000000000000000000000");
        inst.patches[0].modified = false;
        inst.patches[1].id = str_dup("498");
        inst.patches[1].version = str_dup("1.2");
        inst.patches[1].enabled = false;
        inst.patches[1].strip = 0;
        inst.patches[1].sha256 =
            str_dup("2c4d0000000000000000000000000000000000000000000000000000000000");
        inst.patches[1].modified = true;

        CHECK(inst_save(&inst) == NHP_OK);
        inst_free(&inst);

        CHECK(inst_load("myinst", &inst) == NHP_OK);
        CHECK(inst.patch_count == 2);
        if (inst.patch_count == 2) {
            CHECK(str_eq(inst.patches[0].id, "local-menucolors"));
            CHECK(str_eq(inst.patches[0].version, "1.5"));
            CHECK(inst.patches[0].enabled);
            CHECK(inst.patches[0].strip == 1);
            CHECK(!inst.patches[0].modified);
            CHECK(str_eq(inst.patches[1].id, "498"));
            CHECK(!inst.patches[1].enabled);
            CHECK(inst.patches[1].strip == 0);
            CHECK(inst.patches[1].modified);
        }
        inst_free(&inst);
    });
}

static void test_list(void)
{
    WITH_TEMP_INSTANCES({
        inst_instance inst;
        CHECK(inst_new("a", "3.6.7", "", &inst) == NHP_OK);
        inst_free(&inst);
        CHECK(inst_new("b", "3.6.7", "", &inst) == NHP_OK);
        inst_free(&inst);

        inst_instance *list = NULL;
        size_t count = 0;
        CHECK(inst_list(&list, &count) == NHP_OK);
        CHECK(count == 2);
        inst_list_free(list, count);
    });
}

static void test_list_empty(void)
{
    WITH_TEMP_INSTANCES({
        inst_instance *list = NULL;
        size_t count = 0;
        CHECK(inst_list(&list, &count) == NHP_OK);
        CHECK(count == 0);
        inst_list_free(list, count);
    });
}

static void test_check(void)
{
    WITH_TEMP_INSTANCES({
        inst_instance inst;
        CHECK(inst_new("myinst", "3.6.7", "", &inst) == NHP_OK);

        inst_check_result result;
        CHECK(inst_check(&inst, &result) == NHP_OK);
        CHECK(result.ok);
        CHECK(result.source_path_exists);
        inst_free(&inst);

        /* break it: remove the working copy out from under instance.conf */
        char *dir = inst_dir("myinst");
        char *src_dir = path_join(dir, "src");
        fs_remove_tree(src_dir);
        free(src_dir);
        free(dir);

        CHECK(inst_load("myinst", &inst) == NHP_OK);
        CHECK(inst_check(&inst, &result) == NHP_OK);
        CHECK(!result.ok);
        CHECK(!result.source_path_exists);
        inst_free(&inst);
    });
}

static void test_clone(void)
{
    WITH_TEMP_INSTANCES({
        inst_instance inst;
        CHECK(inst_new("myinst", "3.6.7", "nethack-3.6-linux", &inst) == NHP_OK);
        inst_free(&inst);

        CHECK(inst_clone("myinst", "myinst-clone", false) == NHP_OK);
        CHECK(inst_load("myinst-clone", &inst) == NHP_OK);
        CHECK(str_eq(inst.name, "myinst-clone"));
        CHECK(str_eq(inst.recipe, "nethack-3.6-linux"));
        inst_free(&inst);

        /* the original is untouched */
        CHECK(inst_load("myinst", &inst) == NHP_OK);
        CHECK(str_eq(inst.name, "myinst"));
        inst_free(&inst);

        CHECK(inst_clone("myinst", "myinst-clone", false) == NHP_EEXIST);
        CHECK(inst_clone("no-such-instance", "whatever", false) == NHP_ENOENT);
    });
}

static void test_rename(void)
{
    WITH_TEMP_INSTANCES({
        inst_instance inst;
        CHECK(inst_new("old-name", "3.6.7", "", &inst) == NHP_OK);
        inst_free(&inst);

        CHECK(inst_rename("old-name", "new-name") == NHP_OK);
        CHECK(inst_load("new-name", &inst) == NHP_OK);
        CHECK(str_eq(inst.name, "new-name"));
        inst_free(&inst);

        CHECK(inst_load("old-name", &inst) == NHP_ENOENT);
        CHECK(inst_rename("new-name", "bad name!") == NHP_EINVAL);
    });
}

static void test_delete_keep_and_discard_saves(void)
{
    WITH_TEMP_INSTANCES({
        inst_instance inst;
        CHECK(inst_new("keepme", "3.6.7", "", &inst) == NHP_OK);
        inst_free(&inst);
        CHECK(inst_new("dropme", "3.6.7", "", &inst) == NHP_OK);
        inst_free(&inst);

        char *keep_dir = inst_dir("keepme");
        char *keep_install = path_join(keep_dir, "install");
        char *save_marker = path_join(keep_install, "save.dat");
        CHECK(fs_write_atomic(save_marker, "a saved game\n", 13) == NHP_OK);

        CHECK(inst_delete("keepme", true) == NHP_OK);
        CHECK(!fs_exists(keep_dir));

        char *data_dir = cfg_xdg_data_dir();
        char *orphaned_root = path_join(data_dir, "orphaned-saves");
        DIR *dp = opendir(orphaned_root);
        bool found_orphan_with_save = false;
        if (dp != NULL) {
            struct dirent *entry;
            while ((entry = readdir(dp)) != NULL) {
                if (strncmp(entry->d_name, "keepme-", 7) == 0) {
                    char *orphan_dir = path_join(orphaned_root, entry->d_name);
                    char *orphan_save = path_join(orphan_dir, "save.dat");
                    if (fs_exists(orphan_save)) {
                        found_orphan_with_save = true;
                    }
                    free(orphan_dir);
                    free(orphan_save);
                }
            }
            (void)closedir(dp);
        }
        CHECK(found_orphan_with_save);
        free(data_dir);
        free(orphaned_root);
        free(keep_install);
        free(save_marker);
        free(keep_dir);

        char *drop_dir = inst_dir("dropme");
        CHECK(inst_delete("dropme", false) == NHP_OK);
        CHECK(!fs_exists(drop_dir));
        free(drop_dir);

        CHECK(inst_delete("no-such-instance", false) == NHP_ENOENT);
    });
}

static void test_snapshots(void)
{
    WITH_TEMP_INSTANCES({
        inst_instance inst;
        CHECK(inst_new("myinst", "3.6.7", "", &inst) == NHP_OK);
        inst_free(&inst);

        char *snap1 = NULL;
        CHECK(inst_snapshot_create("myinst", "before changes", &snap1) == NHP_OK);
        CHECK(snap1 != NULL);

        inst_snapshot *snaps = NULL;
        size_t count = 0;
        CHECK(inst_snapshot_list("myinst", &snaps, &count) == NHP_OK);
        CHECK(count == 1);
        if (count == 1) {
            CHECK(str_eq(snaps[0].filename, snap1));
            CHECK(str_eq(snaps[0].label, "before-changes"));
            CHECK(strlen(snaps[0].created) > 0);
        }
        inst_snapshot_list_free(snaps, count);

        CHECK(inst_load("myinst", &inst) == NHP_OK);
        free(inst.notes);
        inst.notes = str_dup("changed after the snapshot");
        CHECK(inst_save(&inst) == NHP_OK);
        inst_free(&inst);

        CHECK(inst_snapshot_restore("myinst", snap1) == NHP_OK);
        CHECK(inst_load("myinst", &inst) == NHP_OK);
        CHECK(str_eq(inst.notes, ""));
        inst_free(&inst);

        /* restoring takes its own snapshot first, so there are now two */
        CHECK(inst_snapshot_list("myinst", &snaps, &count) == NHP_OK);
        CHECK(count == 2);
        inst_snapshot_list_free(snaps, count);

        CHECK(inst_snapshot_delete("myinst", snap1) == NHP_OK);
        CHECK(inst_snapshot_delete("myinst", snap1) == NHP_ENOENT);
        CHECK(inst_snapshot_restore("myinst", "../../etc/passwd") == NHP_EINVAL);
        CHECK(inst_snapshot_delete("myinst", "../../etc/passwd") == NHP_EINVAL);

        free(snap1);
    });
}

static void test_export_import(void)
{
    WITH_TEMP_INSTANCES({
        inst_instance inst;
        CHECK(inst_new("myinst", "3.6.7", "nethack-3.6-linux", &inst) == NHP_OK);
        free(inst.notes);
        inst.notes = str_dup("exported notes");
        CHECK(inst_save(&inst) == NHP_OK);
        inst_free(&inst);

        char *export_dir = fs_make_temp_dir("nhp-test-export");
        char *nhinst_path = path_join(export_dir, "myinst.nhinst");
        CHECK(inst_export("myinst", nhinst_path) == NHP_OK);
        CHECK(fs_exists(nhinst_path));

        inst_instance imported;
        CHECK(inst_import(nhinst_path, "myinst-imported", "3.6.7", &imported) == NHP_OK);
        CHECK(str_eq(imported.name, "myinst-imported"));
        CHECK(str_eq(imported.recipe, "nethack-3.6-linux"));
        CHECK(str_eq(imported.notes, "exported notes"));
        inst_free(&imported);

        /* source_version NULL means "whatever the .nhinst itself says" */
        inst_instance imported2;
        CHECK(inst_import(nhinst_path, "myinst-imported-auto", NULL, &imported2) == NHP_OK);
        CHECK(str_eq(imported2.source, "3.6.7"));
        inst_free(&imported2);

        fs_remove_tree(export_dir);
        free(export_dir);
        free(nhinst_path);
    });
}

static void test_locking(void)
{
    WITH_TEMP_INSTANCES({
        inst_instance inst;
        CHECK(inst_new("myinst", "3.6.7", "", &inst) == NHP_OK);
        inst_free(&inst);

        inst_lock lock1;
        pid_t other_pid = 0;
        CHECK(inst_lock_acquire("myinst", &lock1, &other_pid) == NHP_OK);

        inst_lock lock2;
        other_pid = 0;
        CHECK(inst_lock_acquire("myinst", &lock2, &other_pid) == NHP_ELOCKED);
        CHECK(other_pid == getpid()); /* we locked it ourselves, from this same process */

        inst_lock_release(&lock1);
        CHECK(inst_lock_acquire("myinst", &lock2, &other_pid) == NHP_OK);
        inst_lock_release(&lock2);
    });
}

static void test_link(void)
{
    WITH_TEMP_INSTANCES({
        inst_instance inst;
        nhp_err err = inst_link("linked1", "tests/fixtures/source343", "nethack-3.4-unix", &inst);
        CHECK(err == NHP_OK);
        CHECK(inst.kind == INST_LINKED);
        CHECK(str_eq(inst.source, "3.4.3"));
        CHECK(str_eq(inst.source_path, "tests/fixtures/source343"));

        /* nhpm never touches a linked instance's source folder */
        char *dir = inst_dir("linked1");
        char *src_copy = path_join(dir, "src");
        CHECK(!fs_exists(src_copy));
        free(src_copy);
        free(dir);

        inst_check_result result;
        CHECK(inst_check(&inst, &result) == NHP_OK);
        CHECK(result.ok);
        inst_free(&inst);

        CHECK(inst_link("linked2", "/does/not/exist", "", &inst) == NHP_ENOENT);
    });
}

/* fs_write_atomic()'s pattern (write temp, fsync, rename) means a crash
 * mid-save leaves only a stray temp file -- instance.conf is untouched.
 * The fuller SIGKILL-mid-Quilt-operation test waits for stack.c (M4). */
static void test_crash_mid_save_leaves_instance_conf_untouched(void)
{
    WITH_TEMP_INSTANCES({
        inst_instance inst;
        CHECK(inst_new("myinst", "3.6.7", "", &inst) == NHP_OK);
        free(inst.notes);
        inst.notes = str_dup("original notes, from before the crash");
        CHECK(inst_save(&inst) == NHP_OK);
        inst_free(&inst);

        char *dir = inst_dir("myinst");
        char *conf_path = path_join(dir, "instance.conf");
        char *before_data = NULL;
        size_t before_len = 0;
        CHECK(fs_read_file(conf_path, &before_data, &before_len) == NHP_OK);

        /* Simulates a save killed after mkstemp() but before the rename
         * that would have replaced instance.conf. */
        char *stray = path_join(dir, ".nhpm-tmp-simulated-crash");
        FILE *fp = fopen(stray, "w");
        CHECK(fp != NULL);
        if (fp != NULL) {
            (void)fwrite("format = 1\n[instance]\nname = truncated-garb", 1, 30, fp);
            (void)fclose(fp);
        }

        CHECK(inst_load("myinst", &inst) == NHP_OK);
        CHECK(str_eq(inst.notes, "original notes, from before the crash"));
        inst_free(&inst);

        char *after_data = NULL;
        size_t after_len = 0;
        CHECK(fs_read_file(conf_path, &after_data, &after_len) == NHP_OK);
        CHECK(before_len == after_len && memcmp(before_data, after_data, before_len) == 0);

        free(before_data);
        free(after_data);
        free(stray);
        free(conf_path);
        free(dir);
    });
}

int main(void)
{
    test_new_and_load_round_trip();
    test_new_rejects_duplicate_and_bad_name();
    test_save_bak_and_round_trip();
    test_patch_list_round_trip();
    test_list();
    test_list_empty();
    test_check();
    test_clone();
    test_rename();
    test_delete_keep_and_discard_saves();
    test_snapshots();
    test_export_import();
    test_locking();
    test_link();
    test_crash_mid_save_leaves_instance_conf_untouched();

    if (failures == 0) {
        (void)printf("ok: all instance tests passed\n");
    } else {
        (void)printf("FAILED: %d check(s) failed\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
