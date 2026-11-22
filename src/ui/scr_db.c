/* scr_db.c - F3 Database screen: browse/search the Patch Database,
 * download, and add straight to the active instance.
 * SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "screen.h"

#include "index.h"
#include "instance.h"
#include "library.h"
#include "site.h"
#include "stack.h"
#include "version.h"
#include "widgets.h"

#include <curses.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Loaded on enter(), freed on leave() -- mirrors scr_instances.c's
 * "static state scoped to the file" shape. */
static idx_index g_index;
static bool g_index_loaded = false;
static const idx_entry **g_results = NULL;
static size_t g_result_count = 0;
static ui_list g_list;

/* Every instance, for the `in:` token only -- reads the already-loaded
 * patch lists, same as scr_library.c's own g_instances. Loaded once per
 * enter(), so a patch added mid-filter won't show until re-entry. */
static inst_instance *g_instances = NULL;
static size_t g_inst_count = 0;

/* nhpm's search syntax (docs/features.md 13.10), persisted across
 * screens; g_editing/g_edit_buf back '/' composing live, filtering on
 * every keystroke. `lib:`/`in:` are pulled out first -- idx_query_parse() doesn't know them. */
static char g_filter[256] = "";
static char g_edit_buf[256] = "";
static bool g_editing = false;

/* Section 13.9's "s/S next sort column/reverse" -- cycles the sortable
 * columns (Mk doesn't count, it's a fixed glyph). NONE is first so the
 * default view stays in the site's own index order until asked. */
typedef enum db_sort_col {
    DB_SORT_NONE = 0,
    DB_SORT_NAME,
    DB_SORT_AUTHOR,
    DB_SORT_FOR,
    DB_SORT_RATING,
    DB_SORT_COUNT
} db_sort_col;

static db_sort_col g_sort_col = DB_SORT_NONE;
static bool g_sort_desc = false;

/* The F3 mockup's `v` "All versions" -- off by default, collapsing every
 * downloaded version but the newest into a "(+)" suffix the way the
 * Downloaded column already did; on, it lists every one. */
static bool g_show_all_versions = false;

/* The active instance's version, parsed once on enter() so every row's
 * compatibility mark doesn't reparse it. Unset (no active instance, or
 * its version string doesn't parse) means every mark is UNKNOWN ('?'). */
static ver_version g_inst_version;
static bool g_have_inst_version = false;

/* Last do_refresh()/do_download()/do_uninstall() result, kept so it
 * survives the next redraw -- draw()'s own status logic would otherwise
 * silently overwrite it with the generic "%zu patches..." line. */
static char g_action_status[256] = "";

static void free_results(void)
{
    free(g_results);
    g_results = NULL;
    g_result_count = 0;
}

static void free_instances(void)
{
    if (g_instances != NULL) {
        inst_list_free(g_instances, g_inst_count);
        g_instances = NULL;
        g_inst_count = 0;
    }
}

/* True if instance `inst_name` has `id` anywhere in its patch stack,
 * any version -- the Database screen has no notion of "which version",
 * unlike scr_library.c's version_used_by(). False if no such instance. */
static bool id_used_by(const char *id, const char *inst_name)
{
    for (size_t i = 0; i < g_inst_count; i++) {
        const inst_instance *inst = &g_instances[i];
        if (!str_eq(inst->name, inst_name)) {
            continue;
        }
        for (size_t j = 0; j < inst->patch_count; j++) {
            if (str_eq(inst->patches[j].id, id)) {
                return true;
            }
        }
        return false;
    }
    return false;
}

/* Splits `raw` into `in:`/`lib:` tokens (last one wins) and everything
 * else, which still goes through idx_query_parse() unchanged -- mirrors
 * scr_library.c's extract_in_filter(), plus the extra `lib:`. */
static void extract_db_filters(const char *raw, char *in_out, size_t in_size, char *lib_out,
                               size_t lib_size, char *rest_out, size_t rest_size)
{
    in_out[0] = '\0';
    lib_out[0] = '\0';
    rest_out[0] = '\0';
    char *copy = str_dup(raw);
    if (copy == NULL) {
        return;
    }

    buf rest;
    buf_init(&rest);
    char *saveptr = NULL;
    char *token = strtok_r(copy, " \t", &saveptr);
    while (token != NULL) {
        if (strncasecmp(token, "in:", 3) == 0) {
            (void)snprintf(in_out, in_size, "%s", token + 3);
        } else if (strncasecmp(token, "lib:", 4) == 0) {
            (void)snprintf(lib_out, lib_size, "%s", token + 4);
        } else {
            if (rest.len > 0) {
                (void)buf_append_str(&rest, " ");
            }
            (void)buf_append_str(&rest, token);
        }
        token = strtok_r(NULL, " \t", &saveptr);
    }
    free(copy);

    char *rest_text = buf_take(&rest);
    (void)snprintf(rest_out, rest_size, "%s", rest_text);
    free(rest_text);
}

