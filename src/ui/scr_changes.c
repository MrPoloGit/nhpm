/* scr_changes.c - Changes dialog (docs/features.md 12.5): review file
 * changes and save some as a patch. Two callers share one `changes_backend`.
 * SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "screen.h"

#include "mkpatch.h"
#include "widgets.h"

#include <curses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *status_letter(mkp_status status)
{
    switch (status) {
        case MKP_MODIFIED:
            return "M";
        case MKP_ADDED:
            return "A";
        case MKP_DELETED:
            return "D";
        case MKP_BINARY:
        default:
            return "B";
    }
}

/* What the loop needs from whichever caller opened it, keeping it
 * ignorant of inst_instance/folder details: `full_diff` backs Enter,
 * `commit` backs S, `default_target` pre-fills S's target prompt. */
typedef struct changes_backend {
    void *user_data;
    nhp_err (*full_diff)(void *user_data, const char *const *paths, size_t count, char **out_diff);
    bool (*commit)(ui_app *app, void *user_data, const mkp_patch_meta *meta,
                   const char *const *paths, size_t count);
    const char *default_target;
} changes_backend;

typedef struct changes_row_ctx {
    const mkp_file *files;
    const bool *ticked;
    const size_t *visible_idx; /* maps a display row to its index into files/ticked */
} changes_row_ctx;

static void draw_changes_row(WINDOW *win, int y, int width, size_t index, bool selected,
                             // cppcheck-suppress constParameterCallback -- must match
                             // ui_list_draw_row_fn's signature
                             void *user_data)
{
    const changes_row_ctx *ctx = user_data;
    size_t i = ctx->visible_idx[index];
    const mkp_file *f = &ctx->files[i];

    buf line;
    buf_init(&line);
    buf_append_fmt(&line, "[%c] %s    %s%s%s", ctx->ticked[i] ? 'x' : ' ', status_letter(f->status),
                   f->path, (f->status == MKP_BINARY) ? "  (binary, can't be included)" : "",
                   f->hidden ? "  (hidden: build output)" : "");
    char *text = buf_take(&line);

    if (selected) {
        (void)wattron(win, A_REVERSE);
    }
    (void)mvwaddnstr(win, y, 1, text, width);
    if (selected) {
        (void)wattroff(win, A_REVERSE);
    }
    free(text);
}

static void draw_changes_dialog(ui_app *app, const char *title, const mkp_file *files,
                                const bool *ticked, const size_t *visible_idx, size_t visible_count,
                                const ui_list *list)
{
    WINDOW *w = app->layout.main_win;
    if (w == NULL) {
        return;
    }
    (void)werase(w);
    ui_draw_panel(w, title);
    int inner_width = getmaxx(w) - 2;
    (void)mvwaddnstr(w, 1, 1, "    Status  File", inner_width);

    int inner_height = getmaxy(w) - 2;
    changes_row_ctx ctx = {.files = files, .ticked = ticked, .visible_idx = visible_idx};
    ui_list_draw(list, w, 2, inner_height - 1, inner_width, visible_count, draw_changes_row, &ctx);
    (void)wnoutrefresh(w);
}

/* Indices into `files` that should currently be shown: everything when
 * `show_hidden`, otherwise every file except build-output guesses
 * (docs/features.md 12.5's "h"). The caller frees *out_idx. */
static size_t *build_visible_index(const mkp_file *files, size_t count, bool show_hidden,
                                   size_t *out_count)
{
    size_t *idx = (count > 0) ? malloc(count * sizeof(*idx)) : NULL;
    size_t n = 0;
    for (size_t i = 0; i < count; i++) {
        if (show_hidden || !files[i].hidden) {
            if (idx != NULL) {
                idx[n] = i;
            }
            n++;
        }
    }
    *out_count = n;
    return idx;
}

/* `paths[0..*out_count)` holds every non-binary file (all, or just the
 * ticked ones if `only_ticked`) -- pointers into `files`' strings, not
 * copies; the caller frees only the array. */
static char **collect_paths(const mkp_file *files, const bool *ticked, size_t count,
                            bool only_ticked, size_t *out_count)
{
    char **paths = (count > 0) ? malloc(count * sizeof(*paths)) : NULL;
    size_t n = 0;
    for (size_t i = 0; i < count; i++) {
        if (files[i].status == MKP_BINARY) {
            continue;
        }
        if (only_ticked && !ticked[i]) {
            continue;
        }
        if (paths != NULL) {
            paths[n] = files[i].path;
        }
        n++;
    }
    *out_count = n;
    return paths;
}

