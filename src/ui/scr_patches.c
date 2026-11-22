/* scr_patches.c - F5 Patches screen: the active instance's Quilt stack.
 * SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "screen.h"

#include "library.h"
#include "proc.h"
#include "stack.h"
#include "version.h"
#include "widgets.h"

#include <curses.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ui_list g_list;

/* The active instance's own version, parsed once on enter() -- same
 * idea as scr_db.c's own g_inst_version, except here it's always the
 * instance whose patches are on screen, not some other active one. */
static ver_version g_inst_version;
static bool g_have_inst_version = false;

/* docs/features.md 12.4's N/E/R workflow: which patch id "R" acts on, set
 * by a successful "N"/"E" and cleared once "R" saves it or it's gone.
 * g_editing_name is non-"" only right after "N" (no library entry yet). */
static char g_editing_id[128] = "";
static char g_editing_name[160] = "";

static void enter(ui_app *app)
{
    /* Reload from disk rather than trusting app->active_inst to already
     * be current -- guarantees this shows whatever another screen's
     * writes (e.g. Database's "D") just saved. */
    ui_reload_active_instance(app);
    ui_list_init(&g_list);

    if (g_have_inst_version) {
        ver_free(&g_inst_version);
        g_have_inst_version = false;
    }
    g_have_inst_version =
        app->active_loaded && ver_parse(app->active_inst.source, &g_inst_version) == NHP_OK;
}

// cppcheck-suppress constParameterCallback -- must match ui_screen.leave's signature
static void leave(ui_app *app)
{
    (void)app;
    if (g_have_inst_version) {
        ver_free(&g_inst_version);
        g_have_inst_version = false;
    }
}

/* '~' fuzzy, '=' exact, '!' conflict, '?' unknown (docs/features.md
 * section 9.4) -- duplicated from scr_db.c's own compat_mark() (each
 * screen reads a different struct's target field). */
static char compat_mark(const char *target)
{
    if (!g_have_inst_version || target == NULL || target[0] == '\0') {
        return '?';
    }
    ver_version site_ver;
    if (ver_parse(target, &site_ver) != NHP_OK) {
        return '?';
    }
    char mark = ver_compat_mark(ver_compatibility(&g_inst_version, &site_ver));
    ver_free(&site_ver);
    return mark;
}

/* The color role for one compat_mark() result -- duplicated from
 * scr_db.c's own mark_color(). */
static enum ui_color_pair mark_color(char mark)
{
    switch (mark) {
        case '=':
            return UI_COLOR_OK;
        case '~':
            return UI_COLOR_WARN;
        case '!':
            return UI_COLOR_ERROR;
        default:
            return UI_COLOR_DIM;
    }
}

/* Best-effort: "" for a patch whose library entry is gone (deleted from
 * the library after being added to this instance -- the instance keeps
 * its own copy regardless, docs/features.md section 8). */
static void lookup_name_and_target(const char *id, char **out_name, char **out_target)
{
    *out_name = NULL;
    *out_target = NULL;
    lib_patch lp;
    if (lib_find(id, &lp) != NHP_OK) {
        return;
    }
    *out_name = str_dup(lp.name);
    *out_target = str_dup(lp.target);
    lib_patch_free(&lp);
}

static void draw_row(WINDOW *win, int y, int width, size_t index, bool selected, void *user_data)
{
    ui_app *app = user_data;
    const inst_patch *patch = &app->active_inst.patches[index];

    char *name = NULL;
    char *target = NULL;
    lookup_name_and_target(patch->id, &name, &target);
    char mark = compat_mark(target);

    buf line;
    buf_init(&line);
    buf_append_fmt(&line, "  %2zu %-3s %-5s %-24.24s %-9.9s %-8s%s", index + 1,
                   patch->enabled ? "on" : "off", patch->id, name != NULL ? name : "(unknown)",
                   patch->version, target != NULL ? target : "",
                   patch->modified ? "  (hand-fixed)" : "");
    char *text = buf_take(&line);

    bool colorize = !app->monochrome && !selected;

    if (selected) {
        (void)wattron(win, A_REVERSE);
    }
    if (colorize) {
        (void)wattron(win, COLOR_PAIR(mark_color(mark)));
    }
    (void)mvwaddch(win, y, 1, (chtype)mark);
    if (colorize) {
        (void)wattroff(win, COLOR_PAIR(mark_color(mark)));
    }
    (void)mvwaddnstr(win, y, 2, text, width - 1);
    if (selected) {
        (void)wattroff(win, A_REVERSE);
    }
    free(text);
    free(name);
    free(target);
}

