/* quilt.c - run quilt/patch as subprocesses, parse output.
 * SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "quilt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* docs/features.md section 11.2: the environment every quilt invocation
 * runs with. */
/* QUILTRC=- ignores the user's ~/.quiltrc or the system default, which can
 * set things like "--backup" (stray "<patch>~" files) or "--color=always"
 * (ANSI escapes landing in the output nhpm parses). */
static const char *const QUILT_ENV[] = {
    "QUILT_PATCHES=patches", "QUILT_SERIES=series", "QUILT_PC=.pc", "LC_ALL=C", "QUILTRC=-", NULL,
};

static bool looks_like_gnu_patch(const char *binary)
{
    const char *argv[] = {binary, "--version", NULL};
    proc_result result = {0};
    nhp_err err = proc_run(argv, NULL, NULL, &result);
    bool ok = err == NHP_OK && !result.signaled && result.exit_code == 0 &&
              result.out_data != NULL && strstr(result.out_data, "GNU patch") != NULL;
    proc_result_free(&result);
    return ok;
}

nhp_err quilt_check_available(void)
{
    const char *quilt_argv[] = {"quilt", "--version", NULL};
    proc_result result = {0};
    nhp_err err = proc_run(quilt_argv, NULL, NULL, &result);
    bool quilt_ok = err == NHP_OK && !result.signaled && result.exit_code == 0;
    proc_result_free(&result);
    if (!quilt_ok) {
        err_set("quilt is not on PATH -- see docs/tools-libraries.md for how to install it");
        return NHP_ENOENT;
    }

    if (!looks_like_gnu_patch("patch") && !looks_like_gnu_patch("gpatch")) {
        err_set("no GNU patch found on PATH (as 'patch' or 'gpatch') -- see "
                "docs/tools-libraries.md, section 3, for why this matters on macOS");
        return NHP_ENOENT;
    }
    return NHP_OK;
}

/* Splits `text` on '\n' into trimmed, non-empty lines, each with
 * `strip_prefix` removed from the front if present (quilt's own
 * QUILT_PATCHES_PREFIX setting can change the default "patches/"). */
static nhp_err split_lines(const char *text, const char *strip_prefix, char ***out,
                           size_t *out_count)
{
    *out = NULL;
    *out_count = 0;
    if (text == NULL || text[0] == '\0') {
        return NHP_OK;
    }

    char *copy = str_dup(text);
    if (copy == NULL) {
        return NHP_ENOMEM;
    }

    size_t cap = 8;
    char **names = malloc(cap * sizeof(*names));
    if (names == NULL) {
        free(copy);
        return NHP_ENOMEM;
    }

    size_t count = 0;
    char *saveptr = NULL;
    char *line = strtok_r(copy, "\n", &saveptr);
    size_t prefix_len = (strip_prefix != NULL) ? strlen(strip_prefix) : 0;
    while (line != NULL) {
        if (line[0] != '\0') {
            const char *name = line;
            if (prefix_len > 0 && strncmp(line, strip_prefix, prefix_len) == 0) {
                name = line + prefix_len;
            }
            if (count == cap) {
                cap *= 2;
                char **grown = realloc(names, cap * sizeof(*names));
                if (grown == NULL) {
                    for (size_t i = 0; i < count; i++) {
                        free(names[i]);
                    }
                    free(names);
                    free(copy);
                    return NHP_ENOMEM;
                }
                names = grown;
            }
            names[count] = str_dup(name);
            if (names[count] == NULL) {
                for (size_t i = 0; i < count; i++) {
                    free(names[i]);
                }
                free(names);
                free(copy);
                return NHP_ENOMEM;
            }
            count++;
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }

    free(copy);
    *out = names;
    *out_count = count;
    return NHP_OK;
}

void quilt_name_list_free(char **names, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        free(names[i]);
    }
    free(names);
}

