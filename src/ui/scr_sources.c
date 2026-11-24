/* scr_sources.c - F7 Sources screen: base NetHack sources (folder,
 * tarball, git), per docs/features.md section 9.2.
 * SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "screen.h"

#include "instance.h"
#include "sources.h"
#include "widgets.h"

#include <curses.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* Loaded on enter(), freed on leave() -- mirrors scr_instances.c's shape.
 * `g_instances` is only for the "Instances" column and the "in use"
 * check before removing; it just reads instance.conf's `source` field. */
static bsrc_source *g_sources = NULL;
static size_t g_count = 0;
static ui_list g_list;

/* "/" search (13.9): a true filter, unlike Patches' highlight-and-jump --
 * no J/K-style reordering here, so hiding rows has no ambiguous "real
 * position" to worry about. Pointers into g_sources, rebuilt on change. */
static char g_filter[256] = "";
static const bsrc_source **g_visible = NULL;
static size_t g_visible_count = 0;

/* Last add/remove result, kept so it survives the redraw right after --
 * draw() used to overwrite it with the generic source-count line,
 * making a failed clone look like it had silently finished. */
static char g_action_status[256] = "";

static inst_instance *g_instances = NULL;
static size_t g_inst_count = 0;

/* Rebuilds g_visible from g_sources against g_filter (version + origin,
 * plain-words-all-must-match). Keeps the same entry selected if it's
 * still visible, instead of snapping to the top on every keystroke. */