static void draw_details(ui_app *app)
{
    WINDOW *w = app->layout.details;
    if (w == NULL) {
        return;
    }
    (void)werase(w);
    ui_draw_panel(w, "Details");
    size_t count = app->active_inst.patch_count;
    if (g_list.selected < count) {
        const inst_patch *patch = &app->active_inst.patches[g_list.selected];
        int inner_width = getmaxx(w) - 2;

        buf line;
        buf_init(&line);
        buf_append_fmt(&line, "%s %s  strip: %s  sha256: %.16s%s", patch->id, patch->version,
                       patch->strip < 0 ? "not yet determined" : "known",
                       patch->sha256[0] != '\0' ? patch->sha256 : "(unknown)",
                       patch->sha256[0] != '\0' ? "..." : "");
        char *text = buf_take(&line);
        (void)mvwaddnstr(w, 1, 1, text, inner_width);
        free(text);
    }
    (void)wnoutrefresh(w);
}

static void draw(ui_app *app)
{
    WINDOW *w = app->layout.main_win;
    if (w == NULL) {
        return;
    }
    (void)werase(w);
    ui_draw_panel(w, "Patches");
    int inner_width = getmaxx(w) - 2;

    if (!app->active_loaded) {
        (void)mvwaddnstr(w, 1, 1, "no active instance -- pick one on the Instances screen",
                         inner_width);
        (void)wnoutrefresh(w);
        draw_details(app);
        return;
    }

    (void)mvwaddnstr(w, 1, 1, "Mk #  On  ID    Patch                    Version   For",
                     inner_width);
    size_t count = app->active_inst.patch_count;
    int inner_height = getmaxy(w) - 2;
    ui_list_draw(&g_list, w, 2, inner_height - 1, inner_width, count, draw_row, app);
    (void)wnoutrefresh(w);

    draw_details(app);

    if (count == 0) {
        ui_set_status(app, "no patches on %s yet -- add one from the Database screen",
                      app->active_inst.name);
    }
}

/* Mirrors stack.c's own find_patch_filename() (private there): scans
 * <src_dir>/patches/ for a file named `id` or "<id>-*". Duplicated since
 * it's small and stack.c doesn't export it (mkpatch.c does too). */
static char *find_patch_filename(const char *src_dir, const char *id)
{
    char *patches_dir = path_join(src_dir, "patches");
    if (patches_dir == NULL) {
        return NULL;
    }
    DIR *dp = opendir(patches_dir);
    free(patches_dir);
    if (dp == NULL) {
        return NULL;
    }

    size_t id_len = strlen(id);
    char *found = NULL;
    const struct dirent *entry;
    while (found == NULL && (entry = readdir(dp)) != NULL) {
        size_t name_len = strlen(entry->d_name);
        if (name_len > 0 && entry->d_name[name_len - 1] == '~') {
            continue;
        }
        bool is_exact = str_eq(entry->d_name, id);
        bool is_prefixed = strncmp(entry->d_name, id, id_len) == 0 && entry->d_name[id_len] == '-';
        if (is_exact || is_prefixed) {
            found = str_dup(entry->d_name);
        }
    }
    (void)closedir(dp);
    return found;
}

/* docs/features.md 13.9's "d Diff view": shows the selected patch's
 * actual applied diff, from the instance's own patches/ folder, not the
 * library's copy -- a hand-fixed patch's two copies can differ. */
static void do_diff(ui_app *app)
{
    size_t count = app->active_inst.patch_count;
    if (count == 0) {
        return;
    }
    const inst_patch *patch = &app->active_inst.patches[g_list.selected];

    char *src_dir = inst_source_full_path(&app->active_inst);
    char *filename = (src_dir != NULL) ? find_patch_filename(src_dir, patch->id) : NULL;
    if (filename == NULL) {
        ui_set_status(app, "could not find %s's patch file", patch->id);
        free(src_dir);
        return;
    }
    char *patches_dir = path_join(src_dir, "patches");
    char *abs_path = (patches_dir != NULL) ? path_join(patches_dir, filename) : NULL;
    free(patches_dir);
    free(filename);
    free(src_dir);
    if (abs_path == NULL) {
        ui_set_status(app, "out of memory");
        return;
    }

    char *data = NULL;
    size_t len = 0;
    nhp_err err = fs_read_file(abs_path, &data, &len);
    free(abs_path);
    if (err != NHP_OK) {
        ui_set_status(app, "could not read %s's diff: %s", patch->id, err_get());
        return;
    }

    char title[160];
    (void)snprintf(title, sizeof(title), "Diff -- %s %s", patch->id, patch->version);
    ui_show_text_dialog(app, title, data);
    free(data);
}

