/* scr_build.c - F6 Build screen: streams the active instance's recipe
 * output, jumps between errors, rebuilds.
 * SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "screen.h"

#include "build.h"
#include "widgets.h"

#include <curses.h>
#include <stdlib.h>
#include <string.h>

typedef struct build_line {
    char *text;
    bool is_error;   /* contains "error:" (docs/features.md 13.9: red) */
    bool is_warning; /* contains "warning:", and isn't already an error (yellow) */
} build_line;

/* Loaded on enter() from build.log if one already exists, replaced
 * wholesale by do_build() -- mirrors scr_db.c's own "static state scoped
 * to the file" shape. */
static build_line *g_lines = NULL;
static size_t g_line_count = 0;
static size_t g_line_cap = 0;

static ui_text_view g_view;
static size_t g_error_cursor = 0; /* last line `e` jumped to, for "next" */

static bool g_building = false; /* streaming right now (inside do_build()) */
static bool g_canceled = false; /* the last build was stopped with Esc */
/* Separate from g_lines' error:/warning: heuristic -- a recipe step can
 * fail silently (no matching line), which used to let draw()'s generic
 * status message hide the failure. draw() checks this first. */
static bool g_build_failed = false;
static char g_build_error[256] = "";

static bool append_line_owned(char *text)
{
    if (text == NULL) {
        return false;
    }
    if (g_line_count == g_line_cap) {
        size_t new_cap = (g_line_cap == 0) ? 256 : g_line_cap * 2;
        build_line *grown = realloc(g_lines, new_cap * sizeof(*grown));
        if (grown == NULL) {
            free(text);
            return false;
        }
        g_lines = grown;
        g_line_cap = new_cap;
    }
    build_line *line = &g_lines[g_line_count];
    line->text = text;
    line->is_error = str_contains_ci(text, "error:");
    line->is_warning = !line->is_error && str_contains_ci(text, "warning:");
    g_line_count++;
    return true;
}

static bool append_line(const char *text)
{
    return append_line_owned(str_dup(text));
}

static void free_lines(void)
{
    for (size_t i = 0; i < g_line_count; i++) {
        free(g_lines[i].text);
    }
    free(g_lines);
    g_lines = NULL;
    g_line_count = 0;
    g_line_cap = 0;
    g_error_cursor = 0;
    ui_text_view_init(&g_view);
}

/* Loads whatever build.log is already on disk from a previous run, so
 * leaving and returning to this screen (or starting nhpm fresh) still
 * shows the last build instead of a blank panel. */
static void load_existing_log(const inst_instance *inst)
{
    char *dir = inst_dir(inst->name);
    char *log_path = (dir != NULL) ? path_join(dir, "build.log") : NULL;
    free(dir);
    if (log_path == NULL) {
        return;
    }

    char *data = NULL;
    size_t len = 0;
    if (fs_read_file(log_path, &data, &len) == NHP_OK) {
        const char *p = data;
        const char *end = data + len;
        while (p < end) {
            const char *nl = memchr(p, '\n', (size_t)(end - p));
            size_t line_len = (nl != NULL) ? (size_t)(nl - p) : (size_t)(end - p);
            (void)append_line_owned(str_ndup(p, line_len));
            p = (nl != NULL) ? nl + 1 : end;
        }
        free(data);
    }
    free(log_path);
}

// cppcheck-suppress constParameterCallback -- must match ui_screen.enter's signature
static void enter(ui_app *app)
{
    free_lines();
    if (app->active_loaded) {
        load_existing_log(&app->active_inst);
        ui_text_view_scroll_to_end(&g_view, g_line_count, 1000000);
    }
}

static void leave(ui_app *app)
{
    (void)app;
    free_lines();
}

static int visible_rows(const ui_app *app)
{
    WINDOW *w = app->layout.main_win;
    return (w != NULL) ? getmaxy(w) - 2 : 1;
}

// cppcheck-suppress constParameterCallback -- must match ui_text_view_draw_line_fn's signature
static void draw_line(WINDOW *win, int y, int width, size_t index, void *user_data)
{
    const ui_app *app = user_data;
    const build_line *line = &g_lines[index];
    bool colorize = app != NULL && !app->monochrome;
    enum ui_color_pair color = line->is_error ? UI_COLOR_ERROR : UI_COLOR_WARN;
    bool apply_color = colorize && (line->is_error || line->is_warning);

    if (apply_color) {
        (void)wattron(win, COLOR_PAIR(color));
    }
    (void)mvwaddnstr(win, y, 1, line->text, width - 1);
    if (apply_color) {
        (void)wattroff(win, COLOR_PAIR(color));
    }
}