/* qsort() comparator for g_results, keyed by g_sort_col/g_sort_desc --
 * DB_SORT_NONE is never passed to qsort() (recompute_results() skips the
 * call entirely) so every case here is an actual column. */
static int cmp_entries(const void *a, const void *b)
{
    const idx_entry *const *pa = a;
    const idx_entry *const *pb = b;
    const idx_entry *ea = *pa;
    const idx_entry *eb = *pb;
    int cmp = 0;
    switch (g_sort_col) {
        case DB_SORT_NAME:
            cmp = strcasecmp(ea->name, eb->name);
            break;
        case DB_SORT_AUTHOR:
            cmp = strcasecmp(ea->author, eb->author);
            break;
        case DB_SORT_FOR:
            cmp = strcasecmp(ea->target, eb->target);
            break;
        case DB_SORT_RATING: {
            /* "-" (unrated) always sorts last, in either direction --
             * otherwise reversing would put every unrated entry first,
             * which reads as "best", not "no rating at all". */
            bool a_unrated = str_eq(ea->rating, "-");
            bool b_unrated = str_eq(eb->rating, "-");
            if (a_unrated != b_unrated) {
                return a_unrated ? 1 : -1;
            }
            long ra = strtol(ea->rating, NULL, 10);
            long rb = strtol(eb->rating, NULL, 10);
            cmp = (ra < rb) ? -1 : (ra > rb ? 1 : 0);
            break;
        }
        default:
            break;
    }
    return g_sort_desc ? -cmp : cmp;
}

static void recompute_results(void)
{
    free_results();
    if (!g_index_loaded) {
        return;
    }

    char in_filter[256];
    char lib_filter[16];
    char rest[256];
    extract_db_filters(g_filter, in_filter, sizeof(in_filter), lib_filter, sizeof(lib_filter), rest,
                       sizeof(rest));

    idx_query query;
    if (idx_query_parse(rest, &query) != NHP_OK) {
        return;
    }
    const idx_entry **raw = NULL;
    size_t raw_count = 0;
    nhp_err err = idx_search(&g_index, &query, &raw, &raw_count);
    idx_query_free(&query);
    if (err != NHP_OK) {
        return;
    }

    g_results = calloc(raw_count, sizeof(*g_results));
    if (g_results == NULL) {
        free(raw);
        return;
    }
    size_t n = 0;
    for (size_t i = 0; i < raw_count; i++) {
        const idx_entry *entry = raw[i];
        if (lib_filter[0] != '\0') {
            bool want_yes = strcasecmp(lib_filter, "yes") == 0 || strcasecmp(lib_filter, "y") == 0;
            bool want_no = strcasecmp(lib_filter, "no") == 0 || strcasecmp(lib_filter, "n") == 0;
            if (want_yes || want_no) {
                lib_patch lp;
                bool have = lib_find(entry->id, &lp) == NHP_OK;
                if (have) {
                    lib_patch_free(&lp);
                }
                if ((want_yes && !have) || (want_no && have)) {
                    continue;
                }
            }
        }
        if (in_filter[0] != '\0' && !id_used_by(entry->id, in_filter)) {
            continue;
        }
        g_results[n++] = entry;
    }
    free(raw);
    g_result_count = n;

    if (g_sort_col != DB_SORT_NONE && g_result_count > 1) {
        qsort(g_results, g_result_count, sizeof(*g_results), cmp_entries);
    }

    ui_list_init(&g_list);
}

/* Re-sorts/re-filters from scratch but keeps the same entry selected if
 * still present, instead of snapping to the top like a plain filter
 * change -- so toggling sort order with `s`/`S` doesn't lose your place. */
static void reapply_sort_keep_selection(int visible)
{
    const idx_entry *was_selected = (g_result_count > 0) ? g_results[g_list.selected] : NULL;
    recompute_results();
    if (was_selected == NULL) {
        return;
    }
    for (size_t i = 0; i < g_result_count; i++) {
        if (g_results[i] == was_selected) {
            g_list.selected = i;
            g_list.top = i;
            ui_list_move(&g_list, 0, g_result_count, visible);
            return;
        }
    }
}

