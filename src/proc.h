/* proc.h - fork/exec with captured output, no shell.
 * SPDX-License-Identifier: NGPL */
#ifndef NHPM_PROC_H
#define NHPM_PROC_H

#include "util.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct proc_result {
    int exit_code; /* meaningful only when !signaled */
    bool signaled;
    int term_signal; /* meaningful only when signaled */
    char *out_data;  /* captured stdout, NUL-terminated */
    size_t out_len;
    char *err_data; /* captured stderr, NUL-terminated */
    size_t err_len;
} proc_result;

/* Called once per complete line of output (stdout/stderr via
 * `is_stderr`); also captured in proc_result as usual. `line` excludes
 * the newline and is only valid during the call. False cancels (SIGTERM). */
typedef bool (*proc_line_fn)(void *user_data, bool is_stderr, const char *line);

/* Runs argv[0] with NULL-terminated args (like execvp's); no shell
 * involved. `cwd` chdirs the child first; `extra_env` adds "KEY=VALUE"
 * vars. *out is always filled in; NHP_EIO only if exec/wait failed. */
nhp_err proc_run(const char *const *argv, const char *cwd, const char *const *extra_env,
                 proc_result *out);

/* Same as proc_run(), but also calls on_line for each line of output as it
 * arrives, so a caller can stream it (e.g. into the Build screen). */
nhp_err proc_run_streaming(const char *const *argv, const char *cwd, const char *const *extra_env,
                           proc_line_fn on_line, void *user_data, proc_result *out);

/* Same contract as proc_run(), but the child inherits stdin/stdout/stderr
 * directly instead of captured -- for the pager, $EDITOR, or the game
 * itself (13.7). *out_signaled and *out_code mirror proc_result's fields. */
nhp_err proc_run_interactive(const char *const *argv, const char *cwd, const char *const *extra_env,
                             bool *out_signaled, int *out_code);

void proc_result_free(proc_result *r);

#endif /* NHPM_PROC_H */