nhp_err quilt_applied(const char *src_dir, char ***out_names, size_t *out_count)
{
    *out_names = NULL;
    *out_count = 0;

    const char *argv[] = {"quilt", "applied", NULL};
    proc_result result = {0};
    nhp_err err = proc_run(argv, src_dir, QUILT_ENV, &result);
    if (err != NHP_OK) {
        proc_result_free(&result);
        return err;
    }

    /* "No patches applied" or "No patches in series" are both printed to
     * stderr with exit 1, but just mean an empty stack, not a real error. */
    if (result.signaled || result.exit_code != 0) {
        bool none_applied = !result.signaled && result.err_data != NULL &&
                            (strstr(result.err_data, "No patches applied") != NULL ||
                             strstr(result.err_data, "No patches in series") != NULL);
        if (!none_applied) {
            err_set("quilt applied failed: %s", result.err_data);
            proc_result_free(&result);
            return NHP_EIO;
        }
        proc_result_free(&result);
        return NHP_OK;
    }

    err = split_lines(result.out_data, "patches/", out_names, out_count);
    proc_result_free(&result);
    return err;
}

nhp_err quilt_files(const char *src_dir, const char *name, char ***out_files, size_t *out_count)
{
    *out_files = NULL;
    *out_count = 0;

    const char *argv[] = {"quilt", "files", name, NULL};
    proc_result result = {0};
    nhp_err err = proc_run(argv, src_dir, QUILT_ENV, &result);
    if (err != NHP_OK) {
        proc_result_free(&result);
        return err;
    }
    if (result.signaled || result.exit_code != 0) {
        err_set("quilt files %s failed: %s", name, result.err_data);
        proc_result_free(&result);
        return NHP_EIO;
    }

    err = split_lines(result.out_data, "", out_files, out_count);
    proc_result_free(&result);
    return err;
}

nhp_err quilt_push(const char *src_dir, const char *name, bool force, proc_result *out)
{
    const char *argv[5];
    size_t n = 0;
    argv[n++] = "quilt";
    argv[n++] = "push";
    if (force) {
        argv[n++] = "-f";
    }
    if (name != NULL) {
        argv[n++] = name;
    }
    argv[n] = NULL;
    return proc_run(argv, src_dir, QUILT_ENV, out);
}

nhp_err quilt_push_all(const char *src_dir, proc_result *out)
{
    const char *argv[] = {"quilt", "push", "-a", NULL};
    return proc_run(argv, src_dir, QUILT_ENV, out);
}

nhp_err quilt_pop(const char *src_dir, const char *name, proc_result *out)
{
    const char *argv[4];
    size_t n = 0;
    argv[n++] = "quilt";
    argv[n++] = "pop";
    if (name != NULL) {
        argv[n++] = name;
    }
    argv[n] = NULL;
    return proc_run(argv, src_dir, QUILT_ENV, out);
}

nhp_err quilt_pop_all(const char *src_dir, proc_result *out)
{
    const char *argv[] = {"quilt", "pop", "-a", NULL};
    return proc_run(argv, src_dir, QUILT_ENV, out);
}

nhp_err quilt_refresh(const char *src_dir, const char *name, proc_result *out)
{
    const char *argv[] = {"quilt", "refresh", name, NULL};
    return proc_run(argv, src_dir, QUILT_ENV, out);
}

nhp_err quilt_new(const char *src_dir, const char *name, proc_result *out)
{
    const char *argv[] = {"quilt", "new", name, NULL};
    return proc_run(argv, src_dir, QUILT_ENV, out);
}

nhp_err quilt_add(const char *src_dir, const char *file, proc_result *out)
{
    const char *argv[] = {"quilt", "add", file, NULL};
    return proc_run(argv, src_dir, QUILT_ENV, out);
}

typedef struct dry_run_parse_state {
    quilt_dry_run *out;
    size_t file_cap;
    bool pending_no_file; /* "can't find file to patch" was just seen; see
                           * process_dry_run_line() for how its path is
                           * found afterwards. */
} dry_run_parse_state;

static quilt_dry_run_file *add_dry_run_file(dry_run_parse_state *st, const char *path)
{
    if (st->out->file_count == st->file_cap) {
        size_t new_cap = (st->file_cap == 0) ? 4 : st->file_cap * 2;
        quilt_dry_run_file *grown = realloc(st->out->files, new_cap * sizeof(*grown));
        if (grown == NULL) {
            return NULL;
        }
        st->out->files = grown;
        st->file_cap = new_cap;
    }
    quilt_dry_run_file *f = &st->out->files[st->out->file_count++];
    memset(f, 0, sizeof(*f));
    f->path = str_dup(path);
    return f;
}

