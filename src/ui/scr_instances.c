/* scr_instances.c - F2 Instances screen.
 * SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "screen.h"

#include "build.h"
#include "library.h"
#include "mkpatch.h"
#include "sources.h"
#include "stack.h"
#include "widgets.h"

#include <curses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Loaded on enter(), freed on leave() -- the "static state scoped to
 * the file" shape docs/style.md 6.3 asks for (file-local mutable state,
 * documented at its declaration); more screens will follow it too. */
static inst_instance *g_instances = NULL;
static size_t g_count = 0;
static ui_list g_list;

/* "/" search (13.9): a true filter, same as Sources -- Instances has no
 * J/K-style reordering either. Pointers into g_instances, rebuilt by
 * recompute_visible() on every filter change and reload(). */
static char g_filter[256] = "";
static const inst_instance **g_visible = NULL;
static size_t g_visible_count = 0;

/* Against name/source/recipe/notes. Keeps the same instance selected if
 * it's still visible, instead of snapping to the top on every keystroke. */
static void recompute_visible(void)
{
    const inst_instance *kept = (g_visible_count > 0 && g_list.selected < g_visible_count)
                                    ? g_visible[g_list.selected]
                                    : NULL;
    free(g_visible);
    g_visible = malloc((g_count > 0 ? g_count : 1) * sizeof(*g_visible));
    g_visible_count = 0;
    if (g_visible == NULL) {
        return;
    }
    for (size_t i = 0; i < g_count; i++) {
        const char *fields[] = {g_instances[i].name, g_instances[i].source, g_instances[i].recipe,
                                g_instances[i].notes};
        if (str_matches_all_words(g_filter, fields, 4)) {
            g_visible[g_visible_count++] = &g_instances[i];
        }
    }
    ui_list_init(&g_list);
    if (kept != NULL) {
        for (size_t i = 0; i < g_visible_count; i++) {
            if (g_visible[i] == kept) {
                g_list.selected = i;
                break;
            }
        }
    }
}

static void reload(void)
{
    if (g_instances != NULL) {
        inst_list_free(g_instances, g_count);
        g_instances = NULL;
        g_count = 0;
    }
    (void)inst_list(&g_instances, &g_count);
    recompute_visible();
}

static void enter(ui_app *app)
{
    (void)app;
    reload();
}

static void leave(ui_app *app)
{
    (void)app;
    if (g_instances != NULL) {
        inst_list_free(g_instances, g_count);
        g_instances = NULL;
        g_count = 0;
    }
    free(g_visible);
    g_visible = NULL;
    g_visible_count = 0;
}

static void built_column(const inst_instance *inst, char *out, size_t out_size)
{
    if (inst->last_built[0] == '\0') {
        (void)snprintf(out, out_size, "NOT BUILT");
    } else if (build_is_stale(inst)) {
        (void)snprintf(out, out_size, "out of date");
    } else {
        (void)snprintf(out, out_size, "built");
    }
}

/* Same role colors as every screen's status marks (13.8): red = never
 * built (can't play, same check Build's own F9/'p' makes), yellow =
 * built but stale (patches changed since), green = ready. */
static enum ui_color_pair built_color(const inst_instance *inst)
{
    if (inst->last_built[0] == '\0') {
        return UI_COLOR_ERROR;
    }
    if (build_is_stale(inst)) {
        return UI_COLOR_WARN;
    }
    return UI_COLOR_OK;
}

typedef struct row_context {
    ui_app *app;
} row_context;