/* Returns true if it set a status message the caller should keep
 * showing (the early-return failures); false once ui_show_text_dialog()
 * runs, since that modal needs no status line of its own. */
static bool do_show_full_diff(ui_app *app, const changes_backend *backend, const mkp_file *files,
                              size_t count)
{
    size_t n = 0;
    char **paths = collect_paths(files, NULL, count, false, &n);
    if (n == 0) {
        ui_set_status(app, "nothing to show -- every change is binary");
        free(paths);
        return true;
    }

    char *diff = NULL;
    nhp_err err = backend->full_diff(backend->user_data, (const char *const *)paths, n, &diff);
    free(paths);
    if (err != NHP_OK) {
        ui_set_status(app, "could not build the diff: %s", err_get());
        return true;
    }
    ui_show_text_dialog(app, "Diff", diff);
    free(diff);
    return false;
}

/* docs/features.md 12.5's "i": adds the row's file to the ignore list,
 * then re-tags files with the same mkp_is_build_output() check
 * mkp_compare_trees() used -- no need for a fresh comparison. */
static void do_add_ignore_pattern(ui_app *app, mkp_file *files, bool *ticked, size_t count,
                                  size_t selected_index)
{
    const char *path = files[selected_index].path;
    char pattern[300];
    const char *dot = strrchr(path, '.');
    if (dot != NULL && dot != path) {
        (void)snprintf(pattern, sizeof(pattern), "*%s", dot);
    } else {
        (void)snprintf(pattern, sizeof(pattern), "%s", path);
    }

    if (!ui_prompt_line(app, "Ignore pattern: ", pattern, sizeof(pattern), NULL) ||
        pattern[0] == '\0') {
        ui_set_status(app, "cancelled");
        return;
    }

    nhp_err err = cfg_app_add_ignore_pattern(&app->config, pattern);
    if (err != NHP_OK) {
        ui_set_status(app, "could not save the ignore pattern: %s", err_get());
        return;
    }

    size_t newly_hidden = 0;
    for (size_t i = 0; i < count; i++) {
        bool was_hidden = files[i].hidden;
        files[i].hidden =
            mkp_is_build_output(files[i].path, app->active_inst.source, app->config.ignore);
        if (files[i].hidden && !was_hidden) {
            ticked[i] = false;
            newly_hidden++;
        }
    }
    ui_set_status(app, "added \"%s\" to the ignore list (%zu file(s) now hidden)", pattern,
                  newly_hidden);
}

/* Prompts name/version/author/target/description, then hands ticked
 * files to `backend->commit()` (docs/features.md 12.3 steps 3-4, reused
 * by 12.6). Returns true on success so the caller closes the dialog. */
static bool do_save_as_patch(ui_app *app, const changes_backend *backend, const mkp_file *files,
                             const bool *ticked, size_t count)
{
    size_t n = 0;
    char **paths = collect_paths(files, ticked, count, true, &n);
    if (n == 0) {
        ui_set_status(app, "no files ticked -- Space tick a row, then S to save");
        free(paths);
        return false;
    }

    char name[128] = "";
    if (!ui_prompt_line(app, "Patch name: ", name, sizeof(name), NULL) || name[0] == '\0') {
        ui_set_status(app, "cancelled");
        free(paths);
        return false;
    }
    char version[32] = "";
    (void)ui_prompt_line(app, "Version (blank for 1.0): ", version, sizeof(version), NULL);
    char author[128] = "";
    (void)ui_prompt_line(app, "Author (blank for unknown): ", author, sizeof(author), NULL);
    char target[32];
    (void)snprintf(target, sizeof(target), "%s", backend->default_target);
    (void)ui_prompt_line(app, "Target NetHack version: ", target, sizeof(target), NULL);
    char short_desc[200] = "";
    (void)ui_prompt_line(app, "Short description: ", short_desc, sizeof(short_desc), NULL);

    mkp_patch_meta meta = {
        .name = name,
        .version = version,
        .author = author,
        .target = target,
        .short_desc = short_desc,
        .long_desc = "",
    };
    bool ok = backend->commit(app, backend->user_data, &meta, (const char *const *)paths, n);
    free(paths);
    return ok;
}

/* Shared dialog loop: list, ticking, h/i/Enter, S's prompt-then-commit
 * -- identical regardless of what's compared, since `backend` owns
 * that. `files`/`count` stay the caller's; this owns `ticked`/`visible_idx`. */
