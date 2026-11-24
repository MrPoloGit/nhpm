/* scr_patches.c - F5 Patches screen: the active instance's Quilt stack.
 * SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "screen.h"

#include "library.h"
#include "mkpatch.h"
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

/* docs/features.md 11.7's "c": read-only dry-run results for every
 * disabled patch, cached until the next check or any stack change
 * (indices shift, so a stale cache would point at the wrong row). */
static stack_check_result *g_check_results = NULL;
static size_t g_check_count = 0;
static bool g_checked = false;

/* docs/features.md 11.9's shared-files flag, recomputed alongside
 * g_check_results -- same "stale after a stack change" reasoning. */
static stack_shared_file *g_shared_files = NULL;
static size_t g_shared_count = 0;

/* docs/features.md 13.9's "uncaptured changes" header indicator, from
 * mkp_changes() -- recomputed, not kept live, since it's a filesystem
 * comparison (12.3 steps 1-2), not a cheap flag. */
static bool g_has_uncaptured = false;

/* "/" search (13.9), highlight-and-jump, not hide-non-matches like
 * scr_db.c/scr_library.c: row position is the real Quilt stack order
 * J/K depends on, so hiding rows would make it skip some silently. */
static char g_filter[256] = "";

static void free_check_results(void)
{
    stack_check_result_free(g_check_results);
    g_check_results = NULL;
    g_check_count = 0;
    g_checked = false;
}

/* Recomputes everything that goes stale after a stack change (enable,
 * disable, move, add, remove, undo, or anything a capture/refresh can
 * touch). Called after every ui_reload_active_instance() in this file. */
static void refresh_caches(ui_app *app)
{
    free_check_results();
    stack_shared_files_free(g_shared_files, g_shared_count);
    g_shared_files = NULL;
    g_shared_count = 0;
    g_has_uncaptured = false;

    if (!app->active_loaded) {
        return;
    }
    (void)stack_find_shared_files(&app->active_inst, &g_shared_files, &g_shared_count);

    mkp_file *files = NULL;
    size_t count = 0;
    if (mkp_changes(&app->active_inst, app->config.ignore, &files, &count) == NHP_OK) {
        g_has_uncaptured = count > 0;
    }
    mkp_file_list_free(files, count);
}

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

    refresh_caches(app);
}