// cppcheck-suppress constParameterCallback -- must match ui_list_draw_row_fn's signature
static void draw_row(WINDOW *win, int y, int width, size_t index, bool selected, void *user_data)
{
    const row_context *ctx = user_data;
    const inst_instance *inst = g_visible[index];

    size_t enabled = 0;
    for (size_t i = 0; i < inst->patch_count; i++) {
        if (inst->patches[i].enabled) {
            enabled++;
        }
    }

    char built[32];
    built_column(inst, built, sizeof(built));
    char built_padded[sizeof(built)];
    (void)snprintf(built_padded, sizeof(built_padded), "%-12s", built);

    char last_played[32];
    (void)snprintf(last_played, sizeof(last_played), "%s",
                   inst->last_played[0] != '\0' ? inst->last_played : "never");

    const char *active_marker = str_eq(ctx->app->active_name, inst->name) ? " (active)" : "";
    const char *kind_marker = (inst->kind == INST_LINKED) ? " linked" : "";

    /* Drawn in three pieces, not one string, so the middle (Built) can be
     * colored alone -- same split every screen's status glyph uses, but
     * with a variable-width word, so the split point is computed, not fixed. */
    buf left;
    buf_init(&left);
    buf_append_fmt(&left, "%-18s %-8s %zu/%zu ", inst->name, inst->source, enabled,
                   inst->patch_count);
    char *left_text = buf_take(&left);

    buf right;
    buf_init(&right);
    buf_append_fmt(&right, " %-20s%s%s", last_played, active_marker, kind_marker);
    char *right_text = buf_take(&right);

    bool colorize = ctx->app != NULL && !ctx->app->monochrome && !selected;

    if (selected) {
        (void)wattron(win, A_REVERSE);
    }
    int left_len = (int)strlen(left_text);
    (void)mvwaddnstr(win, y, 1, left_text, width);
    if (left_len < width) {
        if (colorize) {
            (void)wattron(win, COLOR_PAIR(built_color(inst)));
        }
        (void)mvwaddnstr(win, y, 1 + left_len, built_padded, width - left_len);
        if (colorize) {
            (void)wattroff(win, COLOR_PAIR(built_color(inst)));
        }
    }
    int built_len = (int)strlen(built_padded);
    if (left_len + built_len < width) {
        (void)mvwaddnstr(win, y, 1 + left_len + built_len, right_text,
                         width - left_len - built_len);
    }
    if (selected) {
        (void)wattroff(win, A_REVERSE);
    }
    free(left_text);
    free(right_text);
}

static void draw_details(ui_app *app)
{
    WINDOW *w = app->layout.details;
    if (w == NULL) {
        return;
    }
    (void)werase(w);
    ui_draw_panel(w, "Details");
    if (g_visible_count == 0) {
        (void)wnoutrefresh(w);
        return;
    }
    int inner_width = getmaxx(w) - 2;
    const inst_instance *inst = g_visible[g_list.selected];
    if (inst->notes[0] != '\0') {
        (void)mvwaddnstr(w, 1, 1, inst->notes, inner_width);
    }

    buf line;
    buf_init(&line);
    buf_append_fmt(&line, "Source: %s (%s)  Recipe: %s", inst->source,
                   (inst->kind == INST_LINKED) ? "linked" : "managed", inst->recipe);
    char *text = buf_take(&line);
    (void)mvwaddnstr(w, 2, 1, text, inner_width);
    free(text);

    (void)wnoutrefresh(w);
}

static void draw(ui_app *app)
{
    WINDOW *w = app->layout.main_win;
    if (w == NULL) {
        return;
    }
    (void)werase(w);
    ui_draw_panel(w, g_filter[0] != '\0' ? "Instances (filtered)" : "Instances");
    int inner_width = getmaxx(w) - 2;
    (void)mvwaddnstr(w, 1, 1, "Name               Version  Patches Built        Last played",
                     inner_width);

    int inner_height = getmaxy(w) - 2;
    row_context ctx = {.app = app};
    ui_list_draw(&g_list, w, 2, inner_height - 1, inner_width, g_visible_count, draw_row, &ctx);
    (void)wnoutrefresh(w);

    draw_details(app);

    if (g_count == 0) {
        ui_set_status(app, "no instances yet -- press n to create one");
    } else if (g_visible_count == 0) {
        ui_set_status(app, "no instance matches \"%s\"", g_filter);
    } else if (g_filter[0] != '\0') {
        ui_set_status(app, "%zu of %zu instance%s, filtered", g_visible_count, g_count,
                      g_count == 1 ? "" : "s");
    }
}