static void run_changes_dialog(ui_app *app, const char *title, mkp_file *files, size_t count,
                               const changes_backend *backend)
{
    bool *ticked = malloc(count * sizeof(*ticked));
    if (ticked == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        ticked[i] = files[i].default_selected;
    }

    bool show_hidden = false;
    size_t visible_count = 0;
    size_t *visible_idx = build_visible_index(files, count, show_hidden, &visible_count);

    size_t hidden_count = 0;
    for (size_t i = 0; i < count; i++) {
        if (files[i].hidden) {
            hidden_count++;
        }
    }

    ui_list list;
    ui_list_init(&list);
    app->text_input_active = true;
    bool done = false;
    /* Last i/Enter/S result, so the redraw right after doesn't silently
     * overwrite it with the generic "%zu file(s)..." status -- same bug
     * already fixed elsewhere. Scoped to this call, not a file static. */
    char action_status[256] = "";
    while (!done) {
        draw_changes_dialog(app, title, files, ticked, visible_idx, visible_count, &list);
        size_t ticked_count = 0;
        for (size_t i = 0; i < count; i++) {
            if (ticked[i]) {
                ticked_count++;
            }
        }
        if (action_status[0] != '\0') {
            ui_set_status(app, "%s", action_status);
        } else if (hidden_count > 0) {
            ui_set_status(app,
                          "%zu file(s), %zu ticked, %zu hidden as build output (h %s) -- Space "
                          "tick, Enter diff, i ignore, S save, Esc back",
                          count, ticked_count, hidden_count, show_hidden ? "hide" : "show");
        } else {
            ui_set_status(app,
                          "%zu file(s), %zu ticked -- Space tick, Enter diff, i ignore, S save, "
                          "Esc back",
                          count, ticked_count);
        }
        ui_flush_status(app);

        wint_t ch = 0;
        int r = wget_wch(stdscr, &ch);
        if (r == KEY_CODE_YES && (ch == KEY_RESIZE || ch == KEY_MOUSE)) {
            continue;
        }

        WINDOW *w = app->layout.main_win;
        int visible = (w != NULL) ? getmaxy(w) - 3 : 1;
        if (ch == KEY_UP || ch == L'k') {
            ui_list_move(&list, -1, visible_count, visible);
            continue;
        }
        if (ch == KEY_DOWN || ch == L'j') {
            ui_list_move(&list, 1, visible_count, visible);
            continue;
        }
        if (ch == KEY_PPAGE) {
            ui_list_move(&list, -visible, visible_count, visible);
            continue;
        }
        if (ch == KEY_NPAGE) {
            ui_list_move(&list, visible, visible_count, visible);
            continue;
        }
        if (ch == 27) { /* Esc */
            done = true;
            continue;
        }
        if (ch == L'h') {
            show_hidden = !show_hidden;
            free(visible_idx);
            visible_idx = build_visible_index(files, count, show_hidden, &visible_count);
            list.selected = 0;
            list.top = 0;
            continue;
        }
        if (ch == L'i') {
            if (visible_count > 0) {
                do_add_ignore_pattern(app, files, ticked, count, visible_idx[list.selected]);
                (void)snprintf(action_status, sizeof(action_status), "%s",
                               (app->status_message != NULL) ? app->status_message : "");
                app->text_input_active = true;
                free(visible_idx);
                visible_idx = build_visible_index(files, count, show_hidden, &visible_count);
                hidden_count = 0;
                for (size_t i = 0; i < count; i++) {
                    if (files[i].hidden) {
                        hidden_count++;
                    }
                }
                if (list.selected >= visible_count && visible_count > 0) {
                    list.selected = visible_count - 1;
                }
            }
            continue;
        }
        if (ch == L' ') {
            if (visible_count > 0) {
                size_t i = visible_idx[list.selected];
                if (files[i].status != MKP_BINARY) {
                    ticked[i] = !ticked[i];
                }
            }
            continue;
        }
        if (ch == L'\n' || ch == KEY_ENTER) {
            if (do_show_full_diff(app, backend, files, count)) {
                (void)snprintf(action_status, sizeof(action_status), "%s",
                               (app->status_message != NULL) ? app->status_message : "");
            } else {
                action_status[0] = '\0';
            }
            app->text_input_active = true;
            continue;
        }
        if (ch == L'S') {
            if (do_save_as_patch(app, backend, files, ticked, count)) {
                done = true;
            } else {
                (void)snprintf(action_status, sizeof(action_status), "%s",
                               (app->status_message != NULL) ? app->status_message : "");
            }
            app->text_input_active = true;
            continue;
        }
    }

    app->text_input_active = false;
    free(visible_idx);
    free(ticked);
}

