/* build.c - build recipes, run build, stream output, launch game.
 * SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "build.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool build_is_stale(const inst_instance *inst)
{
    if (inst->last_built[0] == '\0') {
        return true;
    }
    if (inst->stack_modified[0] == '\0') {
        return false; /* never changed since creation, so never stale */
    }
    return strcmp(inst->stack_modified, inst->last_built) > 0;
}

/* Runs one build step, appending "$ <argv>" and its output to *log,
 * streaming live too if on_line is non-NULL. Returns NHP_EIO (err_get()
 * names `label`) if it ran but failed; else proc_run_streaming()'s own error. */
static nhp_err run_build_step(const char *src_dir, const char *const *argv, const char *label,
                              proc_line_fn on_line, void *user_data, buf *log)
{
    buf cmd_buf;
    buf_init(&cmd_buf);
    for (size_t i = 0; argv[i] != NULL; i++) {
        if (i > 0) {
            buf_append_str(&cmd_buf, " ");
        }
        buf_append_str(&cmd_buf, argv[i]);
    }
    char *cmd_str = buf_take(&cmd_buf);
    buf_append_fmt(log, "$ %s\n", cmd_str);
    free(cmd_str);

    proc_result result = {0};
    nhp_err err = proc_run_streaming(argv, src_dir, NULL, on_line, user_data, &result);
    if (err != NHP_OK) {
        proc_result_free(&result);
        return err;
    }
    if (result.out_data != NULL) {
        buf_append_str(log, result.out_data);
    }
    if (result.err_data != NULL) {
        buf_append_str(log, result.err_data);
    }

    bool failed = result.signaled || result.exit_code != 0;
    if (failed) {
        if (result.signaled) {
            err_set("%s was killed by signal %d -- see build.log", label, result.term_signal);
        } else {
            err_set("%s failed (exit %d) -- see build.log", label, result.exit_code);
        }
    }
    proc_result_free(&result);
    return failed ? NHP_EIO : NHP_OK;
}

/* "make <verb> PREFIX=<install_dir> <extra_tokens...>". */
static nhp_err run_make_step(const char *src_dir, const char *verb, const char *prefix_arg,
                             char *const *extra_tokens, size_t extra_count, const char *label,
                             proc_line_fn on_line, void *user_data, buf *log)
{
    size_t n = 3 + extra_count;
    const char **argv = malloc((n + 1) * sizeof(*argv));
    if (argv == NULL) {
        return NHP_ENOMEM;
    }
    size_t i = 0;
    argv[i++] = "make";
    argv[i++] = verb;
    argv[i++] = prefix_arg;
    for (size_t j = 0; j < extra_count; j++) {
        argv[i++] = extra_tokens[j];
    }
    argv[i] = NULL;
    nhp_err err = run_build_step(src_dir, argv, label, on_line, user_data, log);
    free(argv);
    return err;
}

/* docs/features.md 10.6's unix-family recipe: setup.sh [hint_path], optional
 * "make fetch-lua" (3.7 only), then "make all"/"make install", both with
 * PREFIX=install_dir (HACKDIR is derived from it at build time) plus `make_vars`. */
static nhp_err run_unix_recipe(const char *src_dir, const char *install_dir, const char *hint_path,
                               bool fetch_lua, const char *make_vars, proc_line_fn on_line,
                               void *user_data, buf *log)
{
    const char *setup_argv[4];
    size_t n = 0;
    setup_argv[n++] = "sh";
    setup_argv[n++] = "sys/unix/setup.sh";
    if (hint_path != NULL) {
        setup_argv[n++] = hint_path;
    }
    setup_argv[n] = NULL;
    nhp_err err = run_build_step(src_dir, setup_argv, "setup", on_line, user_data, log);
    if (err != NHP_OK) {
        return err;
    }

    if (fetch_lua) {
        const char *lua_argv[] = {"make", "fetch-lua", NULL};
        err = run_build_step(src_dir, lua_argv, "fetch-lua", on_line, user_data, log);
        if (err != NHP_OK) {
            return err;
        }
    }

    char **extra_tokens = NULL;
    size_t extra_count = 0;
    err = str_split_whitespace(make_vars, &extra_tokens, &extra_count);
    if (err != NHP_OK) {
        return err;
    }

    buf prefix_buf;
    buf_init(&prefix_buf);
    buf_append_fmt(&prefix_buf, "PREFIX=%s", install_dir);
    char *prefix_arg = buf_take(&prefix_buf);

    err = run_make_step(src_dir, "all", prefix_arg, extra_tokens, extra_count, "build", on_line,
                        user_data, log);
    if (err == NHP_OK) {
        err = run_make_step(src_dir, "install", prefix_arg, extra_tokens, extra_count, "install",
                            on_line, user_data, log);
    }

    free(prefix_arg);
    str_tokens_free(extra_tokens, extra_count);
    return err;
}

/* docs/features.md 10.6's `custom` recipe: splits `commands`
 * (instance.conf's `custom_commands`) and runs each in order, stopping at
 * the first failure. NHP_EINVAL if there are none. */
