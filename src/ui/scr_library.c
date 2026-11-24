/* scr_library.c - F4 Library screen: browse every downloaded patch and its
 * versions, independent of any instance (docs/features.md section 8).
 * SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "screen.h"

#include "index.h"
#include "instance.h"
#include "library.h"
#include "mkpatch.h"
#include "site.h"
#include "stack.h"
#include "version.h"
#include "widgets.h"

#include <ctype.h>
#include <curses.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Loaded on enter(), freed on leave() -- mirrors scr_db.c's own shape. */
static lib_patch *g_patches = NULL;
static size_t g_patch_count = 0;

/* Every instance, used only to fill the "Used in" column and the `in:`
 * search token -- instance.c owns instance.conf itself (docs/repo-
 * structure.md); this just reads the already-loaded patch lists. */
static inst_instance *g_instances = NULL;
static size_t g_inst_count = 0;

/* One row per downloaded version, flattened from g_patches so the list
 * widget can scroll plain rows (section 13.9's F4 mockup: a patch's
 * later versions repeat under it with a blank Name column). */
typedef struct lib_row {
    size_t patch_idx;
    size_t version_idx;
} lib_row;

static lib_row *g_rows = NULL;
static size_t g_row_count = 0;
static ui_list g_list;

/* nhpm's search syntax (docs/features.md 13.10), persisted like
 * scr_db.c's g_filter. `in:` is pulled out before idx_query_parse();
 * do_search() below recomputes the rows live on every keystroke. */
static char g_filter[256] = "";

/* Section 13.9's "s/S next sort column/reverse". Sorting reorders whole
 * patches, not the flattened rows, so a patch's versions stay nested
 * (recompute_rows()'s `order` array). NONE keeps the on-disk order. */
typedef enum lib_sort_col {
    LIB_SORT_NONE = 0,
    LIB_SORT_NAME,
    LIB_SORT_VERSION,
    LIB_SORT_FOR,
    LIB_SORT_COUNT
} lib_sort_col;

static lib_sort_col g_sort_col = LIB_SORT_NONE;
static bool g_sort_desc = false;

/* The active instance's version, parsed once on enter() -- same idea as
 * scr_db.c's own g_inst_version, just comparing a lib_patch's target
 * instead of an idx_entry's. */
static ver_version g_inst_version;
static bool g_have_inst_version = false;

/* Last do_view()/do_add()/do_delete_*()/do_import()/do_check_update()
 * result, kept so it survives the redraw right after -- draw() used to
 * clobber it with the generic "%zu versions shown" line one redraw later. */
static char g_action_status[256] = "";

static void free_rows(void)
{
    free(g_rows);
    g_rows = NULL;
    g_row_count = 0;
}

static void free_lists(void)
{
    free_rows();
    if (g_patches != NULL) {
        lib_list_free(g_patches, g_patch_count);
        g_patches = NULL;
        g_patch_count = 0;
    }
    if (g_instances != NULL) {
        inst_list_free(g_instances, g_inst_count);
        g_instances = NULL;
        g_inst_count = 0;
    }
}

/* The target's version, e.g. "NetHack 3.6.0" -> "3.6.0", for the `v:`
 * filter -- same rule as index.c's entry_matches_query(), duplicated
 * here since it filters lib_patch, not idx_entry. */
static const char *target_version_part(const char *target)
{
    const char *p = target;
    while (*p != '\0' && !isdigit((unsigned char)*p)) {
        p++;
    }
    return p;
}

static bool patch_matches_words(const lib_patch *p, const char *words)
{
    if (words == NULL || words[0] == '\0') {
        return true;
    }
    char *copy = str_dup(words);
    if (copy == NULL) {
        return false;
    }
    bool all_match = true;
    char *saveptr = NULL;
    const char *word = strtok_r(copy, " \t", &saveptr);
    while (word != NULL) {
        bool found = str_contains_ci(p->name, word) || str_contains_ci(p->author, word);
        if (!found) {
            all_match = false;
            break;
        }
        word = strtok_r(NULL, " \t", &saveptr);
    }
    free(copy);
    return all_match;
}

