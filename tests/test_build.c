/* test_build.c - unit/integration tests for build.c, including a real
 * build+play cycle against the fake NetHack fixture (docs/testing.md
 * 17.2). SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "build.h"
#include "instance.h"
#include "sources.h"
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

#define WITH_TEMP_INSTANCE(body)                                                                   \
    do {                                                                                           \
        char *tmp_data_dir = fs_make_temp_dir("nhp-test-build");                                   \
        CHECK(tmp_data_dir != NULL);                                                               \
        setenv("XDG_DATA_HOME", tmp_data_dir, 1);                                                  \
        char *src_version = NULL;                                                                  \
        CHECK(bsrc_add_from_folder("tests/fixtures/source", NULL, &src_version) == NHP_OK);        \
        free(src_version);                                                                         \
        inst_instance inst;                                                                        \
        CHECK(inst_new("buildtest", "3.6.7", "nethack-3.4-unix", &inst) == NHP_OK);                \
        body;                                                                                      \
        inst_free(&inst);                                                                          \
        unsetenv("XDG_DATA_HOME");                                                                 \
        fs_remove_tree(tmp_data_dir);                                                              \
        free(tmp_data_dir);                                                                        \
    } while (0)

static void test_is_stale(void)
{
    WITH_TEMP_INSTANCE({
        CHECK(build_is_stale(&inst)); /* never built */

        free(inst.last_built);
        inst.last_built = str_dup("2026-01-01T00:00:00Z");
        CHECK(!build_is_stale(&inst)); /* stack_modified is still "" */

        free(inst.stack_modified);
        inst.stack_modified = str_dup("2026-06-01T00:00:00Z");
        CHECK(build_is_stale(&inst)); /* stack changed after the build */

        free(inst.last_built);
        inst.last_built = str_dup("2026-12-01T00:00:00Z");
        CHECK(!build_is_stale(&inst)); /* built again, after the change */
    });
}

static void test_custom_recipe_rejected(void)
{
    WITH_TEMP_INSTANCE({
        free(inst.recipe);
        inst.recipe = str_dup("custom");
        CHECK(build_run(&inst, NULL, NULL) == NHP_EINVAL);
    });
}

static void test_custom_recipe_runs(void)
{
    WITH_TEMP_INSTANCE({
        free(inst.game_name);
        inst.game_name = str_dup("game");
        free(inst.recipe);
        inst.recipe = str_dup("custom");

        char *inst_dir_path = inst_dir("buildtest");
        char *install_dir_full = path_join(inst_dir_path, inst.install_dir);
        free(inst.custom_commands);
        inst.custom_commands = str_dup("sh sys/unix/setup.sh;make all;make install PREFIX=$PREFIX");
        CHECK(inst_save(&inst) == NHP_OK);

        nhp_err err = build_run(&inst, NULL, NULL);
        CHECK(err == NHP_OK);

        char *game_path = path_join(install_dir_full, "games/game");
        CHECK(fs_exists(game_path));
        free(game_path);

        char *log_path = path_join(inst_dir_path, "build.log");
        char *log_data = NULL;
        size_t log_len = 0;
        CHECK(fs_read_file(log_path, &log_data, &log_len) == NHP_OK);
        CHECK(strstr(log_data, "setup.sh") != NULL);
        CHECK(strstr(log_data, "$PREFIX") == NULL); /* substituted, not left literal */
        buf expect;
        buf_init(&expect);
        buf_append_fmt(&expect, "make install PREFIX=%s", install_dir_full);
        char *expect_prefix_arg = buf_take(&expect);
        CHECK(strstr(log_data, expect_prefix_arg) != NULL);
        free(expect_prefix_arg);
        free(log_data);
        free(log_path);
        free(install_dir_full);
        free(inst_dir_path);
    });
}

