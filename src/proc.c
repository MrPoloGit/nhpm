/* proc.c - fork/exec with captured output, no shell.
 * SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "proc.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define PIPE_READ  0
#define PIPE_WRITE 1

static void close_fd(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

/* Child side: chdir, apply extra_env, and exec. Never returns -- always
 * _exit()s, even on failure, so it never runs the parent's cleanup code. */
static void child_exec_common(const char *const *argv, const char *cwd,
                              const char *const *extra_env)
{
    if (cwd != NULL && chdir(cwd) != 0) {
        _exit(127);
    }

    if (extra_env != NULL) {
        for (size_t i = 0; extra_env[i] != NULL; i++) {
            char *eq = strchr(extra_env[i], '=');
            if (eq == NULL) {
                continue;
            }
            char *key = str_ndup(extra_env[i], (size_t)(eq - extra_env[i]));
            if (key != NULL) {
                setenv(key, eq + 1, 1);
                free(key);
            }
        }
    }

    /* execvp's second parameter isn't const-qualified for historical
     * reasons; we never modify argv through it. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-qual"
    execvp(argv[0], (char *const *)argv);
#pragma GCC diagnostic pop
    _exit(127);
}

/* Child side for a captured run: wire up the pipes, then as above. */
static void child_exec(const char *const *argv, const char *cwd, const char *const *extra_env,
                       int out_write, int err_write)
{
    if (dup2(out_write, STDOUT_FILENO) < 0 || dup2(err_write, STDERR_FILENO) < 0) {
        _exit(127);
    }
    close(out_write);
    close(err_write);
    child_exec_common(argv, cwd, extra_env);
}

/* Feeds `chunk` (len bytes, from `is_err`'s stream) through `line_acc`,
 * calling on_line for each complete line and leaving a trailing partial
 * line buffered in *line_acc for the next call. */
static void feed_lines(buf *line_acc, const char *chunk, size_t len, bool is_err,
                       proc_line_fn on_line, void *user_data, bool *cancel)
{
    buf_append(line_acc, chunk, len);

    char *start = line_acc->data;
    char *end = line_acc->data + line_acc->len;
    for (;;) {
        char *nl = memchr(start, '\n', (size_t)(end - start));
        if (nl == NULL) {
            break;
        }
        *nl = '\0';
        if (!on_line(user_data, is_err, start)) {
            *cancel = true;
        }
        start = nl + 1;
    }

    size_t remaining = (size_t)(end - start);
    memmove(line_acc->data, start, remaining);
    line_acc->len = remaining;
    if (line_acc->data != NULL) {
        line_acc->data[remaining] = '\0';
    }
}

/* Reads both pipes until the child closes them, accumulating output in
 * out_buf/err_buf and (if on_line is given) feeding complete lines to it. */
static void pump_output(pid_t pid, int out_read, int err_read, proc_line_fn on_line,
                        void *user_data, buf *out_buf, buf *err_buf)
{
    buf out_lines;
    buf err_lines;
    buf_init(&out_lines);
    buf_init(&err_lines);

    bool cancel = false;
    int open_count = 2;
    while (open_count > 0) {
        struct pollfd fds[2];
        fds[0].fd = out_read;
        fds[0].events = POLLIN;
        fds[1].fd = err_read;
        fds[1].events = POLLIN;

        int n = poll(fds, 2, -1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        for (int i = 0; i < 2; i++) {
            unsigned revents = (unsigned)fds[i].revents;
            if (fds[i].fd < 0 || (revents & (unsigned)(POLLIN | POLLHUP | POLLERR)) == 0) {
                continue;
            }

            char chunk[4096];
            ssize_t got = read(fds[i].fd, chunk, sizeof(chunk));
            bool is_err = (i == 1);
            if (got <= 0) {
                close(fds[i].fd);
                fds[i].fd = -1;
                if (is_err) {
                    err_read = -1;
                } else {
                    out_read = -1;
                }
                open_count--;
                continue;
            }

            buf_append(is_err ? err_buf : out_buf, chunk, (size_t)got);
            if (on_line != NULL) {
                feed_lines(is_err ? &err_lines : &out_lines, chunk, (size_t)got, is_err, on_line,
                           user_data, &cancel);
            }
        }

        if (cancel) {
            kill(pid, SIGTERM);
            cancel = false; /* already sent; don't resend every loop */
        }
    }

    close_fd(&out_read);
    close_fd(&err_read);
    buf_free(&out_lines);
    buf_free(&err_lines);
}

/* Logs the arguments and outcome of a finished command, per docs/style.md
 * section 6.9 ("every external command run... is logged with its arguments
 * and exit code"). */
static void log_result(const char *const *argv, const proc_result *out)
{
    buf args_buf;
    buf_init(&args_buf);
    for (size_t i = 0; argv[i] != NULL; i++) {
        if (i > 0) {
            buf_append_str(&args_buf, " ");
        }
        buf_append_str(&args_buf, argv[i]);
    }
    char *args_str = buf_take(&args_buf);
    if (out->signaled) {
        log_debug("proc", "%s -> killed by signal %d", args_str, out->term_signal);
    } else {
        log_debug("proc", "%s -> exit %d", args_str, out->exit_code);
    }
    free(args_str);
}

/* Creates the stdout/stderr pipes and forks, leaving the child running
 * child_exec() and the parent holding the read ends in out_pipe[PIPE_READ]
 * / err_pipe[PIPE_READ] (the write ends are already closed). */
static nhp_err spawn(const char *const *argv, const char *cwd, const char *const *extra_env,
                     int out_pipe[2], int err_pipe[2], pid_t *out_pid)
{
    out_pipe[0] = out_pipe[1] = -1;
    err_pipe[0] = err_pipe[1] = -1;
    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        err_set("could not create pipes for %s: %s", argv[0], strerror(errno));
        close_fd(&out_pipe[PIPE_READ]);
        close_fd(&out_pipe[PIPE_WRITE]);
        close_fd(&err_pipe[PIPE_READ]);
        close_fd(&err_pipe[PIPE_WRITE]);
        return NHP_EIO;
    }

    pid_t pid = fork();
    if (pid < 0) {
        err_set("could not fork to run %s: %s", argv[0], strerror(errno));
        close_fd(&out_pipe[PIPE_READ]);
        close_fd(&out_pipe[PIPE_WRITE]);
        close_fd(&err_pipe[PIPE_READ]);
        close_fd(&err_pipe[PIPE_WRITE]);
        return NHP_EIO;
    }

    if (pid == 0) {
        close(out_pipe[PIPE_READ]);
        close(err_pipe[PIPE_READ]);
        child_exec(argv, cwd, extra_env, out_pipe[PIPE_WRITE], err_pipe[PIPE_WRITE]);
        /* unreachable: child_exec always _exit()s */
    }

    close_fd(&out_pipe[PIPE_WRITE]);
    close_fd(&err_pipe[PIPE_WRITE]);
    *out_pid = pid;
    return NHP_OK;
}

/* Waits for `pid`, filling in *out_signaled and *out_code (either the
 * exit code or, if signaled, the signal number) -- shared by every kind
 * of run. */
static nhp_err wait_for_child(pid_t pid, const char *const *argv, bool *out_signaled, int *out_code)
{
    int status = 0;
    pid_t wait_result;
    do {
        wait_result = waitpid(pid, &status, 0);
    } while (wait_result < 0 && errno == EINTR);

    if (wait_result < 0) {
        err_set("could not wait for %s: %s", argv[0], strerror(errno));
        return NHP_EIO;
    }

    *out_signaled = WIFSIGNALED(status);
    *out_code = *out_signaled ? WTERMSIG(status) : WEXITSTATUS(status);
    return NHP_OK;
}

static nhp_err run_argv(const char *const *argv, const char *cwd, const char *const *extra_env,
                        proc_line_fn on_line, void *user_data, proc_result *out)
{
    memset(out, 0, sizeof(*out));

    int out_pipe[2];
    int err_pipe[2];
    pid_t pid;
    nhp_err spawn_err = spawn(argv, cwd, extra_env, out_pipe, err_pipe, &pid);
    if (spawn_err != NHP_OK) {
        return spawn_err;
    }

    buf out_buf;
    buf err_buf;
    buf_init(&out_buf);
    buf_init(&err_buf);
    pump_output(pid, out_pipe[PIPE_READ], err_pipe[PIPE_READ], on_line, user_data, &out_buf,
                &err_buf);

    size_t out_len = out_buf.len;
    size_t err_len = err_buf.len;
    out->out_data = buf_take(&out_buf);
    out->out_len = out_len;
    out->err_data = buf_take(&err_buf);
    out->err_len = err_len;

    int code = 0;
    nhp_err err = wait_for_child(pid, argv, &out->signaled, &code);
    if (err != NHP_OK) {
        return err;
    }
    if (out->signaled) {
        out->term_signal = code;
    } else {
        out->exit_code = code;
    }

    log_result(argv, out);
    return NHP_OK;
}

nhp_err proc_run(const char *const *argv, const char *cwd, const char *const *extra_env,
                 proc_result *out)
{
    return run_argv(argv, cwd, extra_env, NULL, NULL, out);
}

nhp_err proc_run_streaming(const char *const *argv, const char *cwd, const char *const *extra_env,
                           proc_line_fn on_line, void *user_data, proc_result *out)
{
    return run_argv(argv, cwd, extra_env, on_line, user_data, out);
}

nhp_err proc_run_interactive(const char *const *argv, const char *cwd, const char *const *extra_env,
                             bool *out_signaled, int *out_code)
{
    *out_signaled = false;
    *out_code = 0;

    pid_t pid = fork();
    if (pid < 0) {
        err_set("could not fork to run %s: %s", argv[0], strerror(errno));
        return NHP_EIO;
    }

    if (pid == 0) {
        child_exec_common(argv, cwd, extra_env);
        /* unreachable: child_exec_common() always _exit()s */
    }

    nhp_err err = wait_for_child(pid, argv, out_signaled, out_code);
    if (err != NHP_OK) {
        return err;
    }

    if (*out_signaled) {
        log_debug("proc", "%s -> killed by signal %d", argv[0], *out_code);
    } else {
        log_debug("proc", "%s -> exit %d", argv[0], *out_code);
    }
    return NHP_OK;
}

void proc_result_free(proc_result *r)
{
    if (r == NULL) {
        return;
    }
    free(r->out_data);
    free(r->err_data);
    r->out_data = NULL;
    r->err_data = NULL;
}