/* The recipes build.c can run (docs/features.md 10.6); "custom"
 * reads instance.conf's custom_commands, editable from the Build
 * screen's own "c" (docs/ui-progress.md). */
static const char *const RECIPE_OPTIONS[] = {
    "nethack-3.4-unix",
    "nethack-3.6-linux",
    "nethack-5.0-linux",
    "custom",
};

/* docs/features.md 10.1's two kinds: Managed copies a base source into the
 * instance's own src/; Linked points at a folder nhpm never touches
 * (a developer's own git checkout). */
static const char *const KIND_OPTIONS[] = {
    "Managed (copy a base source)",
    "Linked (point at an existing folder)",
};

static const char *pick_recipe(ui_app *app)
{
    size_t recipe_index = 0;
    bool picked_recipe =
        ui_show_popup_menu(app, "Build recipe", RECIPE_OPTIONS,
                           sizeof(RECIPE_OPTIONS) / sizeof(RECIPE_OPTIONS[0]), &recipe_index, draw);
    return picked_recipe ? RECIPE_OPTIONS[recipe_index] : "custom";
}

/* docs/features.md 10.5's "New", Linked half: points at an existing
 * folder (its version read from patchlevel.h the same way sources.c
 * does) instead of copying a base source. */
static void do_new_linked(ui_app *app, const char *name)
{
    char folder[512] = "";
    if (!ui_show_popup_input(app, "Folder to link", folder, sizeof(folder), draw) ||
        folder[0] == '\0') {
        ui_set_status(app, "cancelled");
        return;
    }

    const char *recipe = pick_recipe(app);

    inst_instance inst;
    nhp_err err = inst_link(name, folder, recipe, &inst);
    if (err == NHP_OK) {
        inst_free(&inst);
        reload();
        (void)cfg_set_active_instance(name); /* a fresh instance becomes the active one */
        ui_reload_active_instance(app);
        ui_set_status(app, "linked and switched to %s -> %s", name, folder);
    } else {
        ui_set_status(app, "could not link %s: %s", name, err_get());
    }
}

/* docs/features.md 10.5's "New", Managed half: the original single-kind
 * flow, copying a base source the user already added on Sources. */
static void do_new_managed(ui_app *app, const char *name)
{
    bsrc_source *srcs = NULL;
    size_t src_count = 0;
    (void)bsrc_list(&srcs, &src_count);
    if (src_count == 0) {
        ui_set_status(app, "no base sources yet -- add one on the Sources screen first");
        bsrc_list_free(srcs, src_count);
        return;
    }

    const char **src_options = malloc(src_count * sizeof(*src_options));
    if (src_options == NULL) {
        bsrc_list_free(srcs, src_count);
        ui_set_status(app, "out of memory");
        return;
    }
    for (size_t i = 0; i < src_count; i++) {
        src_options[i] = srcs[i].version;
    }
    size_t src_index = 0;
    bool picked_source =
        ui_show_popup_menu(app, "Base source", src_options, src_count, &src_index, draw);
    free(src_options);
    if (!picked_source) {
        bsrc_list_free(srcs, src_count);
        ui_set_status(app, "cancelled");
        return;
    }
    char version[64];
    (void)snprintf(version, sizeof(version), "%s", srcs[src_index].version);
    bsrc_list_free(srcs, src_count);

    const char *recipe = pick_recipe(app);

    inst_instance inst;
    nhp_err err = inst_new(name, version, recipe, &inst);
    if (err == NHP_OK) {
        inst_free(&inst);
        reload();
        (void)cfg_set_active_instance(name); /* a fresh instance becomes the active one */
        ui_reload_active_instance(app);
        ui_set_status(app, "created and switched to %s", name);
    } else {
        ui_set_status(app, "could not create %s: %s", name, err_get());
    }
}