/* Resolves the editor ($EDITOR, then config's `editor`, else "vi") and execs
 * it on `path`; caller hands over the real terminal first (13.7). Splits on
 * whitespace, no quoting, like other small command lines. */
static nhp_err run_editor(const cfg_app *config, const char *path)
{
    const char *editor = getenv("EDITOR");
    if (editor == NULL || editor[0] == '\0') {
        editor = config->editor;
    }
    if (editor == NULL || editor[0] == '\0') {
        editor = "vi";
    }

    char *copy = str_dup(editor);
    if (copy == NULL) {
        return NHP_ENOMEM;
    }

    const char *argv[18];
    size_t n = 0;
    char *saveptr = NULL;
    const char *tok = strtok_r(copy, " \t", &saveptr);
    while (tok != NULL && n < 16) {
        argv[n++] = tok;
        tok = strtok_r(NULL, " \t", &saveptr);
    }
    if (n == 0) {
        free(copy);
        err_set("no editor configured");
        return NHP_EINVAL;
    }
    argv[n++] = path; /* n is at most 16 here, so this and the next line land on 16/17 of 18 */
    // cppcheck-suppress arrayIndexOutOfBoundsCond -- false positive: n <= 17 here, argv[18] holds
    argv[n] = NULL;

    bool signaled = false;
    int code = 0;
    nhp_err err = proc_run_interactive(argv, NULL, NULL, &signaled, &code);
    free(copy);
    return err;
}

/* docs/features.md 12.3's "o": pick a file, track it into whatever
 * patch is on top (best-effort -- harmless if nothing's on top; Capture
 * ("w") finds the change either way), then hand the terminal to $EDITOR. */
static void do_open_editor(ui_app *app)
{
    char rel_path[512] = "";
    if (!ui_prompt_line(app, "File to edit (relative to the instance's source): ", rel_path,
                        sizeof(rel_path), draw) ||
        rel_path[0] == '\0') {
        ui_set_status(app, "cancelled");
        return;
    }

    nhp_err track_err = stack_track_file(&app->active_inst, rel_path);

    char *src_dir = inst_source_full_path(&app->active_inst);
    char *full_path = (src_dir != NULL) ? path_join(src_dir, rel_path) : NULL;
    free(src_dir);
    if (full_path == NULL) {
        ui_set_status(app, "out of memory");
        return;
    }

    def_prog_mode();
    (void)endwin();
    nhp_err err = run_editor(&app->config, full_path);
    (void)reset_prog_mode();
    (void)clearok(curscr, TRUE);
    (void)refresh();
    free(full_path);

    if (err != NHP_OK) {
        ui_set_status(app, "could not run the editor: %s", err_get());
    } else if (track_err == NHP_OK) {
        ui_set_status(app, "edited %s", rel_path);
    } else {
        ui_set_status(app, "edited %s (not tracked: %s)", rel_path, err_get());
    }
}

/* docs/features.md 12.3's "N" shortcut: a brand-new, empty patch pushed
 * on top right away, ready for "o"/"R" to fill in. */
static void do_new_patch(ui_app *app)
{
    char name[160] = "";
    if (!ui_prompt_line(app, "New patch name: ", name, sizeof(name), draw) || name[0] == '\0') {
        ui_set_status(app, "cancelled");
        return;
    }

    char *id = lib_make_local_id(name);
    if (id == NULL) {
        ui_set_status(app, "out of memory");
        return;
    }

    size_t out_index = 0;
    nhp_err err = stack_new_patch(&app->active_inst, id, name, &out_index);
    if (err != NHP_OK) {
        ui_set_status(app, "could not create %s: %s", name, err_get());
        free(id);
        return;
    }

    (void)snprintf(g_editing_id, sizeof(g_editing_id), "%s", id);
    (void)snprintf(g_editing_name, sizeof(g_editing_name), "%s", name);
    free(id);

    ui_reload_active_instance(app);
    g_list.selected = out_index;
    g_list.top = out_index;
    ui_set_status(app, "%s is on top -- edit files (o tracks new ones), then R to save it",
                  g_editing_id);
}

/* docs/features.md 12.4's "E": pops the stack to put the selected patch
 * on top, ready to edit directly, same shape as "N" above. */