static void reload_index(void)
{
    if (g_index_loaded) {
        idx_free(&g_index);
        g_index_loaded = false;
    }
    idx_init(&g_index);
    g_index_loaded = (idx_load(&g_index) == NHP_OK);
    recompute_results();
}

// cppcheck-suppress constParameterCallback -- must match ui_screen.enter's signature
static void enter(ui_app *app)
{
    g_have_inst_version =
        app->active_loaded && ver_parse(app->active_inst.source, &g_inst_version) == NHP_OK;
    app->text_input_active = g_editing; /* in case a previous visit left it mid-edit */
    free_instances();
    (void)inst_list(&g_instances, &g_inst_count);
    reload_index();
}

static void leave(ui_app *app)
{
    app->text_input_active = false;
    free_results();
    free_instances();
    if (g_index_loaded) {
        idx_free(&g_index);
        g_index_loaded = false;
    }
    if (g_have_inst_version) {
        ver_free(&g_inst_version);
        g_have_inst_version = false;
    }
}

/* '~' fuzzy, '=' exact, '!' conflict, '?' unknown (docs/features.md
 * section 9.4) -- computed against the active instance's version, so
 * it means nothing without one. */
static char compat_mark(const idx_entry *entry)
{
    if (!g_have_inst_version) {
        return '?';
    }
    ver_version site_ver;
    if (ver_parse(entry->target, &site_ver) != NHP_OK) {
        return '?';
    }
    char mark = ver_compat_mark(ver_compatibility(&g_inst_version, &site_ver));
    ver_free(&site_ver);
    return mark;
}

/* Color role for one compat_mark() result (docs/features.md 13.8):
 * OK/green exact, WARN/yellow fuzzy, ERROR/red conflict, dim unknown --
 * same roles the glyph itself encodes, so monochrome loses nothing. */
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

/* Downloaded column text: newest version plus a "(+)" count by default,
 * or every version comma-separated once `v` toggles g_show_all_versions
 * (13.9's F3 key). The row is clipped by width already, so no layout change is needed. */
static void format_downloaded(const idx_entry *entry, char *out, size_t out_size)
{
    lib_patch lp;
    if (lib_find(entry->id, &lp) != NHP_OK || lp.version_count == 0) {
        (void)snprintf(out, out_size, "-");
        return;
    }
    if (g_show_all_versions) {
        buf b;
        buf_init(&b);
        for (size_t i = 0; i < lp.version_count; i++) {
            if (b.len > 0) {
                (void)buf_append_str(&b, ", ");
            }
            (void)buf_append_str(&b, lp.versions[i].version);
        }
        char *text = buf_take(&b);
        (void)snprintf(out, out_size, "%s", text);
        free(text);
    } else {
        (void)snprintf(out, out_size, "%s%s", lp.versions[lp.version_count - 1].version,
                       lp.version_count > 1 ? " (+)" : "");
    }
    lib_patch_free(&lp);
}

// cppcheck-suppress constParameterCallback -- must match ui_list_draw_row_fn's signature
static void draw_row(WINDOW *win, int y, int width, size_t index, bool selected, void *user_data)
{
    const ui_app *app = user_data;
    const idx_entry *entry = g_results[index];
    char mark = compat_mark(entry);

    char *name = NULL;
    char *version = NULL;
    (void)site_split_name_version(entry->name, &name, &version);

    char downloaded[256];
    format_downloaded(entry, downloaded, sizeof(downloaded));

    buf rest;
    buf_init(&rest);
    buf_append_fmt(&rest, "  %-5s %-24.24s %-7.7s %-7.7s %-6.6s %s", entry->id,
                   name != NULL ? name : entry->name, version != NULL ? version : "-",
                   entry->target, entry->rating, downloaded);
    char *rest_text = buf_take(&rest);

    bool colorize = app != NULL && !app->monochrome && !selected;

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
    (void)mvwaddnstr(win, y, 2, rest_text, width - 1);
    if (selected) {
        (void)wattroff(win, A_REVERSE);
    }
    free(rest_text);
    free(name);
    free(version);
}