static bool patch_matches(const lib_patch *p, const idx_query *q)
{
    if (q->exact_id != NULL && q->exact_id[0] != '\0' && !str_eq(p->id, q->exact_id)) {
        return false;
    }
    if (q->target_prefix != NULL && q->target_prefix[0] != '\0') {
        const char *version = target_version_part(p->target);
        if (strncasecmp(version, q->target_prefix, strlen(q->target_prefix)) != 0) {
            return false;
        }
    }
    if (q->author_contains != NULL && q->author_contains[0] != '\0' &&
        !str_contains_ci(p->author, q->author_contains)) {
        return false;
    }
    return patch_matches_words(p, q->text);
}

/* True if instance `inst_name` (exact match, like `id:`) has `id`/`version`
 * in its own patch stack -- false if the instance doesn't exist either. */
static bool version_used_by(const char *id, const char *version, const char *inst_name)
{
    for (size_t i = 0; i < g_inst_count; i++) {
        const inst_instance *inst = &g_instances[i];
        if (!str_eq(inst->name, inst_name)) {
            continue;
        }
        for (size_t j = 0; j < inst->patch_count; j++) {
            if (str_eq(inst->patches[j].id, id) && str_eq(inst->patches[j].version, version)) {
                return true;
            }
        }
        return false;
    }
    return false;
}

/* Every instance using `id`/`version`, comma-separated, for the "Used in"
 * column and the details pane -- unlike version_used_by(), not filtered
 * to one instance. */
static void used_in_text(const char *id, const char *version, buf *out)
{
    for (size_t i = 0; i < g_inst_count; i++) {
        const inst_instance *inst = &g_instances[i];
        for (size_t j = 0; j < inst->patch_count; j++) {
            if (str_eq(inst->patches[j].id, id) && str_eq(inst->patches[j].version, version)) {
                if (out->len > 0) {
                    (void)buf_append_str(out, ", ");
                }
                (void)buf_append_str(out, inst->name);
                break;
            }
        }
    }
}

/* Splits `raw` into its `in:` token (last one wins, same rule
 * idx_query_parse() uses for its own single-value tokens) and everything
 * else, which still goes through idx_query_parse() unchanged. */