static nhp_err run_custom_recipe(const char *src_dir, const char *install_dir,
                                 const char *commands, proc_line_fn on_line, void *user_data,
                                 buf *log)
{
    if (commands == NULL || commands[0] == '\0') {
        err_set("recipe 'custom' has no custom_commands set in instance.conf");
        return NHP_EINVAL;
    }

    char *copy = str_replace_all(commands, "$PREFIX", install_dir);
    if (copy == NULL) {
        return NHP_ENOMEM;
    }

    nhp_err err = NHP_OK;
    int step = 0;
    char *saveptr = NULL;
    char *line = strtok_r(copy, ";", &saveptr);
    while (line != NULL && err == NHP_OK) {
        char **tokens = NULL;
        size_t count = 0;
        err = str_split_whitespace(line, &tokens, &count);
        if (err == NHP_OK && count > 0) {
            char **argv = malloc((count + 1) * sizeof(*argv));
            if (argv == NULL) {
                err = NHP_ENOMEM;
            } else {
                memcpy(argv, tokens, count * sizeof(*argv));
                argv[count] = NULL;
                step++;
                char label[32];
                (void)snprintf(label, sizeof(label), "custom step %d", step);
                err = run_build_step(src_dir, (const char *const *)argv, label, on_line,
                                     user_data, log);
                free(argv);
            }
        }
        str_tokens_free(tokens, count);
        line = strtok_r(NULL, ";", &saveptr);
    }

    free(copy);
    return err;
}

static nhp_err run_recipe(const char *recipe, const char *src_dir, const char *install_dir,
                          const char *make_vars, const char *custom_commands, proc_line_fn on_line,
                          void *user_data, buf *log)
{
    if (str_eq(recipe, "nethack-3.4-unix")) {
        return run_unix_recipe(src_dir, install_dir, NULL, false, make_vars, on_line, user_data,
                               log);
    }
    if (str_eq(recipe, "nethack-3.6-linux")) {
        return run_unix_recipe(src_dir, install_dir, "sys/unix/hints/linux", false, make_vars,
                               on_line, user_data, log);
    }
    if (str_eq(recipe, "nethack-3.7-linux")) {
        return run_unix_recipe(src_dir, install_dir, "sys/unix/hints/linux.370", true, make_vars,
                               on_line, user_data, log);
    }
    if (str_eq(recipe, "custom")) {
        return run_custom_recipe(src_dir, install_dir, custom_commands, on_line, user_data, log);
    }
    err_set("recipe '%s' has no built-in build steps yet", recipe);
    return NHP_EINVAL;
}

nhp_err build_run(inst_instance *inst, proc_line_fn on_line, void *user_data)
{
    char *src_dir = inst_source_full_path(inst);
    if (src_dir == NULL) {
        return NHP_ENOMEM;
    }
    char *inst_dir_path = inst_dir(inst->name);
    char *install_dir =
        (inst_dir_path != NULL) ? path_join(inst_dir_path, inst->install_dir) : NULL;
    if (install_dir == NULL) {
        free(src_dir);
        free(inst_dir_path);
        return NHP_ENOMEM;
    }

    buf log;
    buf_init(&log);
    nhp_err err =
        run_recipe(inst->recipe, src_dir, install_dir, inst->make_vars, inst->custom_commands,
                  on_line, user_data, &log);

    char *log_text = buf_take(&log);
    char *log_path = path_join(inst_dir_path, "build.log");
    if (log_path != NULL) {
        (void)fs_write_atomic(log_path, log_text, strlen(log_text));
    }
    free(log_path);
    free(log_text);
    free(inst_dir_path);
    free(install_dir);
    free(src_dir);

    if (err != NHP_OK) {
        return err;
    }

    free(inst->last_built);
    inst->last_built = time_now_iso8601();
    if (inst->last_built == NULL) {
        return NHP_ENOMEM;
    }
    return inst_save(inst);
}

nhp_err build_play(inst_instance *inst)
{
    char *inst_dir_path = inst_dir(inst->name);
    char *install_dir =
        (inst_dir_path != NULL) ? path_join(inst_dir_path, inst->install_dir) : NULL;
    free(inst_dir_path);
    if (install_dir == NULL) {
        return NHP_ENOMEM;
    }
    char *games_dir = path_join(install_dir, "games");
    char *game_path = (games_dir != NULL) ? path_join(games_dir, inst->game_name) : NULL;
    free(games_dir);
    free(install_dir);
    if (game_path == NULL) {
        return NHP_ENOMEM;
    }

    if (!fs_exists(game_path)) {
        err_set("%s hasn't been built yet", inst->name);
        free(game_path);
        return NHP_ENOENT;
    }

    const char *argv[] = {game_path, NULL};
    bool signaled = false;
    int code = 0;
    nhp_err err = proc_run_interactive(argv, NULL, NULL, &signaled, &code);
    free(game_path);
    if (err != NHP_OK) {
        return err;
    }

    free(inst->last_played);
    inst->last_played = time_now_iso8601();
    if (inst->last_played == NULL) {
        return NHP_ENOMEM;
    }
    return inst_save(inst);
}