static void draw_details(ui_app *app)
{
    WINDOW *w = app->layout.details;
    if (w == NULL) {
        return;
    }
    (void)werase(w);
    ui_draw_panel(w, "Details");
    if (g_result_count == 0) {
        (void)wnoutrefresh(w);
        return;
    }
    int inner_width = getmaxx(w) - 2;
    const idx_entry *entry = g_results[g_list.selected];
    if (entry->short_description[0] != '\0') {
        (void)mvwaddnstr(w, 1, 1, entry->short_description, inner_width);
    }

    char downloaded[64] = "not downloaded";
    lib_patch lp;
    if (lib_find(entry->id, &lp) == NHP_OK) {
        if (lp.version_count > 0) {
            (void)snprintf(downloaded, sizeof(downloaded), "downloaded: %s%s",
                           lp.versions[lp.version_count - 1].version,
                           lp.version_count > 1 ? " (and older versions)" : "");
        }
        lib_patch_free(&lp);
    }

    buf line;
    buf_init(&line);
    buf_append_fmt(&line, "Author: %s  For: %s  Rating: %s  (%s)", entry->author, entry->target,
                   entry->rating, downloaded);
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
    ui_draw_panel(w, g_filter[0] != '\0' ? "Database (filtered)" : "Database");
    int inner_width = getmaxx(w) - 2;

    if (!g_index_loaded) {
        (void)mvwaddnstr(w, 1, 1, "index not downloaded yet -- press R to refresh", inner_width);
        (void)wnoutrefresh(w);
        draw_details(app);
        return;
    }

    (void)mvwaddnstr(w, 1, 1, "Mk ID    Name                     Ver     For     Rating Downloaded",
                     inner_width);
    int inner_height = getmaxy(w) - 2;
    ui_list_draw(&g_list, w, 2, inner_height - 1, inner_width, g_result_count, draw_row, app);
    (void)wnoutrefresh(w);

    draw_details(app);

    /* Search box lives in the status line, not a panel row (docs/features.md
     * 13.6) -- it takes priority over the result-count message while
     * open; Enter/Esc return the status line to normal on the next draw. */
    if (g_editing) {
        ui_set_status(app, "Search: %s_   (Enter to apply, Esc to clear)", g_edit_buf);
        return;
    }

    char state[96] = "";
    if (g_sort_col != DB_SORT_NONE) {
        static const char *names[DB_SORT_COUNT] = {"none", "name", "author", "for", "rating"};
        (void)snprintf(state, sizeof(state), ", sorted by %s %s", names[g_sort_col],
                       g_sort_desc ? "desc" : "asc");
    }
    if (g_show_all_versions) {
        size_t len = strlen(state);
        (void)snprintf(state + len, sizeof(state) - len, ", all versions shown");
    }

    if (g_action_status[0] != '\0') {
        ui_set_status(app, "%s", g_action_status);
    } else if (!g_have_inst_version) {
        ui_set_status(app, "%zu patches%s%s (no active instance -- marks need one)", g_result_count,
                      g_filter[0] != '\0' ? ", filtered" : "", state);
    } else {
        ui_set_status(app, "%zu patches%s%s, matched against NetHack %s", g_result_count,
                      g_filter[0] != '\0' ? ", filtered" : "", state, app->active_inst.source);
    }
}

/* Sets both the status line (immediate feedback) and g_action_status
 * (so it's still there on the redraw right after this call returns). */
static void set_action_status(ui_app *app, const char *fmt, ...) NHP_PRINTF(2, 3);

static void set_action_status(ui_app *app, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(g_action_status, sizeof(g_action_status), fmt, ap);
    va_end(ap);
    ui_set_status(app, "%s", g_action_status);
}

static void progress(void *user_data, size_t page, size_t total_pages)
{
    ui_app *app = user_data;
    ui_set_status(app, "refreshing index: page %zu/%zu...", page, total_pages);
    ui_flush_status(app);
}

static void do_refresh(ui_app *app)
{
    ui_set_status(app, "refreshing index...");
    ui_flush_status(app);
    nhp_err err = idx_refresh(&g_index, &app->config, progress, app);
    if (err != NHP_OK) {
        set_action_status(app, "could not refresh: %s", err_get());
        return;
    }
    g_index_loaded = true;
    recompute_results();
    set_action_status(app, "refreshed: %zu patches", g_index.count);
}

/* lib_download() fails NHP_EEXIST if the detected version is already
 * in the library, with no way to learn which -- so this fetches the
 * detail page first (7.2, cached), deletes it if present, then downloads. */