static void draw_details(ui_app *app)
{
    WINDOW *w = app->layout.details;
    if (w == NULL) {
        return;
    }
    (void)werase(w);
    ui_draw_panel(w, "Details");
    if (!app->active_loaded) {
        (void)wnoutrefresh(w);
        return;
    }
    int inner_width = getmaxx(w) - 2;
    const inst_instance *inst = &app->active_inst;

    size_t errors = 0;
    size_t warnings = 0;
    for (size_t i = 0; i < g_line_count; i++) {
        if (g_lines[i].is_error) {
            errors++;
        } else if (g_lines[i].is_warning) {
            warnings++;
        }
    }

    buf line;
    buf_init(&line);
    buf_append_fmt(&line, "Recipe: %s   Last built: %s   %zu error%s, %zu warning%s",
                   inst->recipe[0] != '\0' ? inst->recipe : "custom",
                   inst->last_built[0] != '\0' ? inst->last_built : "never", errors,
                   errors == 1 ? "" : "s", warnings, warnings == 1 ? "" : "s");
    char *text = buf_take(&line);
    (void)mvwaddnstr(w, 1, 1, text, inner_width);
    free(text);

    if (str_eq(inst->recipe, "custom")) {
        buf cmd_line;
        buf_init(&cmd_line);
        buf_append_fmt(&cmd_line, "custom_commands: %s",
                       inst->custom_commands[0] != '\0' ? inst->custom_commands
                                                        : "(none set -- press c)");
        char *cmd_text = buf_take(&cmd_line);
        (void)mvwaddnstr(w, 2, 1, cmd_text, inner_width);
        free(cmd_text);
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
    ui_draw_panel(w, "Build");
    int inner_width = getmaxx(w) - 2;

    if (!app->active_loaded) {
        (void)mvwaddnstr(w, 1, 1, "no active instance -- pick one on the Instances screen",
                         inner_width);
        (void)wnoutrefresh(w);
        draw_details(app);
        return;
    }

    if (g_line_count == 0) {
        (void)mvwaddnstr(w, 1, 1, "no build output yet -- press b to build", inner_width);
    } else {
        ui_text_view_draw(&g_view, w, 1, visible_rows(app), inner_width, g_line_count, draw_line,
                          app);
    }
    (void)wnoutrefresh(w);

    draw_details(app);

    if (g_building) {
        ui_set_status(app, "building %s... (%zu lines; Esc to cancel)", app->active_inst.name,
                      g_line_count);
    } else if (g_canceled) {
        ui_set_status(app, "build canceled (%zu lines captured) -- b to retry", g_line_count);
    } else if (g_build_failed) {
        ui_set_status(app, "build failed: %s -- b to retry", g_build_error);
    } else if (g_line_count == 0) {
        ui_set_status(app, "press b to build %s", app->active_inst.name);
    } else if (build_is_stale(&app->active_inst)) {
        ui_set_status(app, "%zu lines -- patches changed since the last build, press b to rebuild",
                      g_line_count);
    } else {
        ui_set_status(app, "%zu lines", g_line_count);
    }
}

typedef struct build_ctx {
    ui_app *app;
    bool canceled;
} build_ctx;

/* docs/features.md 13.6: build_run() blocks the main loop, so this
 * redraws after each line to avoid a frozen screen, then polls once
 * for Esc -- returning false cancels the build (SIGTERM via proc.c). */
static bool on_build_line(void *user_data, bool is_stderr, const char *line)
{
    (void)is_stderr;
    build_ctx *ctx = user_data;
    (void)append_line(line);
    ui_text_view_scroll_to_end(&g_view, g_line_count, visible_rows(ctx->app));

    draw(ctx->app);
    ui_flush_status(ctx->app);

    (void)nodelay(stdscr, TRUE);
    wint_t ch = 0;
    int r = wget_wch(stdscr, &ch);
    (void)nodelay(stdscr, FALSE);
    if (r != ERR && (ch == 27 || ch == L'q')) {
        /* 'q' cancels like Esc but also flags quit_requested, since
         * normal key handling is blocked during the build -- otherwise
         * q wouldn't exit immediately like it does everywhere else. */
        ctx->canceled = true;
        if (ch != 27) {
            ctx->app->quit_requested = true;
        }
        return false;
    }
    return true;
}

static void do_build(ui_app *app)
{
    if (!app->active_loaded) {
        ui_set_status(app, "no active instance to build");
        return;
    }
    if (!ui_require_write_access(app)) {
        return;
    }

    free_lines();
    g_canceled = false;
    g_build_failed = false;
    g_building = true;
    draw(app);
    ui_flush_status(app);

    build_ctx ctx = {.app = app, .canceled = false};
    nhp_err err = build_run(&app->active_inst, on_build_line, &ctx);
    g_building = false;

    if (ctx.canceled) {
        g_canceled = true;
    } else if (err == NHP_OK) {
        ui_set_status(app, "build succeeded -- p on Instances to play");
    } else {
        g_build_failed = true;
        (void)snprintf(g_build_error, sizeof(g_build_error), "%s", err_get());
        ui_set_status(app, "build failed: %s", err_get());
    }
}

/* docs/features.md 10.6's custom_commands, editable here instead of only
 * by hand-editing instance.conf: a ';'-separated command list, run in
 * src/ in order, only when the recipe is "custom". */
static void do_edit_commands(ui_app *app)
{
    if (!app->active_loaded) {
        ui_set_status(app, "no active instance");
        return;
    }
    if (!ui_require_write_access(app)) {
        return;
    }

    char commands[1024];
    (void)snprintf(commands, sizeof(commands), "%s", app->active_inst.custom_commands);
    if (!ui_show_popup_input(app, "Custom commands (';'-separated, run in src/, $PREFIX allowed)",
                             commands, sizeof(commands), draw)) {
        ui_set_status(app, "cancelled");
        return;
    }

    char *new_commands = str_dup(commands);
    if (new_commands == NULL) {
        ui_set_status(app, "out of memory");
        return;
    }
    free(app->active_inst.custom_commands);
    app->active_inst.custom_commands = new_commands;

    nhp_err err = inst_save(&app->active_inst);
    if (err == NHP_OK) {
        ui_set_status(app, "saved custom_commands");
    } else {
        ui_set_status(app, "could not save custom_commands: %s", err_get());
    }
}

/* "s" copies the instance's build.log elsewhere, mirroring scr_instances.c's
 * "Save to:" pattern for the combined patch (12.2); reads the file build_run()
 * wrote rather than reserializing g_lines, so a canceled build still exports. */
static void do_export_log(ui_app *app)
{
    if (!app->active_loaded) {
        ui_set_status(app, "no active instance");
        return;
    }
    if (g_line_count == 0) {
        ui_set_status(app, "no build output yet -- press b to build first");
        return;
    }

    char *dir = inst_dir(app->active_inst.name);
    char *log_path = (dir != NULL) ? path_join(dir, "build.log") : NULL;
    free(dir);
    if (log_path == NULL) {
        ui_set_status(app, "out of memory");
        return;
    }
    char *data = NULL;
    size_t len = 0;
    nhp_err err = fs_read_file(log_path, &data, &len);
    free(log_path);
    if (err != NHP_OK) {
        ui_set_status(app, "could not read build.log: %s", err_get());
        return;
    }

    char path[512];
    (void)snprintf(path, sizeof(path), "%s-build.log", app->active_inst.name);
    if (!ui_show_popup_input(app, "Export build log to: ", path, sizeof(path), draw) ||
        path[0] == '\0') {
        ui_set_status(app, "cancelled");
        free(data);
        return;
    }

    err = fs_write_atomic(path, data, len);
    free(data);
    if (err != NHP_OK) {
        ui_set_status(app, "could not write %s: %s", path, err_get());
        return;
    }
    ui_set_status(app, "exported build log to %s", path);
}

/* "e" jumps to the next error line after the last one jumped to,
 * wrapping around to the first error once the end is reached. */
static void do_jump_to_error(ui_app *app)
{
    if (g_line_count == 0) {
        ui_set_status(app, "no build output yet");
        return;
    }

    size_t start = (g_error_cursor + 1 < g_line_count) ? g_error_cursor + 1 : 0;
    size_t found = g_line_count; /* sentinel: not found */
    for (size_t i = 0; i < g_line_count; i++) {
        size_t index = (start + i) % g_line_count;
        if (g_lines[index].is_error) {
            found = index;
            break;
        }
    }

    if (found == g_line_count) {
        ui_set_status(app, "no errors in the build output");
        return;
    }
    g_error_cursor = found;
    ui_text_view_scroll_to(&g_view, found, g_line_count, visible_rows(app));
    ui_set_status(app, "error at line %zu: %s", found + 1, g_lines[found].text);
}

static bool handle_key(ui_app *app, wint_t ch)
{
    int visible = visible_rows(app);

    if (ch == KEY_UP || ch == L'k') {
        ui_text_view_scroll(&g_view, -1, g_line_count, visible);
        return true;
    }
    if (ch == KEY_DOWN || ch == L'j') {
        ui_text_view_scroll(&g_view, 1, g_line_count, visible);
        return true;
    }
    if (ch == KEY_PPAGE) {
        ui_text_view_scroll(&g_view, -visible, g_line_count, visible);
        return true;
    }
    if (ch == KEY_NPAGE) {
        ui_text_view_scroll(&g_view, visible, g_line_count, visible);
        return true;
    }
    if (ch == KEY_HOME) {
        ui_text_view_scroll(&g_view, -(int)g_line_count, g_line_count, visible);
        return true;
    }
    if (ch == KEY_END) {
        ui_text_view_scroll_to_end(&g_view, g_line_count, visible);
        return true;
    }
    if (ch == L'b') {
        do_build(app);
        return true;
    }
    if (ch == L'e') {
        do_jump_to_error(app);
        return true;
    }
    if (ch == L'c') {
        do_edit_commands(app);
        return true;
    }
    if (ch == L's') {
        do_export_log(app);
        return true;
    }
    return false;
}

const ui_screen scr_build = {
    .name = "Build",
    .nav_letter = 'b',
    .key_bar = "Tab/Left menu  b Build/rebuild  e Next error  c Edit commands  s Export log  "
               "Esc cancel",
    .enter = enter,
    .leave = leave,
    .draw = draw,
    .handle_key = handle_key,
};