/* Parses one line of `patch --verbose`'s dry-run output, updating *st
 * (wording confirmed against GNU patch 2.8). A "can't find file" target
 * has no "checking file" line; its path shows up later, in "|+++ <path>". */
static nhp_err process_dry_run_line(const char *line, dry_run_parse_state *st)
{
    quilt_dry_run *d = st->out;

    if (strncmp(line, "checking file ", 14) == 0) {
        return (add_dry_run_file(st, line + 14) != NULL) ? NHP_OK : NHP_ENOMEM;
    }

    if (strstr(line, "can't find file to patch") != NULL) {
        d->no_file_found = true;
        st->pending_no_file = true;
        return NHP_OK;
    }

    if (st->pending_no_file && strncmp(line, "|+++ ", 5) == 0) {
        const char *start = line + 5;
        const char *tab = strchr(start, '\t');
        size_t len = (tab != NULL) ? (size_t)(tab - start) : strlen(start);
        char *path = str_ndup(start, len);
        if (path == NULL) {
            return NHP_ENOMEM;
        }
        quilt_dry_run_file *f = add_dry_run_file(st, path);
        free(path);
        if (f == NULL) {
            return NHP_ENOMEM;
        }
        f->no_file_found = true;
        st->pending_no_file = false;
        return NHP_OK;
    }

    if (strncmp(line, "Hunk #", 6) == 0) {
        bool failed = strstr(line, "FAILED") != NULL || strstr(line, "ignored") != NULL;
        bool fuzzy = strstr(line, "with fuzz") != NULL;
        d->total_hunks++;
        if (failed) {
            d->failed_hunks++;
        } else if (fuzzy) {
            d->fuzzy_hunks++;
        }
        if (d->file_count > 0) {
            quilt_dry_run_file *f = &d->files[d->file_count - 1];
            f->hunks_total++;
            if (failed) {
                f->hunks_failed++;
            }
        }
    }
    return NHP_OK;
}

nhp_err quilt_dry_run_patch(const char *src_dir, const char *patch_path, int strip,
                            quilt_dry_run *out)
{
    memset(out, 0, sizeof(*out));

    char strip_arg[8];
    (void)snprintf(strip_arg, sizeof(strip_arg), "-p%d", strip);

    const char *argv[] = {"patch", "--dry-run", "--batch", "--force",  "--verbose", strip_arg,
                          "-d",    src_dir,     "-i",      patch_path, NULL};
    proc_result result = {0};
    nhp_err err = proc_run(argv, NULL, NULL, &result);
    if (err != NHP_OK) {
        proc_result_free(&result);
        return err;
    }

    buf combined;
    buf_init(&combined);
    if (result.out_data != NULL) {
        buf_append_str(&combined, result.out_data);
    }
    if (result.err_data != NULL) {
        buf_append_str(&combined, result.err_data);
    }
    out->raw_output = buf_take(&combined);
    proc_result_free(&result);

    char *copy = str_dup(out->raw_output);
    if (copy == NULL) {
        quilt_dry_run_free(out);
        return NHP_ENOMEM;
    }

    dry_run_parse_state st = {.out = out, .file_cap = 0, .pending_no_file = false};
    char *saveptr = NULL;
    const char *line = strtok_r(copy, "\n", &saveptr);
    nhp_err parse_err = NHP_OK;
    while (line != NULL && parse_err == NHP_OK) {
        parse_err = process_dry_run_line(line, &st);
        line = strtok_r(NULL, "\n", &saveptr);
    }
    free(copy);
    if (parse_err != NHP_OK) {
        quilt_dry_run_free(out);
        return parse_err;
    }
    return NHP_OK;
}

void quilt_dry_run_free(quilt_dry_run *d)
{
    free(d->raw_output);
    for (size_t i = 0; i < d->file_count; i++) {
        free(d->files[i].path);
    }
    free(d->files);
    memset(d, 0, sizeof(*d));
}