static nhp_err ensure_fresh_download(ui_app *app, const idx_entry *entry, char **out_version,
                                     bool *out_replaced)
{
    *out_version = NULL;
    *out_replaced = false;

    site_detail detail;
    memset(&detail, 0, sizeof(detail));
    nhp_err err = lib_fetch_detail(entry->id, &detail);
    if (err != NHP_OK) {
        return err;
    }

    char *name_part = NULL;
    char *version_part = NULL;
    err = site_split_name_version(detail.name, &name_part, &version_part);
    site_detail_free(&detail);
    if (err != NHP_OK) {
        free(name_part);
        free(version_part);
        return err;
    }

    lib_patch lp;
    if (lib_find(entry->id, &lp) == NHP_OK) {
        for (size_t i = 0; i < lp.version_count; i++) {
            if (str_eq(lp.versions[i].version, version_part)) {
                (void)lib_delete_version(entry->id, version_part);
                *out_replaced = true;
                break;
            }
        }
        lib_patch_free(&lp);
    }
    free(name_part);
    free(version_part);

    return lib_download(entry->id, &app->config, out_version);
}

static void do_download(ui_app *app, bool add_to_instance)
{
    if (g_result_count == 0) {
        return;
    }
    /* Checked before the download itself, not just before stack_add(), so a
     * locked-out active instance doesn't cost a pointless download first --
     * active_loaded is still checked further down for its own message. */
    if (add_to_instance && app->active_loaded && !ui_require_write_access(app)) {
        return;
    }
    const idx_entry *entry = g_results[g_list.selected];

    ui_set_status(app, "checking %s...", entry->id);
    ui_flush_status(app);

    char *version = NULL;
    bool replaced = false;
    nhp_err err = ensure_fresh_download(app, entry, &version, &replaced);
    if (err != NHP_OK) {
        set_action_status(app, "could not download %s: %s", entry->id, err_get());
        return;
    }

    if (!add_to_instance) {
        set_action_status(app, "%s %s %s", replaced ? "redownloaded" : "downloaded", entry->id,
                          version);
        free(version);
        return;
    }

    if (!app->active_loaded) {
        set_action_status(app, "downloaded %s %s (no active instance to add it to)", entry->id,
                          version);
        free(version);
        return;
    }

    lib_patch lp;
    err = lib_find(entry->id, &lp);
    if (err != NHP_OK) {
        set_action_status(app, "downloaded %s %s but could not look it up: %s", entry->id, version,
                          err_get());
        free(version);
        return;
    }

    char *diff_path = NULL;
    char *sha256 = NULL;
    for (size_t i = 0; i < lp.version_count; i++) {
        if (str_eq(lp.versions[i].version, version)) {
            sha256 = str_dup(lp.versions[i].sha256);
            break;
        }
    }
    if (sha256 == NULL) {
        set_action_status(app, "downloaded %s %s but could not stage it for the instance",
                          entry->id, version);
        free(version);
        lib_patch_free(&lp);
        return;
    }
    err = lib_version_diff_path(entry->id, version, &diff_path);
    if (err != NHP_OK) {
        set_action_status(app, "downloaded %s %s but could not stage it for the instance",
                          entry->id, version);
        free(version);
        free(sha256);
        lib_patch_free(&lp);
        return;
    }

    char *name = NULL;
    char *name_version = NULL;
    (void)site_split_name_version(entry->name, &name, &name_version);
    free(name_version);

    size_t new_index = 0;
    stack_conflict conflict;
    err = stack_add(&app->active_inst, entry->id, version, name != NULL ? name : entry->name,
                    diff_path, sha256, true, &new_index, &conflict);
    if (err == NHP_OK) {
        set_action_status(app, "added %s %s to %s", entry->id, version, app->active_inst.name);
    } else if (err == NHP_ECONFLICT) {
        ui_show_conflict(app, &conflict);
        set_action_status(app, "added %s %s to %s, disabled (conflicts with %s %s)", entry->id,
                          version, app->active_inst.name, conflict.patch_id,
                          conflict.patch_version);
        stack_conflict_free(&conflict);
    } else {
        set_action_status(app, "could not add %s %s: %s", entry->id, version, err_get());
    }

    free(name);
    free(diff_path);
    free(sha256);
    free(version);
    lib_patch_free(&lp);
}

/* Removes every downloaded version of the patch from the library
 * (docs/features.md section 8) -- reverse of "download". Doesn't touch
 * any instance that already added it; each keeps its own diff copy. */
