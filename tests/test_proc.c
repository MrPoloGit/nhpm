/* test_proc.c - unit tests for proc.c's fork/exec wrapper (no network, no
 * terminal).
 * SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "proc.h"
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

static void test_basic_run(void)
{
    const char *argv[] = {"/bin/echo", "hello", "world", NULL};
    proc_result r;
    CHECK(proc_run(argv, NULL, NULL, &r) == NHP_OK);
    CHECK(!r.signaled);
    CHECK(r.exit_code == 0);
    CHECK(r.out_data != NULL && strcmp(r.out_data, "hello world\n") == 0);
    proc_result_free(&r);
}

static void test_exit_code(void)
{
    const char *argv[] = {"/usr/bin/false", NULL};
    proc_result r;
    CHECK(proc_run(argv, NULL, NULL, &r) == NHP_OK);
    CHECK(!r.signaled);
    CHECK(r.exit_code != 0);
    proc_result_free(&r);
}

static void test_no_shell_injection(void)
{
    /* Checks that an argument with shell metacharacters is passed
     * through literally -- they'd be interpreted if proc_run used a
     * shell instead of execvp. */
    const char *argv[] = {"/bin/echo", "a;b$(echo c)", NULL};
    proc_result r;
    CHECK(proc_run(argv, NULL, NULL, &r) == NHP_OK);
    CHECK(r.out_data != NULL && strcmp(r.out_data, "a;b$(echo c)\n") == 0);
    proc_result_free(&r);
}

static bool collect_line(void *user_data, bool is_stderr, const char *line)
{
    (void)is_stderr;
    (void)line;
    int *count = user_data;
    (*count)++;
    return true;
}

static void test_streaming(void)
{
    const char *argv[] = {"/bin/sh", "-c", "echo one; echo two; echo three", NULL};
    int line_count = 0;
    proc_result r;
    CHECK(proc_run_streaming(argv, NULL, NULL, collect_line, &line_count, &r) == NHP_OK);
    CHECK(line_count == 3);
    proc_result_free(&r);
}

static void test_extra_env(void)
{
    const char *argv[] = {"/bin/sh", "-c", "echo $NHP_TEST_VAR", NULL};
    const char *env[] = {"NHP_TEST_VAR=hello-env", NULL};
    proc_result r;
    CHECK(proc_run(argv, NULL, env, &r) == NHP_OK);
    CHECK(r.out_data != NULL && strcmp(r.out_data, "hello-env\n") == 0);
    proc_result_free(&r);
}

static void test_cwd(void)
{
    const char *argv[] = {"/bin/sh", "-c", "pwd -P", NULL};
    proc_result r;
    CHECK(proc_run(argv, "/tmp", NULL, &r) == NHP_OK);
    CHECK(r.out_data != NULL && strstr(r.out_data, "tmp") != NULL);
    proc_result_free(&r);
}

static void test_interactive(void)
{
    const char *true_argv[] = {"/usr/bin/true", NULL};
    bool signaled = false;
    int code = -1;
    CHECK(proc_run_interactive(true_argv, NULL, NULL, &signaled, &code) == NHP_OK);
    CHECK(!signaled);
    CHECK(code == 0);

    const char *false_argv[] = {"/usr/bin/false", NULL};
    CHECK(proc_run_interactive(false_argv, NULL, NULL, &signaled, &code) == NHP_OK);
    CHECK(!signaled);
    CHECK(code != 0);

    const char *cwd_argv[] = {"/bin/sh", "-c", "test -f ./test_proc.c", NULL};
    char cwd[1024];
    CHECK(getcwd(cwd, sizeof(cwd)) != NULL);
    char *tests_dir = path_join(cwd, "tests");
    CHECK(proc_run_interactive(cwd_argv, tests_dir, NULL, &signaled, &code) == NHP_OK);
    CHECK(!signaled);
    CHECK(code == 0);
    free(tests_dir);
}

int main(void)
{
    test_basic_run();
    test_exit_code();
    test_no_shell_injection();
    test_streaming();
    test_extra_env();
    test_cwd();
    test_interactive();

    if (failures == 0) {
        (void)printf("ok: all proc tests passed\n");
    } else {
        (void)printf("FAILED: %d check(s) failed\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