static void extract_in_filter(const char *raw, char *in_out, size_t in_size, char *rest_out,
                              size_t rest_size)
{
    in_out[0] = '\0';
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

/* qsort() comparator for g_patches, keyed by g_sort_col/g_sort_desc.
 * LIB_SORT_VERSION compares each patch's newest version as plain
 * strings, not semantically. LIB_SORT_NONE is never passed to qsort(). */
static int cmp_patches(const void *a, const void *b)
{
    const lib_patch *pa = a;
    const lib_patch *pb = b;
    int cmp = 0;
    switch (g_sort_col) {
        case LIB_SORT_NAME:
            cmp = strcasecmp(pa->name, pb->name);
            break;
        case LIB_SORT_VERSION: {
            const char *va =
                pa->version_count > 0 ? pa->versions[pa->version_count - 1].version : "";
            const char *vb =
                pb->version_count > 0 ? pb->versions[pb->version_count - 1].version : "";
            cmp = strcasecmp(va, vb);
            break;
        }
        case LIB_SORT_FOR:
            cmp = strcasecmp(pa->target, pb->target);
            break;
        default:
            break;
    }
    return g_sort_desc ? -cmp : cmp;
}

/* qsort() comparator over an index permutation, not g_patches itself --
 * g_patches is only refetched by reload(), so sorting it in place would
 * lose the on-disk order once LIB_SORT_NONE was picked again. */
static int cmp_order(const void *a, const void *b)
{
    const size_t *pa = a;
    const size_t *pb = b;
    return cmp_patches(&g_patches[*pa], &g_patches[*pb]);
}

static void recompute_rows(void)
{
    free_rows();

    size_t max_rows = 0;
    for (size_t i = 0; i < g_patch_count; i++) {
        max_rows += g_patches[i].version_count;
    }
    if (max_rows == 0) {
        ui_list_init(&g_list);
        return;
    }

    g_rows = calloc(max_rows, sizeof(*g_rows));
    if (g_rows == NULL) {
        return;
    }

    size_t *order = calloc(g_patch_count, sizeof(*order));
    if (order == NULL) {
        free_rows();
        return;
    }
    for (size_t i = 0; i < g_patch_count; i++) {
        order[i] = i;
    }
    if (g_sort_col != LIB_SORT_NONE && g_patch_count > 1) {
        qsort(order, g_patch_count, sizeof(*order), cmp_order);
    }

    char in_filter[256];
    char rest[256];
    extract_in_filter(g_filter, in_filter, sizeof(in_filter), rest, sizeof(rest));

    idx_query query;
    if (idx_query_parse(rest, &query) != NHP_OK) {
        free_rows();
        free(order);
        return;
    }

    size_t n = 0;
    for (size_t k = 0; k < g_patch_count; k++) {
        size_t i = order[k];
        const lib_patch *p = &g_patches[i];
        if (!patch_matches(p, &query)) {
            continue;
        }
        for (size_t v = 0; v < p->version_count; v++) {
            if (in_filter[0] != '\0' &&
                !version_used_by(p->id, p->versions[v].version, in_filter)) {
                continue;
            }
            g_rows[n].patch_idx = i;
            g_rows[n].version_idx = v;
            n++;
        }
    }
    idx_query_free(&query);
    free(order);

    g_row_count = n;
    ui_list_init(&g_list);
}

/* Mirrors scr_db.c's reapply_sort_keep_selection(): recomputes rows via
 * recompute_rows(), then relocates the selection to whatever row still
 * matches the same patch id + version, instead of snapping to the top. */
static void reapply_sort_keep_selection(int visible)
{
    char saved_id[160] = "";
    char saved_version[64] = "";
    if (g_row_count > 0) {
        const lib_row *row = &g_rows[g_list.selected];
        (void)snprintf(saved_id, sizeof(saved_id), "%s", g_patches[row->patch_idx].id);
        (void)snprintf(saved_version, sizeof(saved_version), "%s",
                       g_patches[row->patch_idx].versions[row->version_idx].version);
    }
    recompute_rows();
    if (saved_id[0] == '\0') {
        return;
    }
    for (size_t k = 0; k < g_row_count; k++) {
        const lib_row *row = &g_rows[k];
        if (str_eq(g_patches[row->patch_idx].id, saved_id) &&
            str_eq(g_patches[row->patch_idx].versions[row->version_idx].version, saved_version)) {
            g_list.selected = k;
            g_list.top = k;
            ui_list_move(&g_list, 0, g_row_count, visible);
            return;
        }
    }
}

static void reload(void)
{
    free_lists();
    (void)lib_list(&g_patches, &g_patch_count);
    (void)inst_list(&g_instances, &g_inst_count);
    recompute_rows();
}

// cppcheck-suppress constParameterCallback -- must match ui_screen.enter's signature
static void enter(ui_app *app)
{
    g_have_inst_version =
        app->active_loaded && ver_parse(app->active_inst.source, &g_inst_version) == NHP_OK;
    reload();
}

static void leave(ui_app *app)
{
    app->text_input_active = false;
    free_lists();
    if (g_have_inst_version) {
        ver_free(&g_inst_version);
        g_have_inst_version = false;
    }
}

/* '~' fuzzy, '=' exact, '!' conflict, '?' unknown (docs/features.md
 * section 9.4) -- same idea as scr_db.c's own compat_mark(), just
 * reading a lib_patch's plain target string instead of an idx_entry's. */
static char compat_mark(const char *target)
{
    if (!g_have_inst_version || target[0] == '\0') {
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
 * scr_db.c's own mark_color() (small, and each screen's own module per
 * docs/repo-structure.md). */
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

// cppcheck-suppress constParameterCallback -- must match ui_list_draw_row_fn's signature
static void draw_row(WINDOW *win, int y, int width, size_t index, bool selected, void *user_data)
{
    const ui_app *app = user_data;
    const lib_row *row = &g_rows[index];
    const lib_patch *p = &g_patches[row->patch_idx];
    const lib_version *v = &p->versions[row->version_idx];
    char mark = compat_mark(p->target);

    buf used;
    buf_init(&used);
    used_in_text(p->id, v->version, &used);
    char *used_text = buf_take(&used);

    buf line;
    buf_init(&line);
    buf_append_fmt(&line, "  %-26.26s %-9.9s %-7.7s %s", row->version_idx == 0 ? p->name : "",
                   v->version, p->target[0] != '\0' ? p->target : "-",
                   used_text[0] != '\0' ? used_text : "-");
    char *text = buf_take(&line);

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
    (void)mvwaddnstr(win, y, 2, text, width - 1);
    if (selected) {
        (void)wattroff(win, A_REVERSE);
    }
    free(text);
    free(used_text);
}

static void draw_details(ui_app *app)
{
    WINDOW *w = app->layout.details;
    if (w == NULL) {
        return;
    }
    (void)werase(w);
    ui_draw_panel(w, "Details");
    if (g_row_count == 0) {
        (void)wnoutrefresh(w);
        return;
    }
    int inner_width = getmaxx(w) - 2;
    const lib_row *row = &g_rows[g_list.selected];
    const lib_patch *p = &g_patches[row->patch_idx];
    const lib_version *v = &p->versions[row->version_idx];

    buf line1;
    buf_init(&line1);
    buf_append_fmt(&line1, "ID %s   Author: %s   For: %s", p->id,
                   p->author[0] != '\0' ? p->author : "-", p->target[0] != '\0' ? p->target : "-");
    char *text1 = buf_take(&line1);
    (void)mvwaddnstr(w, 1, 1, text1, inner_width);
    free(text1);

    buf line2;
    buf_init(&line2);
    buf_append_fmt(&line2, "Downloaded %s%s   touches: %s", v->downloaded,
                   v->via_wayback ? " (via Wayback)" : "",
                   v->files_touched[0] != '\0' ? v->files_touched : "unknown");
    char *text2 = buf_take(&line2);
    (void)mvwaddnstr(w, 2, 1, text2, inner_width);
    free(text2);

    (void)wnoutrefresh(w);
}

static void draw(ui_app *app)
{
    WINDOW *w = app->layout.main_win;
    if (w == NULL) {
        return;
    }
    (void)werase(w);
    ui_draw_panel(w, g_filter[0] != '\0' ? "Library (filtered)" : "Library");
    int inner_width = getmaxx(w) - 2;

    if (g_patch_count == 0) {
        (void)mvwaddnstr(w, 1, 1, "library is empty -- download patches from the Database screen",
                         inner_width);
        (void)wnoutrefresh(w);
        draw_details(app);
        if (g_action_status[0] != '\0') {
            ui_set_status(app, "%s", g_action_status);
        } else {
            ui_set_status(app, "0 patches");
        }
        return;
    }

    (void)mvwaddnstr(w, 1, 1, "Mk Name                       Version   For     Used in",
                     inner_width);
    int inner_height = getmaxy(w) - 2;
    ui_list_draw(&g_list, w, 2, inner_height - 1, inner_width, g_row_count, draw_row, app);
    (void)wnoutrefresh(w);

    draw_details(app);

    char state[64] = "";
    if (g_sort_col != LIB_SORT_NONE) {
        static const char *names[LIB_SORT_COUNT] = {"none", "name", "version", "for"};
        (void)snprintf(state, sizeof(state), ", sorted by %s %s", names[g_sort_col],
                       g_sort_desc ? "desc" : "asc");
    }

    if (g_action_status[0] != '\0') {
        ui_set_status(app, "%s", g_action_status);
    } else if (g_row_count == 0) {
        ui_set_status(app, "no matches (%zu patches in the library)", g_patch_count);
    } else if (!g_have_inst_version) {
        ui_set_status(app, "%zu versions shown%s%s (no active instance -- marks need one)",
                      g_row_count, g_filter[0] != '\0' ? ", filtered" : "", state);
    } else {
        ui_set_status(app, "%zu versions shown%s%s, matched against NetHack %s", g_row_count,
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

static void do_view(ui_app *app)
{
    if (g_row_count == 0) {
        return;
    }
    const lib_row *row = &g_rows[g_list.selected];
    const lib_patch *p = &g_patches[row->patch_idx];
    const lib_version *v = &p->versions[row->version_idx];
    set_action_status(app, "%s %s -- touches: %s -- from %s", p->id, v->version,
                      v->files_touched[0] != '\0' ? v->files_touched : "unknown",
                      v->url[0] != '\0' ? v->url : "(local import)");
}

static void do_add(ui_app *app)
{
    if (g_row_count == 0) {
        return;
    }
    if (!app->active_loaded) {
        set_action_status(app, "no active instance to add to");
        return;
    }
    if (!ui_require_write_access(app)) {
        return;
    }
    const lib_row *row = &g_rows[g_list.selected];
    const lib_patch *p = &g_patches[row->patch_idx];
    const lib_version *v = &p->versions[row->version_idx];

    char *diff_path = NULL;
    nhp_err err = lib_version_diff_path(p->id, v->version, &diff_path);
    if (err != NHP_OK) {
        set_action_status(app, "could not stage %s %s: %s", p->id, v->version, err_get());
        return;
    }

    size_t new_index = 0;
    stack_conflict conflict;
    err = stack_add(&app->active_inst, p->id, v->version, p->name, diff_path, v->sha256, true,
                    &new_index, &conflict);
    free(diff_path);
    if (err == NHP_OK) {
        set_action_status(app, "added %s %s to %s", p->id, v->version, app->active_inst.name);
    } else if (err == NHP_ECONFLICT) {
        ui_show_conflict(app, &conflict);
        set_action_status(app, "added %s %s to %s, disabled (conflicts with %s %s)", p->id,
                          v->version, app->active_inst.name, conflict.patch_id,
                          conflict.patch_version);
        stack_conflict_free(&conflict);
    } else if (err == NHP_EEXIST) {
        set_action_status(app, "%s is already in %s's stack -- see Patches (%s)", p->id,
                          app->active_inst.name, err_get());
    } else {
        set_action_status(app, "could not add %s %s: %s", p->id, v->version, err_get());
    }
}

static void do_delete_version(ui_app *app)
{
    if (g_row_count == 0) {
        return;
    }
    const lib_row *row = &g_rows[g_list.selected];
    char *id = str_dup(g_patches[row->patch_idx].id);
    char *version = str_dup(g_patches[row->patch_idx].versions[row->version_idx].version);

    buf used;
    buf_init(&used);
    used_in_text(id, version, &used);
    char *used_text = buf_take(&used);

    char message[300];
    if (used_text[0] != '\0') {
        (void)snprintf(message, sizeof(message),
                       "Delete %s version %s? %s already has a copy and keeps working; re-adding "
                       "it later means downloading it again.",
                       id, version, used_text);
    } else {
        (void)snprintf(message, sizeof(message), "Delete %s version %s?", id, version);
    }
    free(used_text);

    if (!ui_confirm(app, message, draw)) {
        free(id);
        free(version);
        return;
    }

    nhp_err err = lib_delete_version(id, version);
    if (err == NHP_OK) {
        set_action_status(app, "deleted %s version %s", id, version);
        reload();
        if (g_row_count > 0 && g_list.selected >= g_row_count) {
            g_list.selected = g_row_count - 1;
        }
    } else {
        set_action_status(app, "could not delete %s version %s: %s", id, version, err_get());
    }
    free(id);
    free(version);
}

static void do_delete_patch(ui_app *app)
{
    if (g_row_count == 0) {
        return;
    }
    const lib_row *row = &g_rows[g_list.selected];
    char *id = str_dup(g_patches[row->patch_idx].id);
    char *name = str_dup(g_patches[row->patch_idx].name);

    char message[300];
    (void)snprintf(message, sizeof(message),
                   "Delete %s (%s)? This removes every downloaded version.", name, id);
    if (!ui_confirm(app, message, draw)) {
        free(id);
        free(name);
        return;
    }

    nhp_err err = lib_delete_patch(id);
    if (err == NHP_OK) {
        set_action_status(app, "deleted %s (every version)", name);
        reload();
        if (g_row_count > 0 && g_list.selected >= g_row_count) {
            g_list.selected = g_row_count - 1;
        }
    } else {
        set_action_status(app, "could not delete %s: %s", name, err_get());
    }
    free(id);
    free(name);
}

static void do_import(ui_app *app)
{
    char path[512] = "";
    if (!ui_show_popup_input(app, "Import local .diff, path: ", path, sizeof(path), draw) ||
        path[0] == '\0') {
        set_action_status(app, "cancelled");
        return;
    }
    char name[256] = "";
    if (!ui_show_popup_input(app, "Name: ", name, sizeof(name), draw) || name[0] == '\0') {
        set_action_status(app, "cancelled");
        return;
    }
    char author[128] = "";
    (void)ui_show_popup_input(app, "Author (blank if unknown): ", author, sizeof(author), draw);
    char target[64] = "";
    (void)ui_show_popup_input(app, "Target NetHack version (blank for any): ", target,
                              sizeof(target), draw);
    char version[64] = "";
    (void)ui_show_popup_input(app, "Version (blank for \"1.0\"): ", version, sizeof(version), draw);

    char *id = NULL;
    nhp_err err =
        lib_import_local(path, name, author, target, version[0] != '\0' ? version : NULL, &id);
    if (err == NHP_OK) {
        set_action_status(app, "imported %s as %s", name, id);
        reload();
    } else {
        set_action_status(app, "could not import %s: %s", name, err_get());
    }
    free(id);
}

/* "Check for update" (docs/features.md 8): re-reads the cached detail
 * page and compares its current version against the newest one already
 * downloaded -- there's no version history, so that's the whole check. */
static void do_check_update(ui_app *app)
{
    if (g_row_count == 0) {
        return;
    }
    const lib_row *row = &g_rows[g_list.selected];
    const lib_patch *p = &g_patches[row->patch_idx];

    if (strncmp(p->id, "local-", 6) == 0) {
        set_action_status(app, "%s is a local patch -- no site page to check", p->id);
        return;
    }

    ui_set_status(app, "checking %s...", p->id);
    ui_flush_status(app);

    site_detail detail;
    memset(&detail, 0, sizeof(detail));
    nhp_err err = lib_fetch_detail(p->id, &detail);
    if (err != NHP_OK) {
        set_action_status(app, "could not check %s: %s", p->id, err_get());
        return;
    }

    char *name_part = NULL;
    char *version_part = NULL;
    err = site_split_name_version(detail.name, &name_part, &version_part);
    site_detail_free(&detail);
    if (err != NHP_OK || version_part == NULL) {
        set_action_status(app, "could not read %s's current version", p->id);
        free(name_part);
        free(version_part);
        return;
    }

    const char *newest = (p->version_count > 0) ? p->versions[p->version_count - 1].version : "";
    if (str_eq(newest, version_part)) {
        set_action_status(app, "%s is up to date (%s)", p->id, newest);
    } else {
        set_action_status(app,
                          "%s: the site now has version %s (you have %s) -- Enter/d on the "
                          "Database screen downloads it",
                          p->id, version_part, newest[0] != '\0' ? newest : "none");
    }
    free(name_part);
    free(version_part);
}

/* docs/features.md 12.6: a patch from any modified NetHack folder,
 * read-only (mkp_from_folder() never touches it). File ticking is now
 * scr_changes.c's job (ui_show_changes_dialog_for_folder()). */
static void do_make_from_folder(ui_app *app)
{
    char folder[512] = "";
    if (!ui_show_popup_input(app, "Folder to make a patch from: ", folder, sizeof(folder), draw) ||
        folder[0] == '\0') {
        set_action_status(app, "cancelled");
        return;
    }

    char from[256] = "base";
    if (!ui_show_popup_input(app, "Base (base, folder:<path>, tarball:<path>, or git:<ref>): ",
                             from, sizeof(from), draw)) {
        set_action_status(app, "cancelled");
        return;
    }

    mkp_base_spec base;
    if (str_eq(from, "base")) {
        base.kind = MKP_BASE_AUTO;
        base.path_or_ref = "";
    } else if (strncmp(from, "folder:", 7) == 0) {
        base.kind = MKP_BASE_FOLDER;
        base.path_or_ref = from + 7;
    } else if (strncmp(from, "tarball:", 8) == 0) {
        base.kind = MKP_BASE_TARBALL;
        base.path_or_ref = from + 8;
    } else if (strncmp(from, "git:", 4) == 0) {
        base.kind = MKP_BASE_GIT_REF;
        base.path_or_ref = from + 4;
    } else {
        set_action_status(
            app, "bad base '%s' (want base, folder:<path>, tarball:<path>, or git:<ref>)", from);
        return;
    }

    /* docs/features.md 12.6 steps 3-4: ticking and the capture form both
     * happen inside the Changes dialog now (Space, then S). Unavailable
     * for a git ref base, which is compared as raw diff text. */
    ui_show_changes_dialog_for_folder(app, folder, &base);
    /* The dialog sets the final result via plain ui_set_status(); mirrored
     * into g_action_status here so draw()'s own sticky-status logic
     * doesn't immediately clobber it back out. */
    (void)snprintf(g_action_status, sizeof(g_action_status), "%s",
                   (app->status_message != NULL) ? app->status_message : "");
    reload();
}

/* docs/features.md 13.9's "/": a floating popup box (ui_show_popup_input_live(),
 * not the status line) that re-filters live on every keystroke; Esc clears
 * the filter entirely rather than restoring whatever it was before. */
static void on_search_change(ui_app *app, const char *text)
{
    (void)app;
    (void)snprintf(g_filter, sizeof(g_filter), "%s", text);
    recompute_rows();
}

static void do_search(ui_app *app)
{
    char search_text[256];
    (void)snprintf(search_text, sizeof(search_text), "%s", g_filter);
    bool committed = ui_show_popup_input_live(app, "Search (v: a: id: in:<instance>)", search_text,
                                              sizeof(search_text), draw, on_search_change);
    if (!committed) {
        g_filter[0] = '\0';
    } else {
        (void)snprintf(g_filter, sizeof(g_filter), "%s", search_text);
    }
    recompute_rows();
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
        ui_list_move(&g_list, -1, g_row_count, visible);
        return true;
    }
    if (ch == KEY_DOWN || ch == L'j') {
        ui_list_move(&g_list, 1, g_row_count, visible);
        return true;
    }
    if (ch == KEY_PPAGE) {
        ui_list_move(&g_list, -visible, g_row_count, visible);
        return true;
    }
    if (ch == KEY_NPAGE) {
        ui_list_move(&g_list, visible, g_row_count, visible);
        return true;
    }
    if (ch == KEY_HOME) {
        ui_list_move(&g_list, -(int)g_row_count, g_row_count, visible);
        return true;
    }
    if (ch == KEY_END) {
        ui_list_move(&g_list, (int)g_row_count, g_row_count, visible);
        return true;
    }
    if (ch == L'n') { /* next match -- the list is already filtered, so this is j */
        ui_list_move(&g_list, 1, g_row_count, visible);
        return true;
    }
    if (ch == L'N') { /* previous match */
        ui_list_move(&g_list, -1, g_row_count, visible);
        return true;
    }
    if (ch == L's') {
        g_sort_col = (lib_sort_col)((g_sort_col + 1) % LIB_SORT_COUNT);
        reapply_sort_keep_selection(visible);
        return true;
    }
    if (ch == L'S') {
        g_sort_desc = !g_sort_desc;
        reapply_sort_keep_selection(visible);
        return true;
    }
    if (ch == L'\n' || ch == KEY_ENTER) {
        do_view(app);
        return true;
    }
    if (ch == L'a') {
        do_add(app);
        return true;
    }
    if (ch == L'x') {
        do_delete_version(app);
        return true;
    }
    if (ch == L'X') {
        do_delete_patch(app);
        return true;
    }
    if (ch == L'i') {
        do_import(app);
        return true;
    }
    if (ch == L'u') {
        do_check_update(app);
        return true;
    }
    if (ch == L'm') {
        do_make_from_folder(app);
        return true;
    }
    return false;
}

const ui_screen scr_library = {
    .name = "Library",
    .nav_letter = 'l',
    .key_bar = "Tab/Left menu  / search  n/N next/prev  s/S sort  Enter view  a Add  x Del version "
               " X Del patch  u Updates  i Import  m Make from folder",
    .enter = enter,
    .leave = leave,
    .draw = draw,
    .handle_key = handle_key,
};