static void do_uninstall(ui_app *app)
{
    if (g_result_count == 0) {
        return;
    }
    const idx_entry *entry = g_results[g_list.selected];

    lib_patch lp;
    if (lib_find(entry->id, &lp) != NHP_OK) {
        set_action_status(app, "%s isn't downloaded", entry->id);
        return;
    }
    lib_patch_free(&lp);

    nhp_err err = lib_delete_patch(entry->id);
    if (err == NHP_OK) {
        set_action_status(app, "uninstalled %s (removed from the library)", entry->id);
    } else {
        set_action_status(app, "could not uninstall %s: %s", entry->id, err_get());
    }
}

static bool handle_key(ui_app *app, wint_t ch)
{
    WINDOW *w = app->layout.main_win;
    int visible = (w != NULL) ? getmaxy(w) - 3 : 1;

    if (g_editing) {
        if (ch == L'\n' || ch == KEY_ENTER) {
            (void)snprintf(g_filter, sizeof(g_filter), "%s", g_edit_buf);
            g_editing = false;
            app->text_input_active = false;
            recompute_results();
            return true;
        }
        if (ch == 27) { /* Esc: clear the filter entirely */
            g_filter[0] = '\0';
            g_edit_buf[0] = '\0';
            g_editing = false;
            app->text_input_active = false;
            recompute_results();
            return true;
        }
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            size_t len = strlen(g_edit_buf);
            if (len > 0) {
                g_edit_buf[len - 1] = '\0';
            }
            (void)snprintf(g_filter, sizeof(g_filter), "%s", g_edit_buf);
            recompute_results();
            return true;
        }
        if (ch >= 0x20 && ch < 0x7f) {
            size_t len = strlen(g_edit_buf);
            if (len + 1 < sizeof(g_edit_buf)) {
                g_edit_buf[len] = (char)ch;
                g_edit_buf[len + 1] = '\0';
            }
            (void)snprintf(g_filter, sizeof(g_filter), "%s", g_edit_buf);
            recompute_results();
            return true;
        }
        return true; /* editing: swallow everything else too */
    }

    if (ch == L'/') {
        (void)snprintf(g_edit_buf, sizeof(g_edit_buf), "%s", g_filter);
        g_editing = true;
        app->text_input_active = true;
        return true;
    }

    if (ch == KEY_UP || ch == L'k') {
        ui_list_move(&g_list, -1, g_result_count, visible);
        return true;
    }
    if (ch == KEY_DOWN || ch == L'j') {
        ui_list_move(&g_list, 1, g_result_count, visible);
        return true;
    }
    if (ch == KEY_PPAGE) {
        ui_list_move(&g_list, -visible, g_result_count, visible);
        return true;
    }
    if (ch == KEY_NPAGE) {
        ui_list_move(&g_list, visible, g_result_count, visible);
        return true;
    }
    if (ch == KEY_HOME) {
        ui_list_move(&g_list, -(int)g_result_count, g_result_count, visible);
        return true;
    }
    if (ch == KEY_END) {
        ui_list_move(&g_list, (int)g_result_count, g_result_count, visible);
        return true;
    }
    if (ch == L'n') { /* next match -- the list is already filtered, so this is j */
        ui_list_move(&g_list, 1, g_result_count, visible);
        return true;
    }
    if (ch == L'N') { /* previous match */
        ui_list_move(&g_list, -1, g_result_count, visible);
        return true;
    }
    if (ch == L's') {
        g_sort_col = (db_sort_col)((g_sort_col + 1) % DB_SORT_COUNT);
        reapply_sort_keep_selection(visible);
        return true;
    }
    if (ch == L'S') {
        g_sort_desc = !g_sort_desc;
        reapply_sort_keep_selection(visible);
        return true;
    }
    if (ch == L'v') {
        g_show_all_versions = !g_show_all_versions;
        return true;
    }
    if (ch == L'R') {
        do_refresh(app);
        return true;
    }
    if (ch == L'd' || ch == L'\n' || ch == KEY_ENTER) {
        do_download(app, false);
        return true;
    }
    if (ch == L'D') {
        do_download(app, true);
        return true;
    }
    if (ch == L'u') {
        do_uninstall(app);
        return true;
    }
    return false;
}

const ui_screen scr_db = {
    .name = "Database",
    .nav_letter = 'd',
    .key_bar = "Tab/Left menu  / search  n/N next/prev  s/S sort  v All versions  Enter/d Download "
               " D Download+add  u Uninstall  R Refresh",
    .enter = enter,
    .leave = leave,
    .draw = draw,
    .handle_key = handle_key,
};