static void do_new(ui_app *app)
{
    char name[256] = "";
    if (!ui_show_popup_input(app, "Instance name", name, sizeof(name), draw) || name[0] == '\0') {
        ui_set_status(app, "cancelled");
        return;
    }

    size_t kind_index = 0;
    bool picked_kind =
        ui_show_popup_menu(app, "Kind", KIND_OPTIONS,
                           sizeof(KIND_OPTIONS) / sizeof(KIND_OPTIONS[0]), &kind_index, draw);
    bool linked = picked_kind && kind_index == 1;

    if (linked) {
        do_new_linked(app, name);
    } else {
        do_new_managed(app, name);
    }
}

static void do_clone(ui_app *app)
{
    if (g_visible_count == 0) {
        return;
    }
    char *old_name = str_dup(g_visible[g_list.selected]->name);

    char new_name[256] = "";
    if (!ui_show_popup_input(app, "Clone, new name: ", new_name, sizeof(new_name), draw) ||
        new_name[0] == '\0') {
        ui_set_status(app, "cancelled");
        free(old_name);
        return;
    }

    bool with_saves = ui_confirm(app, "Copy its save games too?", draw);
    nhp_err err = inst_clone(old_name, new_name, with_saves);
    if (err == NHP_OK) {
        ui_set_status(app, "cloned %s to %s", old_name, new_name);
        reload();
    } else {
        ui_set_status(app, "could not clone %s: %s", old_name, err_get());
    }
    free(old_name);
}

static void do_rename(ui_app *app)
{
    if (g_visible_count == 0) {
        return;
    }
    char *old_name = str_dup(g_visible[g_list.selected]->name);
    bool was_active = str_eq(app->active_name, old_name);

    /* docs/features.md 10.5: refused while locked -- but only checkable
     * here for the active instance, since that's the only lock this
     * session's ui_app tracks. A non-active row's lock is someone else's business. */
    if (was_active && !ui_require_write_access(app)) {
        free(old_name);
        return;
    }

    char new_name[256];
    (void)snprintf(new_name, sizeof(new_name), "%s", old_name);
    bool ok = ui_show_popup_input(app, "Rename to: ", new_name, sizeof(new_name), draw);
    if (!ok || new_name[0] == '\0' || str_eq(new_name, old_name)) {
        ui_set_status(app, "cancelled");
        free(old_name);
        return;
    }

    nhp_err err = inst_rename(old_name, new_name);
    if (err == NHP_OK) {
        ui_set_status(app, "renamed %s to %s", old_name, new_name);
        /* inst_rename() is filesystem-only (same as the CLI) -- keep the
         * active-instance pointer following its target rather than
         * leaving it stuck on a name that no longer exists. */
        if (was_active) {
            (void)cfg_set_active_instance(new_name);
            ui_reload_active_instance(app);
        }
        reload();
    } else {
        ui_set_status(app, "could not rename %s: %s", old_name, err_get());
    }
    free(old_name);
}

static void do_delete(ui_app *app)
{
    if (g_visible_count == 0) {
        return;
    }
    char *name = str_dup(g_visible[g_list.selected]->name);
    bool was_active = str_eq(app->active_name, name);
    if (was_active && !ui_require_write_access(app)) {
        free(name);
        return;
    }

    char message[300];
    (void)snprintf(message, sizeof(message), "Delete %s? This removes it entirely.", name);
    if (!ui_confirm(app, message, draw)) {
        free(name);
        return;
    }
    bool keep_saves = ui_confirm(app, "Keep its save games (move them aside)?", draw);

    nhp_err err = inst_delete(name, keep_saves);
    if (err == NHP_OK) {
        ui_set_status(app, "deleted %s", name);
        /* Same as CLI's "inst rm": the persisted active-instance pointer
         * is left as-is; just stop showing it active this session. Also
         * drops our lock hold -- else a reused name could fool ui_reload_active_instance(). */
        if (was_active) {
            free(app->active_name);
            app->active_name = str_dup("");
            if (app->active_loaded) {
                inst_free(&app->active_inst);
                app->active_loaded = false;
            }
            if (app->active_lock_held) {
                inst_lock_release(&app->active_lock);
                app->active_lock_held = false;
            }
            free(app->active_lock_name);
            app->active_lock_name = str_dup("");
            app->active_read_only = false;
            app->active_lock_other_pid = 0;
        }
        reload(); /* recompute_visible() resets selection; the deleted entry is gone either way */
    } else {
        ui_set_status(app, "could not delete %s: %s", name, err_get());
    }
    free(name);
}