static void recompute_visible(void)
{
    const bsrc_source *kept = (g_visible_count > 0 && g_list.selected < g_visible_count)
                                  ? g_visible[g_list.selected]
                                  : NULL;
    free(g_visible);
    g_visible = malloc((g_count > 0 ? g_count : 1) * sizeof(*g_visible));
    g_visible_count = 0;
    if (g_visible == NULL) {
        return;
    }
    for (size_t i = 0; i < g_count; i++) {
        const char *fields[] = {g_sources[i].version, g_sources[i].origin};
        if (str_matches_all_words(g_filter, fields, 2)) {
            g_visible[g_visible_count++] = &g_sources[i];
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
    if (g_sources != NULL) {
        bsrc_list_free(g_sources, g_count);
        g_sources = NULL;
        g_count = 0;
    }
    if (g_instances != NULL) {
        inst_list_free(g_instances, g_inst_count);
        g_instances = NULL;
        g_inst_count = 0;
    }
    (void)bsrc_list(&g_sources, &g_count);
    (void)inst_list(&g_instances, &g_inst_count);
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
    if (g_sources != NULL) {
        bsrc_list_free(g_sources, g_count);
        g_sources = NULL;
        g_count = 0;
    }
    if (g_instances != NULL) {
        inst_list_free(g_instances, g_inst_count);
        g_instances = NULL;
        g_inst_count = 0;
    }
    free(g_visible);
    g_visible = NULL;
    g_visible_count = 0;
}

static size_t usage_count(const char *version)
{
    size_t n = 0;
    for (size_t i = 0; i < g_inst_count; i++) {
        if (str_eq(g_instances[i].source, version)) {
            n++;
        }
    }
    return n;
}

static void usage_names(const char *version, buf *out)
{
    for (size_t i = 0; i < g_inst_count; i++) {
        if (str_eq(g_instances[i].source, version)) {
            if (out->len > 0) {
                (void)buf_append_str(out, ", ");
            }
            (void)buf_append_str(out, g_instances[i].name);
        }
    }
}

/* "folder:<path>" / "tarball:<path>" / "git:<url>#<tag>" (sources.h) into
 * something readable: "folder <path>", "tarball <path>", "git <tag>". */
static void format_origin(const char *origin, buf *out)
{
    const char *colon = strchr(origin, ':');
    if (colon == NULL) {
        (void)buf_append_str(out, origin);
        return;
    }
    size_t kind_len = (size_t)(colon - origin);
    const char *rest = colon + 1;
    if (kind_len == 3 && strncmp(origin, "git", 3) == 0) {
        const char *tag = strchr(rest, '#');
        (void)buf_append_fmt(out, "git %s", (tag != NULL) ? tag + 1 : rest);
    } else {
        (void)buf_append_fmt(out, "%.*s %s", (int)kind_len, origin, rest);
    }
}

/* Display-only sanity check, not a backend fact: true if `version`'s
 * tree/ folder has any top-level entry besides "include" (a real
 * source tree, not a stub). A minimal variant could still read "N". */
static bool looks_downloaded(const char *version)
{
    char *tree = bsrc_tree_path(version);
    if (tree == NULL) {
        return false;
    }
    DIR *dp = opendir(tree);
    free(tree);
    if (dp == NULL) {
        return false;
    }
    bool found_other = false;
    const struct dirent *entry;
    while (!found_other && (entry = readdir(dp)) != NULL) {
        if (str_eq(entry->d_name, ".") || str_eq(entry->d_name, "..") ||
            str_eq(entry->d_name, "include")) {
            continue;
        }
        found_other = true;
    }
    (void)closedir(dp);
    return found_other;
}

// cppcheck-suppress constParameterCallback -- must match ui_list_draw_row_fn's signature
static void draw_row(WINDOW *win, int y, int width, size_t index, bool selected, void *user_data)
{
    const ui_app *app = user_data;
    const bsrc_source *src = g_visible[index];
    bool downloaded = looks_downloaded(src->version);

    buf origin_buf;
    buf_init(&origin_buf);
    format_origin(src->origin, &origin_buf);
    char *origin_text = buf_take(&origin_buf);

    /* version(9) + " " + origin(40) + " " = 51 columns, so the glyph
     * lands at column 52 -- drawn separately so only it gets colored,
     * same split scr_db.c/scr_library.c/scr_patches.c use for their Mk. */
    buf left;
    buf_init(&left);
    buf_append_fmt(&left, "%-9.9s %-40.40s ", src->version, origin_text);
    char *left_text = buf_take(&left);

    buf right;
    buf_init(&right);
    buf_append_fmt(&right, "%zu%s", usage_count(src->version), src->is_variant ? "  variant" : "");
    char *right_text = buf_take(&right);

    bool colorize = app != NULL && !app->monochrome && !selected;

    if (selected) {
        (void)wattron(win, A_REVERSE);
    }
    (void)mvwaddnstr(win, y, 1, left_text, width);
    if (colorize) {
        (void)wattron(win, COLOR_PAIR(downloaded ? UI_COLOR_OK : UI_COLOR_ERROR));
    }
    (void)mvwaddch(win, y, 52, (chtype)(downloaded ? 'Y' : 'N'));
    if (colorize) {
        (void)wattroff(win, COLOR_PAIR(downloaded ? UI_COLOR_OK : UI_COLOR_ERROR));
    }
    if (width > 53) {
        (void)mvwaddnstr(win, y, 54, right_text, width - 53);
    }
    if (selected) {
        (void)wattroff(win, A_REVERSE);
    }
    free(left_text);
    free(right_text);
    free(origin_text);
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
    const bsrc_source *src = g_visible[g_list.selected];

    buf line1;
    buf_init(&line1);
    buf_append_fmt(&line1, "Added: %s%s   Checksum: %s", src->added,
                   src->is_variant ? "   variant (compatibility marks unknown, 9.4)" : "",
                   src->sha256[0] != '\0' ? src->sha256 : "none (folder or git import)");
    char *text1 = buf_take(&line1);
    (void)mvwaddnstr(w, 1, 1, text1, inner_width);
    free(text1);

    buf used;
    buf_init(&used);
    usage_names(src->version, &used);
    char *used_text = buf_take(&used);
    buf line2;
    buf_init(&line2);
    buf_append_fmt(&line2, "Used by: %s", used_text[0] != '\0' ? used_text : "no instance");
    char *text2 = buf_take(&line2);
    (void)mvwaddnstr(w, 2, 1, text2, inner_width);
    free(text2);
    free(used_text);

    (void)wnoutrefresh(w);
}

static void draw(ui_app *app)
{
    WINDOW *w = app->layout.main_win;
    if (w == NULL) {
        return;
    }
    (void)werase(w);
    ui_draw_panel(w, g_filter[0] != '\0' ? "Base sources (filtered)" : "Base sources");
    int inner_width = getmaxx(w) - 2;
    (void)mvwaddnstr(w, 1, 1, "Version   Origin                                   DL  Instances",
                     inner_width);

    int inner_height = getmaxy(w) - 2;
    ui_list_draw(&g_list, w, 2, inner_height - 1, inner_width, g_visible_count, draw_row, app);
    (void)wnoutrefresh(w);

    draw_details(app);

    if (g_action_status[0] != '\0') {
        ui_set_status(app, "%s", g_action_status);
    } else if (g_count == 0) {
        ui_set_status(app, "no base sources yet -- n picks a known version, or a/t/g add your own");
    } else if (g_visible_count == 0) {
        ui_set_status(app, "no base source matches \"%s\"", g_filter);
    } else if (g_filter[0] != '\0') {
        ui_set_status(app, "%zu of %zu base source%s, filtered", g_visible_count, g_count,
                      g_count == 1 ? "" : "s");
    } else {
        ui_set_status(app, "%zu base source%s", g_count, g_count == 1 ? "" : "s");
    }
}

/* Sets both the status line and g_action_status (so it's still there on
 * the redraw right after) -- every add/remove action's final result goes
 * through this, not plain ui_set_status(), which draw() would clobber. */
static void set_action_status(ui_app *app, const char *fmt, ...) NHP_PRINTF(2, 3);

static void set_action_status(ui_app *app, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(g_action_status, sizeof(g_action_status), fmt, ap);
    va_end(ap);
    ui_set_status(app, "%s", g_action_status);
}

/* Builds the "replace it?" prompt text shared by all three do_add_*
 * functions below, and by the known-versions dialog's own Enter. */
static void format_replace_prompt(const char *version, char *out, size_t out_size)
{
    (void)snprintf(out, out_size,
                   "A base source for %s already exists -- replace it with this one?", version);
}

/* ---- known-versions popup ('n'): pick a hand-verified version instead
 * of hunting down a URL or tag yourself -- a/t/g below stay as the
 * manual add flow for anything not in this list. */

/* net_download()'s progress callback. Esc/`q` cancels (13.6) -- `q` also
 * flags quit_requested, same reasoning as scr_build.c's on_build_line():
 * normal key handling is blocked during a download. */
static bool known_download_progress(void *user_data, double done_bytes, double total_bytes)
{
    ui_app *app = user_data;
    char text[64];
    double fraction;
    if (total_bytes > 0) {
        (void)snprintf(text, sizeof(text), "downloading... %.1f/%.1f MB", done_bytes / 1e6,
                       total_bytes / 1e6);
        fraction = done_bytes / total_bytes;
    } else {
        (void)snprintf(text, sizeof(text), "downloading... %.1f MB", done_bytes / 1e6);
        fraction = -1.0;
    }
    ui_draw_progress(app, fraction, text);

    (void)nodelay(stdscr, TRUE);
    wint_t ch = 0;
    int r = wget_wch(stdscr, &ch);
    (void)nodelay(stdscr, FALSE);
    if (r != ERR && (ch == 27 || ch == L'q')) {
        if (ch != 27) {
            app->quit_requested = true;
        }
        return true;
    }
    return false;
}

static void do_pick_known_version(ui_app *app)
{
    char **labels = malloc(BSRC_KNOWN_VERSION_COUNT * sizeof(*labels));
    if (labels == NULL) {
        set_action_status(app, "out of memory");
        return;
    }
    for (size_t i = 0; i < BSRC_KNOWN_VERSION_COUNT; i++) {
        const bsrc_known_version *e = &BSRC_KNOWN_VERSIONS[i];
        buf line;
        buf_init(&line);
        buf_append_fmt(&line, "%-9.9s %-9s %s", e->version,
                       e->kind == BSRC_KNOWN_TARBALL ? "tarball" : "github", e->location);
        labels[i] = buf_take(&line);
    }

    size_t index = 0;
    bool picked = ui_show_popup_menu(app, "Known versions", (const char *const *)labels,
                                     BSRC_KNOWN_VERSION_COUNT, &index, draw);
    for (size_t i = 0; i < BSRC_KNOWN_VERSION_COUNT; i++) {
        free(labels[i]);
    }
    free(labels);
    if (!picked) {
        return;
    }

    const bsrc_known_version *entry = &BSRC_KNOWN_VERSIONS[index];
    if (entry->kind == BSRC_KNOWN_TARBALL) {
        ui_set_status(app, "downloading %s...", entry->version);
    } else {
        ui_set_status(app, "cloning %s...", entry->version);
    }
    ui_flush_status(app);

    char *version = NULL;
    nhp_err err = bsrc_add_known(entry, known_download_progress, app, &version);
    if (err == NHP_EEXIST && version != NULL) {
        char message[200];
        format_replace_prompt(version, message, sizeof(message));
        if (ui_confirm(app, message, draw)) {
            nhp_err rm_err = bsrc_remove(version);
            free(version);
            version = NULL;
            err = (rm_err == NHP_OK) ? bsrc_add_known(entry, known_download_progress, app, &version)
                                     : rm_err;
        }
    }
    if (err == NHP_OK) {
        set_action_status(app, "added base source %s", version);
        reload();
    } else if (err == NHP_ECANCELED) {
        set_action_status(app, "canceled adding %s", entry->version);
    } else {
        set_action_status(app, "could not add %s: %s", entry->version, err_get());
    }
    free(version);
}

static void do_add_folder(ui_app *app)
{
    char path[512] = "";
    if (!ui_show_popup_input(app, "Add from folder, path", path, sizeof(path), draw) ||
        path[0] == '\0') {
        set_action_status(app, "cancelled");
        return;
    }
    char hint[64] = "";
    (void)ui_show_popup_input(app, "Version (blank to read include/patchlevel.h)", hint,
                              sizeof(hint), draw);

    char *version = NULL;
    nhp_err err = bsrc_add_from_folder(path, hint, &version);
    if (err == NHP_EEXIST && version != NULL) {
        /* *out_version is the conflicting version even on failure
         * (sources.h's contract) -- offering to replace it here avoids
         * sending the user back to press x themselves first. */
        char message[200];
        format_replace_prompt(version, message, sizeof(message));
        if (ui_confirm(app, message, draw)) {
            nhp_err rm_err = bsrc_remove(version);
            free(version);
            version = NULL;
            err = (rm_err == NHP_OK) ? bsrc_add_from_folder(path, hint, &version) : rm_err;
        }
    }
    if (err == NHP_OK) {
        set_action_status(app, "added base source %s", version);
        reload();
    } else {
        set_action_status(app, "could not add %s: %s", path, err_get());
    }
    free(version);
}

static void do_add_tarball(ui_app *app)
{
    char path[512] = "";
    if (!ui_show_popup_input(app, "Add from tarball, path", path, sizeof(path), draw) ||
        path[0] == '\0') {
        set_action_status(app, "cancelled");
        return;
    }
    char hint[64] = "";
    (void)ui_show_popup_input(app, "Version (blank to read include/patchlevel.h)", hint,
                              sizeof(hint), draw);

    char *version = NULL;
    nhp_err err = bsrc_add_from_tarball(path, hint, &version);
    if (err == NHP_EEXIST && version != NULL) {
        char message[200];
        format_replace_prompt(version, message, sizeof(message));
        if (ui_confirm(app, message, draw)) {
            nhp_err rm_err = bsrc_remove(version);
            free(version);
            version = NULL;
            err = (rm_err == NHP_OK) ? bsrc_add_from_tarball(path, hint, &version) : rm_err;
        }
    }
    if (err == NHP_OK) {
        set_action_status(app, "added base source %s", version);
        reload();
    } else {
        set_action_status(app, "could not add %s: %s", path, err_get());
    }
    free(version);
}

static void do_add_git(ui_app *app)
{
    char tag[128] = "";
    if (!ui_show_popup_input(app, "Add from git, tag (e.g. NetHack-3.6.7_Released)", tag,
                             sizeof(tag), draw) ||
        tag[0] == '\0') {
        set_action_status(app, "cancelled");
        return;
    }

    ui_set_status(app, "cloning %s...", tag);
    ui_flush_status(app);

    char *version = NULL;
    nhp_err err = bsrc_add_from_git(tag, &version);
    if (err == NHP_EEXIST && version != NULL) {
        char message[200];
        format_replace_prompt(version, message, sizeof(message));
        if (ui_confirm(app, message, draw)) {
            nhp_err rm_err = bsrc_remove(version);
            free(version);
            version = NULL;
            if (rm_err == NHP_OK) {
                ui_set_status(app, "cloning %s...", tag);
                ui_flush_status(app);
                err = bsrc_add_from_git(tag, &version);
            } else {
                err = rm_err;
            }
        }
    }
    if (err == NHP_OK) {
        set_action_status(app, "added base source %s", version);
        reload();
    } else {
        set_action_status(app, "could not clone %s: %s", tag, err_get());
    }
    free(version);
}

static void do_remove(ui_app *app)
{
    if (g_visible_count == 0) {
        return;
    }
    char *version = str_dup(g_visible[g_list.selected]->version);
    size_t used = usage_count(version);

    char message[400];
    if (used > 0) {
        buf used_buf;
        buf_init(&used_buf);
        usage_names(version, &used_buf);
        char *used_text = buf_take(&used_buf);
        (void)snprintf(message, sizeof(message),
                       "Remove base source %s? %s still use%s it, but keep their own copy of "
                       "the source -- only re-creating another instance from %s needs it again.",
                       version, used_text, used == 1 ? "s" : "", version);
        free(used_text);
    } else {
        (void)snprintf(message, sizeof(message), "Remove base source %s?", version);
    }

    if (!ui_confirm(app, message, draw)) {
        free(version);
        return;
    }

    nhp_err err = bsrc_remove(version);
    if (err == NHP_OK) {
        set_action_status(app, "removed base source %s", version);
        reload(); /* recompute_visible() resets selection; the removed entry is gone either way */
    } else {
        set_action_status(app, "could not remove %s: %s", version, err_get());
    }
    free(version);
}

static void on_search_change(ui_app *app, const char *text)
{
    (void)app;
    (void)snprintf(g_filter, sizeof(g_filter), "%s", text);
    recompute_visible();
}

/* "/": live-filters by version/origin, same widget and Esc-clears
 * convention as scr_db.c/scr_library.c. No "n"/"N" next-match aliases
 * here -- both letters are already taken (Known version / nothing free). */
static void do_search(ui_app *app)
{
    char search_text[256];
    (void)snprintf(search_text, sizeof(search_text), "%s", g_filter);
    bool committed = ui_show_popup_input_live(app, "Search (version, origin)", search_text,
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
    if (ch == L'n') {
        do_pick_known_version(app);
        return true;
    }
    if (ch == L'a') {
        do_add_folder(app);
        return true;
    }
    if (ch == L't') {
        do_add_tarball(app);
        return true;
    }
    if (ch == L'g') {
        do_add_git(app);
        return true;
    }
    if (ch == L'x') {
        do_remove(app);
        return true;
    }
    return false;
}

const ui_screen scr_sources = {
    .name = "Sources",
    .nav_letter = 's',
    .key_bar = "Tab/Left menu  n Known version  a Add folder  t Add tarball  g Add git tag  "
               "x Remove  / Search",
    .enter = enter,
    .leave = leave,
    .draw = draw,
    .handle_key = handle_key,
};