static void do_edit_begin(ui_app *app)
{
    if (app->active_inst.patch_count == 0) {
        return;
    }
    size_t index = g_list.selected;
    const char *id = app->active_inst.patches[index].id;

    stack_conflict conflict;
    nhp_err err = stack_edit_begin(&app->active_inst, index, &conflict);
    if (err == NHP_ECONFLICT) {
        ui_show_conflict(app, &conflict);
        stack_conflict_free(&conflict);
        return;
    }
    if (err != NHP_OK) {
        ui_set_status(app, "could not edit %s: %s", id, err_get());
        return;
    }

    (void)snprintf(g_editing_id, sizeof(g_editing_id), "%s", id);
    g_editing_name[0] = '\0';
    ui_reload_active_instance(app);
    ui_set_status(app, "%s is on top -- edit files (o tracks new ones), then R to save it",
                  g_editing_id);
}

/* docs/features.md 12.4's "R": quilt-refreshes whatever "N"/"E" put on top
 * and saves it as a new library version. A local- id bumps its version; a
 * downloaded one forks into a local- variant via lib_fork_as_local(). */
static void do_refresh(ui_app *app)
{
    if (g_editing_id[0] == '\0') {
        ui_set_status(app, "no patch is being edited -- press N or E first");
        return;
    }

    size_t index = SIZE_MAX;
    nhp_err err = inst_find_patch_index(&app->active_inst, g_editing_id, &index);
    if (err != NHP_OK) {
        ui_set_status(app, "%s is no longer on the stack", g_editing_id);
        g_editing_id[0] = '\0';
        g_editing_name[0] = '\0';
        return;
    }

    stack_conflict conflict;
    char *diff_path = NULL;
    err = stack_edit_refresh(&app->active_inst, index, &conflict, &diff_path);
    if (err == NHP_ECONFLICT) {
        ui_show_conflict(app, &conflict);
        stack_conflict_free(&conflict);
        return;
    }
    if (err != NHP_OK) {
        ui_set_status(app, "could not refresh %s: %s", g_editing_id, err_get());
        return;
    }

    char status_msg[256];
    bool is_local = strncmp(g_editing_id, "local-", 6) == 0;
    if (is_local) {
        char *new_version = NULL;
        nhp_err lib_err = lib_add_version(g_editing_id, g_editing_name, app->config.author_name, "",
                                          "", diff_path, &new_version);
        if (lib_err == NHP_OK) {
            (void)snprintf(status_msg, sizeof(status_msg), "refreshed %s, saved as version %s",
                           g_editing_id, new_version);
        } else {
            (void)snprintf(status_msg, sizeof(status_msg),
                           "refreshed %s on disk, but could not save a library version: %s",
                           g_editing_id, err_get());
        }
        free(new_version);
    } else {
        lib_patch original;
        nhp_err find_err = lib_find(g_editing_id, &original);
        if (find_err != NHP_OK) {
            (void)snprintf(status_msg, sizeof(status_msg),
                           "refreshed %s on disk, but could not look it up to fork it: %s",
                           g_editing_id, err_get());
        } else {
            char *new_id = NULL;
            char *new_version = NULL;
            nhp_err fork_err = lib_fork_as_local(
                g_editing_id, app->active_inst.patches[index].version, original.name,
                original.author, original.target, diff_path, &new_id, &new_version);
            if (fork_err == NHP_OK) {
                (void)snprintf(status_msg, sizeof(status_msg),
                               "refreshed %s, saved as %s version %s", g_editing_id, new_id,
                               new_version);
            } else {
                (void)snprintf(status_msg, sizeof(status_msg),
                               "refreshed %s on disk, but could not save it as a local variant: %s",
                               g_editing_id, err_get());
            }
            free(new_id);
            free(new_version);
            lib_patch_free(&original);
        }
    }
    free(diff_path);

    g_editing_id[0] = '\0';
    g_editing_name[0] = '\0';
    ui_reload_active_instance(app);
    ui_set_status(app, "%s", status_msg);
}