/* ---- backend: the active instance's own uncaptured edits (12.3/'w') ---- */

// cppcheck-suppress constParameterCallback -- must match changes_backend.full_diff's signature
static nhp_err backend_full_diff_inst(void *user_data, const char *const *paths, size_t count,
                                      char **out_diff)
{
    return mkp_changes_diff(user_data, paths, count, out_diff);
}

static bool backend_commit_inst(ui_app *app, void *user_data, const mkp_patch_meta *meta,
                                const char *const *paths, size_t count)
{
    if (!ui_require_write_access(app)) {
        return false;
    }
    inst_instance *inst = user_data;
    char *out_id = NULL;
    nhp_err err = mkp_capture(inst, meta, paths, count, &out_id);
    if (err != NHP_OK) {
        ui_set_status(app, "could not capture: %s", err_get());
        return false;
    }
    ui_set_status(app, "captured %s as %s %s", meta->name, out_id,
                  meta->version[0] != '\0' ? meta->version : "1.0");
    free(out_id);
    ui_reload_active_instance(app);
    return true;
}

void ui_show_changes_dialog(ui_app *app)
{
    if (!app->active_loaded) {
        ui_set_status(app, "no active instance");
        return;
    }

    mkp_file *files = NULL;
    size_t count = 0;
    nhp_err err = mkp_changes(&app->active_inst, app->config.ignore, &files, &count);
    if (err != NHP_OK) {
        ui_set_status(app, "could not list changes: %s", err_get());
        return;
    }
    if (count == 0) {
        ui_set_status(app, "no changes found in %s", app->active_inst.name);
        mkp_file_list_free(files, count);
        return;
    }

    char title[160];
    (void)snprintf(title, sizeof(title), "Changes in %s", app->active_inst.name);
    changes_backend backend = {
        .user_data = &app->active_inst,
        .full_diff = backend_full_diff_inst,
        .commit = backend_commit_inst,
        .default_target = app->active_inst.source,
    };
    run_changes_dialog(app, title, files, count, &backend);
    mkp_file_list_free(files, count);
}

/* ---- backend: an arbitrary folder against a base (12.6/Library's 'm') ---- */

typedef struct folder_changes_ctx {
    const char *folder;
    const mkp_base_spec *base;
} folder_changes_ctx;

// cppcheck-suppress constParameterCallback -- must match changes_backend.full_diff's signature
static nhp_err backend_full_diff_folder(void *user_data, const char *const *paths, size_t count,
                                        char **out_diff)
{
    const folder_changes_ctx *ctx = user_data;
    return mkp_from_folder_diff(ctx->folder, ctx->base, paths, count, out_diff);
}

// cppcheck-suppress constParameterCallback -- must match changes_backend.commit's signature
static bool backend_commit_folder(ui_app *app, void *user_data, const mkp_patch_meta *meta,
                                  const char *const *paths, size_t count)
{
    const folder_changes_ctx *ctx = user_data;
    char *out_id = NULL;
    nhp_err err =
        mkp_from_folder(ctx->folder, ctx->base, meta, app->config.ignore, paths, count, &out_id);
    if (err != NHP_OK) {
        ui_set_status(app, "could not make a patch from %s: %s", ctx->folder, err_get());
        return false;
    }
    ui_set_status(app, "made %s %s from %s", out_id,
                  meta->version[0] != '\0' ? meta->version : "1.0", ctx->folder);
    free(out_id);
    return true;
}

void ui_show_changes_dialog_for_folder(ui_app *app, const char *folder, const mkp_base_spec *base)
{
    mkp_file *files = NULL;
    size_t count = 0;
    nhp_err err = mkp_from_folder_changes(folder, base, app->config.ignore, &files, &count);
    if (err != NHP_OK) {
        ui_set_status(app, "could not compare %s: %s", folder, err_get());
        return;
    }
    if (count == 0) {
        ui_set_status(app, "no changes found between %s and the given base", folder);
        mkp_file_list_free(files, count);
        return;
    }

    char title[160];
    (void)snprintf(title, sizeof(title), "Changes in %s", folder);
    folder_changes_ctx ctx = {.folder = folder, .base = base};
    changes_backend backend = {
        .user_data = &ctx,
        .full_diff = backend_full_diff_folder,
        .commit = backend_commit_folder,
        .default_target = "",
    };
    run_changes_dialog(app, title, files, count, &backend);
    mkp_file_list_free(files, count);
}