static void test_custom_recipe_stops_on_failure(void)
{
    WITH_TEMP_INSTANCE({
        free(inst.recipe);
        inst.recipe = str_dup("custom");
        free(inst.custom_commands);
        inst.custom_commands = str_dup("false;sh sys/unix/setup.sh");
        CHECK(inst_save(&inst) == NHP_OK);

        CHECK(build_run(&inst, NULL, NULL) == NHP_EIO);

        char *inst_dir_path = inst_dir("buildtest");
        char *log_path = path_join(inst_dir_path, "build.log");
        char *log_data = NULL;
        size_t log_len = 0;
        CHECK(fs_read_file(log_path, &log_data, &log_len) == NHP_OK);
        CHECK(strstr(log_data, "setup.sh") == NULL);
        free(log_data);
        free(log_path);
        free(inst_dir_path);
    });
}

static bool capture_line(void *user_data, bool is_stderr, const char *line)
{
    (void)is_stderr;
    buf *out = user_data;
    buf_append_str(out, line);
    buf_append_str(out, "\n");
    return true;
}

static void test_build_and_play(void)
{
    WITH_TEMP_INSTANCE({
        /* the fixture's Makefile installs to <prefix>/games/game, not
         * .../games/nethack -- point game_name at the real binary name. */
        free(inst.game_name);
        inst.game_name = str_dup("game");
        CHECK(inst_save(&inst) == NHP_OK);

        buf log;
        buf_init(&log);
        nhp_err err = build_run(&inst, capture_line, &log);
        char *streamed = buf_take(&log);
        CHECK(err == NHP_OK);
        CHECK(strstr(streamed, "game") != NULL || streamed[0] != '\0');
        free(streamed);

        CHECK(inst.last_built[0] != '\0');
        CHECK(!build_is_stale(&inst));

        char *inst_dir_path = inst_dir("buildtest");
        char *game_path = path_join(inst_dir_path, "install/games/game");
        CHECK(fs_exists(game_path));
        free(game_path);

        char *log_path = path_join(inst_dir_path, "build.log");
        char *log_data = NULL;
        size_t log_len = 0;
        CHECK(fs_read_file(log_path, &log_data, &log_len) == NHP_OK);
        CHECK(log_len > 0);
        CHECK(strstr(log_data, "setup.sh") != NULL);
        CHECK(strstr(log_data, "make all") != NULL);
        CHECK(strstr(log_data, "make install") != NULL);
        free(log_data);
        free(log_path);
        free(inst_dir_path);

        /* the fake "game" isn't interactive: it just prints and exits,
         * so play() returns immediately without needing piped input. */
        CHECK(inst.last_played[0] == '\0');
        err = build_play(&inst);
        CHECK(err == NHP_OK);
        CHECK(inst.last_played[0] != '\0');
    });
}

static void test_play_before_build_fails(void)
{
    WITH_TEMP_INSTANCE({
        nhp_err err = build_play(&inst);
        CHECK(err == NHP_ENOENT);
    });
}

static void test_make_vars_passed_to_build(void)
{
    WITH_TEMP_INSTANCE({
        free(inst.game_name);
        inst.game_name = str_dup("game");
        free(inst.make_vars);
        /* CFLAGS+=-DEXTRA_DEFINE is harmless but confirms the token
         * reaches the real "make all"/"make install" invocations. */
        inst.make_vars = str_dup("EXTRA_UNUSED_VAR=1");
        CHECK(inst_save(&inst) == NHP_OK);

        nhp_err err = build_run(&inst, NULL, NULL);
        CHECK(err == NHP_OK);

        char *inst_dir_path = inst_dir("buildtest");
        char *log_path = path_join(inst_dir_path, "build.log");
        char *log_data = NULL;
        size_t log_len = 0;
        CHECK(fs_read_file(log_path, &log_data, &log_len) == NHP_OK);
        CHECK(strstr(log_data, "EXTRA_UNUSED_VAR=1") != NULL);
        free(log_data);
        free(log_path);
        free(inst_dir_path);
    });
}

int main(void)
{
    test_is_stale();
    test_custom_recipe_rejected();
    test_custom_recipe_runs();
    test_custom_recipe_stops_on_failure();
    test_build_and_play();
    test_play_before_build_fails();
    test_make_vars_passed_to_build();

    if (failures == 0) {
        (void)printf("ok: all build tests passed\n");
    } else {
        (void)printf("FAILED: %d check(s) failed\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