static bool handle_key(ui_app *app, wint_t ch)
{
    if (!app->active_loaded) {
        return false;
    }
    WINDOW *w = app->layout.main_win;
    int visible = (w != NULL) ? getmaxy(w) - 3 : 1;
    size_t count = app->active_inst.patch_count;

    if (ch == KEY_UP || ch == L'k') {
        ui_list_move(&g_list, -1, count, visible);
        return true;
    }
    if (ch == KEY_DOWN || ch == L'j') {
        ui_list_move(&g_list, 1, count, visible);
        return true;
    }
    if (ch == KEY_PPAGE) {
        ui_list_move(&g_list, -visible, count, visible);
        return true;
    }
    if (ch == KEY_NPAGE) {
        ui_list_move(&g_list, visible, count, visible);
        return true;
    }
    if (ch == KEY_HOME) {
        ui_list_move(&g_list, -(int)count, count, visible);
        return true;
    }
    if (ch == KEY_END) {
        ui_list_move(&g_list, (int)count, count, visible);
        return true;
    }

    /* N/o/R work even with zero patches: N creates the first one, o opens
     * any file with nothing on top, and R looks its target up by id, not
     * list position. E needs a row, so it stays behind the count==0 guard. */
    if (ch == L'N') {
        if (ui_require_write_access(app)) {
            do_new_patch(app);
        }
        return true;
    }
    if (ch == L'o') {
        if (ui_require_write_access(app)) {
            do_open_editor(app);
        }
        return true;
    }
    if (ch == L'R') {
        if (ui_require_write_access(app)) {
            do_refresh(app);
        }
        return true;
    }

    if (count == 0) {
        return false;
    }
    size_t index = g_list.selected;
    const char *id = app->active_inst.patches[index].id;

    if (ch == L' ') {
        if (!ui_require_write_access(app)) {
            return true;
        }
        bool was_enabled = app->active_inst.patches[index].enabled;
        stack_conflict conflict;
        nhp_err err = was_enabled ? stack_disable(&app->active_inst, index, &conflict)
                                  : stack_enable(&app->active_inst, index, &conflict);
        if (err == NHP_OK) {
            ui_reload_active_instance(app);
            ui_set_status(app, "%s %s", was_enabled ? "disabled" : "enabled", id);
        } else if (err == NHP_ECONFLICT) {
            ui_show_conflict(app, &conflict);
            stack_conflict_free(&conflict);
        } else {
            ui_set_status(app, "could not change %s: %s", id, err_get());
        }
        return true;
    }
    if (ch == L'J' && index + 1 < count) {
        if (!ui_require_write_access(app)) {
            return true;
        }
        stack_conflict conflict;
        nhp_err err = stack_move(&app->active_inst, index, index + 1, &conflict);
        if (err == NHP_OK) {
            ui_reload_active_instance(app);
            ui_list_move(&g_list, 1, count, visible);
            ui_set_status(app, "moved %s down", id);
        } else if (err == NHP_ECONFLICT) {
            ui_show_conflict(app, &conflict);
            stack_conflict_free(&conflict);
        } else {
            ui_set_status(app, "could not move %s: %s", id, err_get());
        }
        return true;
    }
    if (ch == L'K' && index > 0) {
        if (!ui_require_write_access(app)) {
            return true;
        }
        stack_conflict conflict;
        nhp_err err = stack_move(&app->active_inst, index, index - 1, &conflict);
        if (err == NHP_OK) {
            ui_reload_active_instance(app);
            ui_list_move(&g_list, -1, count, visible);
            ui_set_status(app, "moved %s up", id);
        } else if (err == NHP_ECONFLICT) {
            ui_show_conflict(app, &conflict);
            stack_conflict_free(&conflict);
        } else {
            ui_set_status(app, "could not move %s: %s", id, err_get());
        }
        return true;
    }
    if (ch == L'w') {
        ui_show_changes_dialog(app);
        ui_reload_active_instance(app);
        ui_list_init(&g_list);
        return true;
    }
    if (ch == L'd') {
        do_diff(app);
        return true;
    }
    if (ch == L'E') {
        if (ui_require_write_access(app)) {
            do_edit_begin(app);
        }
        return true;
    }
    if (ch == L'x') {
        if (!ui_require_write_access(app)) {
            return true;
        }
        /* No confirm dialog yet -- acts right away, same as the CLI's
         * own `nhpm disable`/remove commands have no confirmation. */
        stack_conflict conflict;
        nhp_err err = stack_remove(&app->active_inst, index, &conflict);
        if (err == NHP_OK) {
            ui_reload_active_instance(app);
            if (g_list.selected > 0 && g_list.selected >= app->active_inst.patch_count) {
                g_list.selected--;
            }
            ui_set_status(app, "removed %s", id);
        } else if (err == NHP_ECONFLICT) {
            ui_show_conflict(app, &conflict);
            stack_conflict_free(&conflict);
        } else {
            ui_set_status(app, "could not remove %s: %s", id, err_get());
        }
        return true;
    }
    return false;
}

const ui_screen scr_patches = {
    .name = "Patches",
    .nav_letter = 'p',
    .key_bar = "Tab/Left menu  Space toggle  J/K move  d Diff  w Capture changes  x remove  N new "
               "patch  E edit  o open file  R refresh",
    .enter = enter,
    .leave = leave,
    .draw = draw,
    .handle_key = handle_key,
};