// cppcheck-suppress constParameterCallback -- must match ui_screen.leave's signature
static void leave(ui_app *app)
{
    (void)app;
    if (g_have_inst_version) {
        ver_free(&g_inst_version);
        g_have_inst_version = false;
    }
    free_check_results();
    stack_shared_files_free(g_shared_files, g_shared_count);
    g_shared_files = NULL;
    g_shared_count = 0;
    g_has_uncaptured = false;
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

/* Against id/name/version/target. `name`/`target` may be NULL (lib entry
 * gone, see lookup_name_and_target() above); str_matches_all_words() skips
 * a NULL field rather than treating it as a match. */
static bool patch_matches_filter(const inst_patch *patch, const char *name, const char *target)
{
    const char *fields[] = {patch->id, name, target, patch->version};
    return str_matches_all_words(g_filter, fields, 4);
}

/* docs/features.md 11.7's "c" dry-run results, as a Status-column word:
 * "applied"/"disabled" from inst_patch itself, plus the dry-run outcome
 * for a disabled patch once g_checked (NULL before "c" has run). */
static const stack_check_result *find_check_result(size_t index)
{
    if (!g_checked) {
        return NULL;
    }
    for (size_t i = 0; i < g_check_count; i++) {
        if (g_check_results[i].index == index) {
            return &g_check_results[i];
        }
    }
    return NULL;
}

static void build_status_text(size_t index, const inst_patch *patch, char *out, size_t out_size)
{
    if (patch->enabled) {
        (void)snprintf(out, out_size, "applied%s", patch->modified ? " (modified)" : "");
        return;
    }
    const stack_check_result *r = find_check_result(index);
    if (r == NULL) {
        (void)snprintf(out, out_size, "disabled");
    } else if (r->hunks_failed == 0) {
        (void)snprintf(out, out_size, "disabled - clean");
    } else {
        (void)snprintf(out, out_size, "disabled - %d/%d fail", r->hunks_failed, r->hunks_total);
    }
}

static void draw_row(WINDOW *win, int y, int width, size_t index, bool selected, void *user_data)
{
    ui_app *app = user_data;
    const inst_patch *patch = &app->active_inst.patches[index];

    char *name = NULL;
    char *target = NULL;
    lookup_name_and_target(patch->id, &name, &target);
    char mark = compat_mark(target);

    char status_text[40];
    build_status_text(index, patch, status_text, sizeof(status_text));

    buf line;
    buf_init(&line);
    buf_append_fmt(&line, "  %2zu %-3s %-5s %-24.24s %-9.9s %-8s %-20.20s", index + 1,
                   patch->enabled ? "on" : "off", patch->id, name != NULL ? name : "(unknown)",
                   patch->version, target != NULL ? target : "", status_text);
    char *text = buf_take(&line);
    bool matches_filter = g_filter[0] != '\0' && patch_matches_filter(patch, name, target);

    bool colorize = !app->monochrome && !selected;

    if (selected) {
        (void)wattron(win, A_REVERSE);
    }
    if (matches_filter) {
        (void)wattron(win, A_BOLD);
    }
    if (colorize) {
        (void)wattron(win, COLOR_PAIR(mark_color(mark)));
    }
    (void)mvwaddch(win, y, 1, (chtype)mark);
    if (colorize) {
        (void)wattroff(win, COLOR_PAIR(mark_color(mark)));
    }
    (void)mvwaddnstr(win, y, 2, text, width - 1);
    if (matches_filter) {
        (void)wattroff(win, A_BOLD);
    }
    if (selected) {
        (void)wattroff(win, A_REVERSE);
    }
    free(text);
    free(name);
    free(target);
}

/* docs/features.md 11.9: which of `id`'s shared files (from g_shared_files)
 * it's involved in, and which other enabled patches also touch each one.
 * "" if none. */
static void shared_files_summary(const char *id, char *out, size_t out_size)
{
    buf b;
    buf_init(&b);
    bool any = false;
    for (size_t i = 0; i < g_shared_count; i++) {
        const stack_shared_file *f = &g_shared_files[i];
        bool touches = false;
        for (size_t j = 0; j < f->patch_count && !touches; j++) {
            touches = str_eq(f->patch_ids[j], id);
        }
        if (!touches) {
            continue;
        }
        buf_append_fmt(&b, "%s%s (also:", any ? "; " : "shares ", f->path);
        for (size_t j = 0; j < f->patch_count; j++) {
            if (!str_eq(f->patch_ids[j], id)) {
                buf_append_fmt(&b, " %s", f->patch_ids[j]);
            }
        }
        buf_append_fmt(&b, ")");
        any = true;
    }
    char *text = buf_take(&b);
    (void)snprintf(out, out_size, "%s", (text != NULL) ? text : "");
    free(text);
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

        char shared[200];
        shared_files_summary(patch->id, shared, sizeof(shared));
        if (shared[0] != '\0') {
            bool colorize = !app->monochrome;
            if (colorize) {
                (void)wattron(w, COLOR_PAIR(UI_COLOR_WARN));
            }
            (void)mvwaddnstr(w, 2, 1, shared, inner_width);
            if (colorize) {
                (void)wattroff(w, COLOR_PAIR(UI_COLOR_WARN));
            }
        }
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
    ui_draw_panel(w, g_filter[0] != '\0' ? "Patches (search on)" : "Patches");
    int inner_width = getmaxx(w) - 2;

    if (!app->active_loaded) {
        (void)mvwaddnstr(w, 1, 1, "no active instance -- pick one on the Instances screen",
                         inner_width);
        (void)wnoutrefresh(w);
        draw_details(app);
        return;
    }

    static const char *const header_text = "Mk #  On  ID    Patch                    Version   For"
                                           "      Status";
    (void)mvwaddnstr(w, 1, 1, header_text, inner_width);
    if (g_has_uncaptured) {
        static const char *const marker = "* uncaptured changes";
        int col = inner_width - (int)strlen(marker);
        if (col > (int)strlen(header_text)) {
            bool colorize = !app->monochrome;
            if (colorize) {
                (void)wattron(w, COLOR_PAIR(UI_COLOR_WARN));
            }
            (void)mvwaddnstr(w, 1, 1 + col, marker, (int)strlen(marker));
            if (colorize) {
                (void)wattroff(w, COLOR_PAIR(UI_COLOR_WARN));
            }
        }
    }
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
    if (!ui_show_popup_input(app, "File to edit (relative to the instance's source): ", rel_path,
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
    if (!ui_show_popup_input(app, "New patch name: ", name, sizeof(name), draw) ||
        name[0] == '\0') {
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
    refresh_caches(app);
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
    refresh_caches(app);
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
    refresh_caches(app);
    ui_set_status(app, "%s", status_msg);
}

/* docs/features.md 11.7's "c": dry-runs every disabled patch, read-only,
 * and fills in the Status column (build_status_text() above) until the
 * next stack change invalidates g_check_results. */
static void do_check(ui_app *app)
{
    free_check_results();
    nhp_err err = stack_check_disabled(&app->active_inst, &g_check_results, &g_check_count);
    if (err != NHP_OK) {
        ui_set_status(app, "could not check disabled patches: %s", err_get());
        return;
    }
    g_checked = true;
    if (g_check_count == 0) {
        ui_set_status(app, "no disabled patches to check");
    } else {
        ui_set_status(app, "checked %zu disabled patch(es)", g_check_count);
    }
}

/* docs/features.md 11.8's "t": a manual, labeled snapshot, same as
 * Instances' own Snapshots dialog "a" -- Patches just offers it without
 * leaving the screen. */
static void do_snapshot(ui_app *app)
{
    char label[128] = "";
    if (!ui_show_popup_input(app, "Snapshot label (blank for none): ", label, sizeof(label),
                             draw)) {
        ui_set_status(app, "cancelled");
        return;
    }
    char *filename = NULL;
    nhp_err err = inst_snapshot_create(app->active_inst.name, label, &filename);
    if (err == NHP_OK) {
        ui_set_status(app, "snapshot: %s", filename);
    } else {
        ui_set_status(app, "could not snapshot: %s", err_get());
    }
    free(filename);
}

/* docs/features.md 11.8's "u": restores the latest "auto-"-labeled
 * snapshot taken before the last Enable/Disable/Move/Add/Remove. */
static void do_undo(ui_app *app)
{
    stack_conflict conflict;
    nhp_err err = stack_undo(&app->active_inst, &conflict);
    if (err == NHP_OK) {
        ui_reload_active_instance(app);
        refresh_caches(app);
        if (g_list.selected > 0 && g_list.selected >= app->active_inst.patch_count) {
            g_list.selected = app->active_inst.patch_count - 1;
        }
        ui_set_status(app, "undid the last change");
    } else if (err == NHP_ECONFLICT) {
        ui_show_conflict(app, &conflict);
        stack_conflict_free(&conflict);
    } else {
        ui_set_status(app, "could not undo: %s", err_get());
    }
}

static void on_search_change(ui_app *app, const char *text)
{
    (void)app;
    (void)snprintf(g_filter, sizeof(g_filter), "%s", text);
}

/* "/": highlights matches live as you type (on_search_change() above), same
 * widget Database/Library use, but doesn't hide anything -- see g_filter's
 * own comment for why. Esc clears the filter entirely, same convention. */
static void do_search(ui_app *app)
{
    char search_text[256];
    (void)snprintf(search_text, sizeof(search_text), "%s", g_filter);
    bool committed = ui_show_popup_input_live(app, "Search (highlights matches; n jumps)",
                                              search_text, sizeof(search_text), draw,
                                              on_search_change);
    (void)snprintf(g_filter, sizeof(g_filter), "%s", committed ? search_text : "");
}

/* "n": jumps the selection to the next match, wrapping around. Unlike
 * Database/Library's own "n" (just an alias for "down" on an already-
 * filtered list), this is the only way to reach a match here. */
static void do_jump_to_match(ui_app *app, size_t count, int visible)
{
    if (g_filter[0] == '\0') {
        ui_set_status(app, "no active filter -- / to search");
        return;
    }
    if (count == 0) {
        return;
    }
    for (size_t step = 1; step <= count; step++) {
        size_t idx = (g_list.selected + step) % count;
        const inst_patch *patch = &app->active_inst.patches[idx];
        char *name = NULL;
        char *target = NULL;
        lookup_name_and_target(patch->id, &name, &target);
        bool matches = patch_matches_filter(patch, name, target);
        free(name);
        free(target);
        if (matches) {
            g_list.selected = idx;
            ui_list_move(&g_list, 0, count, visible); /* just fixes scroll */
            ui_set_status(app, "match: %s", patch->id);
            return;
        }
    }
    ui_set_status(app, "no match for \"%s\"", g_filter);
}

static bool handle_key(ui_app *app, wint_t ch)
{
    if (!app->active_loaded) {
        return false;
    }
    WINDOW *w = app->layout.main_win;
    int visible = (w != NULL) ? getmaxy(w) - 3 : 1;
    size_t count = app->active_inst.patch_count;

    if (ch == L'/') {
        do_search(app);
        return true;
    }
    if (ch == L'n') {
        do_jump_to_match(app, count, visible);
        return true;
    }
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
    if (ch == L'c') {
        do_check(app);
        return true;
    }
    if (ch == L't') {
        do_snapshot(app);
        return true;
    }
    if (ch == L'u') {
        if (ui_require_write_access(app)) {
            do_undo(app);
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
            refresh_caches(app);
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
            refresh_caches(app);
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
            refresh_caches(app);
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
        refresh_caches(app);
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
            refresh_caches(app);
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
               "patch  E edit  o open file  R refresh  c Check  t Snapshot  u Undo  / Search  "
               "n Next match",
    .enter = enter,
    .leave = leave,
    .draw = draw,
    .handle_key = handle_key,
};