/* One row of the snapshots dialog below -- packaged so ui_list_draw()'s
 * generic (WINDOW*, y, width, index, selected, user_data) callback can
 * reach the snapshot array without a file-scope global. */
typedef struct snap_row_ctx {
    const inst_snapshot *snaps;
} snap_row_ctx;

static void draw_snap_row(WINDOW *win, int y, int width, size_t index, bool selected,
                          // cppcheck-suppress constParameterCallback -- must match
                          // ui_list_draw_row_fn's signature
                          void *user_data)
{
    const snap_row_ctx *ctx = user_data;
    const inst_snapshot *s = &ctx->snaps[index];

    buf line;
    buf_init(&line);
    buf_append_fmt(&line, "%-45.45s %-20.20s %s", s->filename, s->label[0] != '\0' ? s->label : "-",
                   s->created);
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

static void draw_snapshot_dialog(ui_app *app, const char *inst_name, const inst_snapshot *snaps,
                                 size_t count, const ui_list *list)
{
    WINDOW *w = app->layout.main_win;
    if (w == NULL) {
        return;
    }
    (void)werase(w);
    char title[160];
    (void)snprintf(title, sizeof(title), "Snapshots -- %s", inst_name);
    ui_draw_panel(w, title);
    int inner_width = getmaxx(w) - 2;
    (void)mvwaddnstr(w, 1, 1,
                     "Snapshot                                     Label                Created",
                     inner_width);

    int inner_height = getmaxy(w) - 2;
    snap_row_ctx ctx = {.snaps = snaps};
    ui_list_draw(list, w, 2, inner_height - 1, inner_width, count, draw_snap_row, &ctx);
    (void)wnoutrefresh(w);
}

/* Blocking dialog (docs/features.md 10.5): Enter restores, x deletes, a
 * adds, Esc closes. Its "a"/Enter/"x" sub-prompts save and restore
 * app->text_input_active, since ui_show_popup_input() leaves it false on return. */
static void do_snapshots(ui_app *app)
{
    if (g_visible_count == 0) {
        return;
    }
    char *inst_name = str_dup(g_visible[g_list.selected]->name);

    inst_snapshot *snaps = NULL;
    size_t count = 0;
    (void)inst_snapshot_list(inst_name, &snaps, &count);
    ui_list list;
    ui_list_init(&list);

    app->text_input_active = true;
    bool restored_active = false;
    for (;;) {
        draw_snapshot_dialog(app, inst_name, snaps, count, &list);
        if (count == 0) {
            ui_set_status(app, "no snapshots for %s -- a adds one, Esc closes", inst_name);
        } else {
            ui_set_status(app, "%zu snapshot(s) -- Enter restore, x delete, a add, Esc close",
                          count);
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
            ui_list_move(&list, -1, count, visible);
            continue;
        }
        if (ch == KEY_DOWN || ch == L'j') {
            ui_list_move(&list, 1, count, visible);
            continue;
        }
        if (ch == KEY_PPAGE) {
            ui_list_move(&list, -visible, count, visible);
            continue;
        }
        if (ch == KEY_NPAGE) {
            ui_list_move(&list, visible, count, visible);
            continue;
        }
        if (ch == 27) { /* Esc */
            break;
        }
        if (ch == L'a') {
            char label[128] = "";
            bool ok =
                ui_show_popup_input(app, "Label (blank for none): ", label, sizeof(label), draw);
            app->text_input_active = true;
            if (ok) {
                char *filename = NULL;
                nhp_err err = inst_snapshot_create(inst_name, label, &filename);
                if (err == NHP_OK) {
                    ui_set_status(app, "snapshot: %s", filename);
                } else {
                    ui_set_status(app, "could not snapshot: %s", err_get());
                }
                free(filename);
                inst_snapshot_list_free(snaps, count);
                snaps = NULL;
                count = 0;
                (void)inst_snapshot_list(inst_name, &snaps, &count);
                ui_list_init(&list);
            }
            continue;
        }
        if ((ch == L'\n' || ch == KEY_ENTER) && count > 0) {
            char message[300];
            (void)snprintf(message, sizeof(message), "Restore %s from %s?", inst_name,
                           snaps[list.selected].filename);
            bool confirmed = ui_confirm(app, message, draw);
            app->text_input_active = true;
            if (confirmed) {
                inst_instance target;
                nhp_err err = inst_load(inst_name, &target);
                if (err == NHP_OK) {
                    stack_conflict conflict;
                    err = stack_restore_snapshot(&target, snaps[list.selected].filename, &conflict);
                    if (err == NHP_OK) {
                        ui_set_status(app, "restored %s", inst_name);
                        restored_active = restored_active || str_eq(app->active_name, inst_name);
                    } else if (err == NHP_ECONFLICT) {
                        ui_show_conflict(app, &conflict);
                        ui_set_status(app, "restore failed: %s %s does not apply cleanly",
                                      conflict.patch_id, conflict.patch_version);
                        stack_conflict_free(&conflict);
                    } else {
                        ui_set_status(app, "could not restore: %s", err_get());
                    }
                    inst_free(&target);
                } else {
                    ui_set_status(app, "could not restore: %s", err_get());
                }
            }
            continue;
        }
        if (ch == L'x' && count > 0) {
            char message[300];
            (void)snprintf(message, sizeof(message), "Delete snapshot %s?",
                           snaps[list.selected].filename);
            bool confirmed = ui_confirm(app, message, draw);
            app->text_input_active = true;
            if (confirmed) {
                nhp_err err = inst_snapshot_delete(inst_name, snaps[list.selected].filename);
                if (err == NHP_OK) {
                    ui_set_status(app, "deleted");
                    inst_snapshot_list_free(snaps, count);
                    snaps = NULL;
                    count = 0;
                    (void)inst_snapshot_list(inst_name, &snaps, &count);
                    ui_list_init(&list);
                } else {
                    ui_set_status(app, "could not delete: %s", err_get());
                }
            }
            continue;
        }
    }

    app->text_input_active = false;
    inst_snapshot_list_free(snaps, count);
    if (restored_active) {
        ui_reload_active_instance(app);
    }
    reload();
    free(inst_name);
}

static void do_export(ui_app *app)
{
    if (g_visible_count == 0) {
        return;
    }
    char *name = str_dup(g_visible[g_list.selected]->name);

    char path[512];
    (void)snprintf(path, sizeof(path), "%s.nhinst", name);
    if (!ui_show_popup_input(app, "Export to: ", path, sizeof(path), draw) || path[0] == '\0') {
        ui_set_status(app, "cancelled");
        free(name);
        return;
    }

    nhp_err err = inst_export(name, path);
    if (err == NHP_OK) {
        ui_set_status(app, "exported %s to %s", name, path);
    } else {
        ui_set_status(app, "could not export %s: %s", name, err_get());
    }
    free(name);
}

static void do_import(ui_app *app)
{
    char path[512] = "";
    if (!ui_show_popup_input(app, "Import .nhinst, path: ", path, sizeof(path), draw) ||
        path[0] == '\0') {
        ui_set_status(app, "cancelled");
        return;
    }
    char new_name[256] = "";
    if (!ui_show_popup_input(app, "New instance name: ", new_name, sizeof(new_name), draw) ||
        new_name[0] == '\0') {
        ui_set_status(app, "cancelled");
        return;
    }
    char source_version[64] = "";
    (void)ui_show_popup_input(app, "Base source version (blank to use what was exported): ",
                              source_version, sizeof(source_version), draw);

    inst_instance inst;
    nhp_err err =
        inst_import(path, new_name, source_version[0] != '\0' ? source_version : NULL, &inst);
    if (err == NHP_OK) {
        inst_free(&inst);
        ui_set_status(app, "imported %s as %s", path, new_name);
        reload();
    } else {
        ui_set_status(app, "could not import %s: %s", path, err_get());
    }
}

/* "2026-11-15T20:14:03Z" -> "2026-11-15", for the "new version each
 * time" library version of a combined patch (docs/features.md 12.2) --
 * mirrors main.c's own today_date(). */
static char *today_date(void)
{
    char *iso = time_now_iso8601();
    if (iso != NULL && strlen(iso) >= 10) {
        iso[10] = '\0';
    }
    return iso;
}

/* docs/features.md 12.2: always writes to a file first, then optionally
 * also adds it to the library and/or flattens it into a new instance --
 * the CLI's --out/--to-library/--flatten, just asked one at a time. */
static void do_combined(ui_app *app)
{
    if (g_visible_count == 0) {
        return;
    }
    const inst_instance *inst = g_visible[g_list.selected];

    mkp_combined_result result;
    nhp_err err = mkp_combined(inst, NULL, 0, app->config.ignore, &result);
    if (err != NHP_OK) {
        ui_set_status(app, "could not build the combined patch: %s", err_get());
        return;
    }

    char path[512];
    (void)snprintf(path, sizeof(path), "%s-combined.diff", inst->name);
    if (!ui_show_popup_input(app, "Save to: ", path, sizeof(path), draw) || path[0] == '\0') {
        ui_set_status(app, "cancelled");
        mkp_combined_result_free(&result);
        return;
    }
    err = fs_write_atomic(path, result.diff, strlen(result.diff));
    if (err != NHP_OK) {
        ui_set_status(app, "could not write %s: %s", path, err_get());
        mkp_combined_result_free(&result);
        return;
    }
    ui_set_status(app, "wrote %s (%zu files)", path, result.file_count);

    if (ui_confirm(app, "Also add to the library?", draw)) {
        char *date = today_date();
        char patch_name[160];
        (void)snprintf(patch_name, sizeof(patch_name), "%s-combined", inst->name);
        char *tmp_path = NULL;
        nhp_err lib_err =
            fs_write_temp_file("nhpm-combined-lib", result.diff, strlen(result.diff), &tmp_path);
        char *id = NULL;
        if (lib_err == NHP_OK) {
            lib_err = lib_import_local(tmp_path, patch_name, "", inst->source,
                                       (date != NULL) ? date : "1.0", &id);
        }
        if (lib_err == NHP_OK) {
            ui_set_status(app, "wrote %s; added to the library as %s %s", path, id,
                          (date != NULL) ? date : "1.0");
        } else {
            ui_set_status(app, "wrote %s but could not add to the library: %s", path, err_get());
        }
        free(id);
        if (tmp_path != NULL) {
            (void)unlink(tmp_path);
        }
        free(tmp_path);
        free(date);
    }

    if (ui_confirm(app, "Flatten into a new instance?", draw)) {
        char new_name[256] = "";
        if (!ui_show_popup_input(app, "New instance name: ", new_name, sizeof(new_name), draw) ||
            new_name[0] == '\0') {
            ui_set_status(app, "cancelled");
        } else {
            inst_instance new_inst;
            nhp_err flat_err = inst_new(new_name, inst->source, inst->recipe, &new_inst);
            char *tmp_path = NULL;
            char *id = NULL;
            if (flat_err == NHP_OK) {
                flat_err = fs_write_temp_file("nhpm-combined-flatten", result.diff,
                                              strlen(result.diff), &tmp_path);
            }
            if (flat_err == NHP_OK) {
                flat_err = lib_import_local(tmp_path, new_name, "", inst->source, "1.0", &id);
            }
            if (flat_err == NHP_OK) {
                size_t idx = 0;
                stack_conflict conflict = {0};
                char sha[65];
                sha256_hex(result.diff, strlen(result.diff), sha);
                flat_err =
                    stack_add(&new_inst, id, "1.0", new_name, tmp_path, sha, true, &idx, &conflict);
                if (flat_err == NHP_ECONFLICT) {
                    ui_show_conflict(app, &conflict);
                    stack_conflict_free(&conflict);
                }
            }
            if (flat_err == NHP_OK) {
                ui_set_status(app, "created %s with the combined patch applied", new_name);
                inst_free(&new_inst);
                reload();
            } else if (flat_err == NHP_ECONFLICT) {
                ui_set_status(app, "created %s, but the combined patch doesn't apply cleanly to it",
                              new_name);
                inst_free(&new_inst);
            } else {
                ui_set_status(app, "could not flatten into %s: %s", new_name, err_get());
            }
            free(id);
            if (tmp_path != NULL) {
                (void)unlink(tmp_path);
            }
            free(tmp_path);
        }
    }

    mkp_combined_result_free(&result);
}

static void on_search_change(ui_app *app, const char *text)
{
    (void)app;
    (void)snprintf(g_filter, sizeof(g_filter), "%s", text);
    recompute_visible();
}

/* "/": live-filters by name/source/recipe/notes, same widget and
 * Esc-clears convention as scr_db.c/scr_library.c. No "n"/"N" aliases --
 * "n" is already New here, and "N" isn't used anywhere else to match it. */
static void do_search(ui_app *app)
{
    char search_text[256];
    (void)snprintf(search_text, sizeof(search_text), "%s", g_filter);
    bool committed =
        ui_show_popup_input_live(app, "Search (name, version, recipe, notes)", search_text,
                                 sizeof(search_text), draw, on_search_change);
    (void)snprintf(g_filter, sizeof(g_filter), "%s", committed ? search_text : "");
    recompute_visible();
}

static bool handle_key(ui_app *app, wint_t ch)
{
    WINDOW *w = app->layout.main_win;
    int visible = (w != NULL) ? getmaxy(w) - 3 : 1;

    if (ch == L'/') {
        do_search(app);
        return true;
    }
    if (ch == KEY_UP || ch == L'k') {
        ui_list_move(&g_list, -1, g_visible_count, visible);
        return true;
    }
    if (ch == KEY_DOWN || ch == L'j') {
        ui_list_move(&g_list, 1, g_visible_count, visible);
        return true;
    }
    if (ch == KEY_PPAGE) {
        ui_list_move(&g_list, -visible, g_visible_count, visible);
        return true;
    }
    if (ch == KEY_NPAGE) {
        ui_list_move(&g_list, visible, g_visible_count, visible);
        return true;
    }
    if (ch == KEY_HOME) {
        ui_list_move(&g_list, -(int)g_visible_count, g_visible_count, visible);
        return true;
    }
    if (ch == KEY_END) {
        ui_list_move(&g_list, (int)g_visible_count, g_visible_count, visible);
        return true;
    }
    if (ch == L'\n' || ch == KEY_ENTER) {
        if (g_visible_count == 0) {
            return true;
        }
        const inst_instance *inst = g_visible[g_list.selected];
        nhp_err err = cfg_set_active_instance(inst->name);
        if (err == NHP_OK) {
            ui_reload_active_instance(app);
            ui_set_status(app, "using %s", inst->name);
        } else {
            ui_set_status(app, "could not switch instances: %s", err_get());
        }
        return true;
    }
    if (ch == L'n') {
        do_new(app);
        return true;
    }
    if (ch == L'c') {
        do_clone(app);
        return true;
    }
    if (ch == L'r') {
        do_rename(app);
        return true;
    }
    if (ch == L'x') {
        do_delete(app);
        return true;
    }
    if (ch == L't') {
        do_snapshots(app);
        return true;
    }
    if (ch == L'e') {
        do_export(app);
        return true;
    }
    if (ch == L'i') {
        do_import(app);
        return true;
    }
    if (ch == L'C') {
        do_combined(app);
        return true;
    }
    if (ch == L'p') {
        ui_suspend_and_run_play(app);
        return true;
    }
    return false;
}

const ui_screen scr_instances = {
    .name = "Instances",
    .nav_letter = 'i',
    .key_bar = "Tab/Left menu  Enter activate  n New  c Clone  r Rename  x Delete  t Snapshots  "
               "e Export  i Import  C Combined  p Play  / Search",
    .enter = enter,
    .leave = leave,
    .draw = draw,
    .handle_key = handle_key,
};
