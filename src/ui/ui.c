/* ui.c - ncurses setup/teardown, colors, main loop, resize, key bar.
 * SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "ui.h"

#include "build.h"
#include "screen.h"
#include "widgets.h"

#include <curses.h>
#include <locale.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>

/* The one mutable global in src/ui/ (docs/style.md 6.3's ncurses carve-out):
 * signal handlers below need to reach ncurses state without threading it
 * through. Zeroed before ui_startup() runs. */
static ui_app g_app;
static bool g_curses_active = false;

/* Debug activity log for `--debug`, separate from util.c's lower-traffic
 * nhpm.log: one line per ui_set_status() call. Path is reported only after
 * ui_run() exits, since curses owns the terminal while it's running. */
static FILE *g_debug_log = NULL;
static char *g_debug_log_path = NULL;

static void open_debug_log(void)
{
    char *cache_dir = cfg_xdg_cache_dir();
    if (cache_dir == NULL) {
        return;
    }
    if (fs_mkdir_p(cache_dir, 0700) != NHP_OK) {
        free(cache_dir);
        return;
    }

    time_t now = time(NULL);
    struct tm tm_local;
    (void)localtime_r(&now, &tm_local);
    char stamp[32];
    (void)strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm_local);

    char filename[64];
    (void)snprintf(filename, sizeof(filename), "nhpm-debug-%s.log", stamp);
    char *path = path_join(cache_dir, filename);
    free(cache_dir);
    if (path == NULL) {
        return;
    }

    g_debug_log = fopen(path, "w");
    if (g_debug_log == NULL) {
        free(path);
        return;
    }
    g_debug_log_path = path;
    (void)fprintf(g_debug_log, "nhpm %s debug log, started %s\n", NHPM_VERSION, stamp);
    (void)fflush(g_debug_log);
}

static void close_debug_log(void)
{
    if (g_debug_log != NULL) {
        (void)fclose(g_debug_log);
        g_debug_log = NULL;
    }
    if (g_debug_log_path != NULL) {
        (void)fprintf(stderr, "debug log: %s\n", g_debug_log_path);
        free(g_debug_log_path);
        g_debug_log_path = NULL;
    }
}

/* Screens in sidebar order: Instances first (the normal home screen,
 * docs/features.md 13.9), Help last. Nav letters only act while the
 * sidebar has focus, so they never shadow a screen's own letter action. */
static const ui_screen *const g_screens[] = {
    &scr_instances, &scr_db, &scr_library, &scr_patches, &scr_build, &scr_sources, &scr_help,
};

/* current_screen() is defined near switch_to() below; declared here so
 * ui_check_drift()'s redraw callback can use it before
 * ui_reload_active_instance() (which calls ui_check_drift()) is reached. */
static const ui_screen *current_screen(const ui_app *app);
static void ui_check_drift(ui_app *app);

#define SIDEBAR_WIDTH 18

static void end_curses(void)
{
    if (g_curses_active) {
        (void)endwin();
        g_curses_active = false;
    }
}

static void handle_fatal_signal(int sig)
{
    end_curses();
    /* re-raise with the default handler so the process actually exits
     * with the expected signal/status, rather than looping back here. */
    signal(sig, SIG_DFL); // NOLINT(cert-sig31-c,cert-sig34-c) -- plain reset, no shared state
    raise(sig);
}

void ui_set_status(ui_app *app, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    buf b;
    buf_init(&b);
    char tmp[512];
    (void)vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    buf_append_str(&b, tmp);

    free(app->status_message);
    app->status_message = buf_take(&b);

    if (g_debug_log != NULL && app->status_message != NULL && app->status_message[0] != '\0') {
        time_t now = time(NULL);
        struct tm tm_local;
        (void)localtime_r(&now, &tm_local);
        char ts[16];
        (void)strftime(ts, sizeof(ts), "%H:%M:%S", &tm_local);
        (void)fprintf(g_debug_log, "[%s] %s\n", ts, app->status_message);
        (void)fflush(g_debug_log);
    }
}

/* docs/features.md 10.4: called only on an actual instance switch, not a
 * same-instance refresh. Declining both Adopt and Restore leaves the
 * folder and instance.conf as found -- 10.4's "look before deciding". */
static void ui_check_drift(ui_app *app)
{
    inst_check_result check;
    if (inst_check(&app->active_inst, &check) != NHP_OK || !check.source_path_exists) {
        return;
    }
    stack_drift drift;
    if (stack_check_consistency(&app->active_inst, &drift) != NHP_OK) {
        return;
    }
    if (drift.ok) {
        stack_drift_free(&drift);
        return;
    }

    char title[160];
    (void)snprintf(title, sizeof(title), "%s has drifted from instance.conf", app->active_name);
    ui_show_text_dialog(app, title, drift.detail[0] != '\0' ? drift.detail : "(no further detail)");
    stack_drift_free(&drift);

    void (*redraw)(ui_app *) = current_screen(app)->draw;

    if (ui_confirm(app, "Adopt the folder's state into instance.conf?", redraw)) {
        nhp_err err = stack_adopt(&app->active_inst);
        if (err == NHP_OK) {
            ui_set_status(app, "adopted %s's folder state into instance.conf", app->active_name);
        } else {
            ui_set_status(app, "could not adopt: %s", err_get());
        }
        return;
    }
    if (ui_confirm(app, "Restore the folder to match instance.conf instead?", redraw)) {
        stack_conflict conflict;
        nhp_err err = stack_restore(&app->active_inst, &conflict);
        if (err == NHP_OK) {
            ui_set_status(app, "restored %s's folder to match instance.conf", app->active_name);
        } else if (err == NHP_ECONFLICT) {
            ui_show_conflict(app, &conflict);
            stack_conflict_free(&conflict);
        } else {
            ui_set_status(app, "could not restore: %s", err_get());
        }
        return;
    }
    ui_set_status(app, "%s left as drifted -- switch away and back to decide later",
                  app->active_name);
}

void ui_reload_active_instance(ui_app *app)
{
    char *name = cfg_get_active_instance();
    const char *lock_target = (name != NULL) ? name : "";

    /* Only touch the lock on an actual instance change: re-acquiring on every
     * same-instance refresh would thrash flock() and open a release/re-
     * acquire race for a concurrent CLI command (section 10.8). */
    bool switched = !str_eq(app->active_lock_name, lock_target);
    if (switched) {
        if (app->active_lock_held) {
            inst_lock_release(&app->active_lock);
            app->active_lock_held = false;
        }
        free(app->active_lock_name);
        app->active_lock_name = str_dup(lock_target);
        app->active_read_only = false;
        app->active_lock_other_pid = 0;

        if (lock_target[0] != '\0') {
            pid_t other_pid = 0;
            nhp_err lock_err = inst_lock_acquire(lock_target, &app->active_lock, &other_pid);
            if (lock_err == NHP_OK) {
                app->active_lock_held = true;
            } else if (lock_err == NHP_ELOCKED) {
                app->active_read_only = true;
                app->active_lock_other_pid = other_pid;
                ui_set_status(app, "%s is open in another nhpm (pid %d) -- read-only", lock_target,
                              (int)other_pid);
            }
            /* Any other inst_lock_acquire() error: proceed unlocked, same
             * as not having tried -- a later mutating action's own error
             * is the only thing more nhpm could usefully report. */
        }
    }

    if (name == NULL) {
        if (app->active_loaded) {
            inst_free(&app->active_inst);
            app->active_loaded = false;
        }
        free(app->active_name);
        app->active_name = str_dup("");
        return;
    }

    if (app->active_loaded) {
        inst_free(&app->active_inst);
        app->active_loaded = false;
    }
    nhp_err err = inst_load(name, &app->active_inst);
    if (err == NHP_OK) {
        app->active_loaded = true;
        if (switched && !app->active_read_only) {
            ui_check_drift(app);
        }
    }
    free(app->active_name);
    app->active_name = name;
}

bool ui_require_write_access(ui_app *app)
{
    if (!app->active_loaded) {
        return false;
    }
    if (app->active_read_only) {
        ui_set_status(app, "%s is open in another nhpm (pid %d) -- read-only", app->active_name,
                      (int)app->active_lock_other_pid);
        return false;
    }
    return true;
}

void ui_show_conflict(ui_app *app, const stack_conflict *conflict)
{
    buf detail;
    buf_init(&detail);
    buf_append_fmt(
        &detail, "%s %s does not apply cleanly%s\n", conflict->patch_id, conflict->patch_version,
        conflict->is_requested_patch ? "" : " (a later patch in the stack depends on it)");
    for (size_t i = 0; i < conflict->file_count; i++) {
        buf_append_fmt(&detail, "  %-30s hunk %d of %d FAILED\n", conflict->files[i].path,
                       conflict->files[i].hunks_failed, conflict->files[i].hunks_total);
    }
    if (conflict->quilt_output != NULL && conflict->quilt_output[0] != '\0') {
        buf_append_str(&detail, "\n--- quilt output ---\n");
        buf_append_str(&detail, conflict->quilt_output);
    }
    char *text = buf_take(&detail);
    if (text != NULL) {
        ui_show_text_dialog(app, "Conflict", text);
        free(text);
    }
}

/* Role-based color pairs (docs/features.md section 13.8); monochrome
 * terminals (no color support, or NO_COLOR set) fall back to
 * bold/reverse so status is never color-only (section 13.11). */
static void init_theme(ui_app *app)
{
    const char *no_color = getenv("NO_COLOR");
    app->monochrome = !has_colors() || (no_color != NULL && no_color[0] != '\0');
    if (app->monochrome) {
        return;
    }

    (void)start_color();
    (void)use_default_colors();
    (void)init_pair(UI_COLOR_HEADER, COLOR_BLACK, COLOR_CYAN);
    (void)init_pair(UI_COLOR_SELECTED, COLOR_BLACK, COLOR_CYAN);
    (void)init_pair(UI_COLOR_OK, COLOR_GREEN, -1);
    (void)init_pair(UI_COLOR_WARN, COLOR_YELLOW, -1);
    (void)init_pair(UI_COLOR_ERROR, COLOR_RED, -1);
    (void)init_pair(UI_COLOR_DIM, COLOR_WHITE, -1); /* dimmed with A_DIM, not a separate color */
}

static nhp_err ui_startup(void)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        err_set("not running in a terminal");
        return NHP_EIO;
    }

    (void)setlocale(LC_ALL, "");
    if (initscr() == NULL) {
        err_set("could not initialize the terminal (check $TERM)");
        return NHP_EIO;
    }
    g_curses_active = true;
    (void)atexit(end_curses);
    (void)signal(SIGINT, handle_fatal_signal);
    (void)signal(SIGTERM, handle_fatal_signal);

    (void)cbreak();
    (void)noecho();
    (void)keypad(stdscr, TRUE);
    (void)curs_set(0);
    set_escdelay(25);
    /* No mousemask() call: it would take click-drag away from the terminal's
     * text selection, and nothing reads KEY_MOUSE yet (docs/ui-progress.md).
     * Revisit once click handling is built. */

    /* Primes curscr's idea of "what's on screen" right after initscr()
     * -- without this, some terminals don't show anything until the
     * first doupdate() that follows a key press. */
    (void)refresh();

    init_theme(&g_app);
    return NHP_OK;
}

static void delete_layout_windows(ui_layout *layout)
{
    if (layout->header != NULL) {
        delwin(layout->header);
    }
    if (layout->sidebar != NULL) {
        delwin(layout->sidebar);
    }
    if (layout->main_win != NULL) {
        delwin(layout->main_win);
    }
    if (layout->details != NULL) {
        delwin(layout->details);
    }
    if (layout->status != NULL) {
        delwin(layout->status);
    }
    if (layout->keybar != NULL) {
        delwin(layout->keybar);
    }
    memset(layout, 0, sizeof(*layout));
}

/* docs/features.md section 13.3's fixed layout: header (1), main (rest),
 * details (6), status (1), key bar (1); section 13.11's 80x24 minimum. */
static void relayout(ui_app *app)
{
    delete_layout_windows(&app->layout);
    app->layout.rows = LINES;
    app->layout.cols = COLS;

    if (app->layout.rows < 24 || app->layout.cols < 80) {
        app->layout.too_small = true;
        return;
    }
    app->layout.too_small = false;

    const int chrome_lines = 1 + 6 + 1 + 1; /* header + details + status + keybar */
    int main_lines = app->layout.rows - chrome_lines;
    int main_width = app->layout.cols - SIDEBAR_WIDTH;
    int row = 0;
    app->layout.header = newwin(1, app->layout.cols, row, 0);
    row += 1;
    app->layout.sidebar = newwin(main_lines, SIDEBAR_WIDTH, row, 0);
    app->layout.main_win = newwin(main_lines, main_width, row, SIDEBAR_WIDTH);
    row += main_lines;
    app->layout.details = newwin(6, app->layout.cols, row, 0);
    row += 6;
    app->layout.status = newwin(1, app->layout.cols, row, 0);
    row += 1;
    app->layout.keybar = newwin(1, app->layout.cols, row, 0);
}

static void draw_header(ui_app *app, const ui_screen *screen)
{
    WINDOW *w = app->layout.header;
    if (w == NULL) {
        return;
    }
    (void)werase(w);
    if (!app->monochrome) {
        (void)wbkgd(w, (chtype)COLOR_PAIR(UI_COLOR_HEADER));
    } else {
        (void)wattron(w, A_REVERSE);
    }

    buf b;
    buf_init(&b);
    buf_append_fmt(&b, " nhpm 0.1 - %s", screen->name);
    if (app->active_loaded) {
        buf_append_fmt(&b, " - %s (NetHack %s)", app->active_inst.name, app->active_inst.source);
    }
    char *text = buf_take(&b);
    (void)mvwaddnstr(w, 0, 0, text, app->layout.cols);
    free(text);

    if (app->monochrome) {
        (void)wattroff(w, A_REVERSE);
    }
    (void)wnoutrefresh(w);
}

static void draw_status(ui_app *app)
{
    WINDOW *w = app->layout.status;
    if (w == NULL) {
        return;
    }
    (void)werase(w);
    if (app->status_message != NULL && app->status_message[0] != '\0') {
        (void)mvwaddnstr(w, 0, 0, app->status_message, app->layout.cols);
    }
    (void)wnoutrefresh(w);
}

void ui_flush_status(ui_app *app)
{
    draw_status(app);
    (void)doupdate();
    (void)fflush(stdout);
}

static void draw_keybar(ui_app *app, const ui_screen *screen)
{
    WINDOW *w = app->layout.keybar;
    if (w == NULL) {
        return;
    }
    (void)werase(w);
    if (!app->monochrome) {
        (void)wbkgd(w, (chtype)COLOR_PAIR(UI_COLOR_HEADER));
    } else {
        (void)wattron(w, A_REVERSE);
    }
    (void)mvwaddnstr(w, 0, 0, screen->key_bar, app->layout.cols);
    if (app->monochrome) {
        (void)wattroff(w, A_REVERSE);
    }
    (void)wnoutrefresh(w);
}

static void draw_sidebar(ui_app *app)
{
    WINDOW *w = app->layout.sidebar;
    if (w == NULL) {
        return;
    }
    (void)werase(w);
    ui_draw_panel(w, "Menu");
    int inner_width = getmaxx(w) - 2;
    size_t count = sizeof(g_screens) / sizeof(g_screens[0]);
    for (size_t i = 0; i < count && (int)i + 1 < getmaxy(w) - 1; i++) {
        const ui_screen *s = g_screens[i];
        bool is_active_screen = (i == (size_t)app->screen_index);
        bool is_cursor = (app->focus == UI_FOCUS_SIDEBAR && i == app->sidebar_cursor);

        char line[32];
        (void)snprintf(line, sizeof(line), "%s[%c] %s", is_active_screen ? "*" : " ", s->nav_letter,
                       s->name);

        if (is_cursor) {
            (void)wattron(w, A_REVERSE);
        } else if (is_active_screen) {
            (void)wattron(w, A_BOLD);
        }
        (void)mvwaddnstr(w, (int)i + 1, 1, line, inner_width);
        if (is_cursor) {
            (void)wattroff(w, A_REVERSE);
        } else if (is_active_screen) {
            (void)wattroff(w, A_BOLD);
        }
    }
    (void)wnoutrefresh(w);
}

static void draw_too_small(ui_app *app)
{
    (void)erase();
    const char *msg = "terminal too small (need at least 80x24)";
    int row = app->layout.rows / 2;
    int col = (app->layout.cols - (int)strlen(msg)) / 2;
    if (col < 0) {
        col = 0;
    }
    (void)mvaddnstr(row, col, msg, app->layout.cols);
    (void)wnoutrefresh(stdscr);
}

static const ui_screen *current_screen(const ui_app *app)
{
    return g_screens[app->screen_index];
}

static void switch_to(ui_app *app, size_t index)
{
    if (index == (size_t)app->screen_index) {
        return;
    }
    const ui_screen *old = current_screen(app);
    if (old->leave != NULL) {
        old->leave(app);
    }
    app->screen_index = (int)index;
    const ui_screen *next = g_screens[index];
    if (next->enter != NULL) {
        next->enter(app);
    }
}

/* Hands the terminal to another program and takes it back afterward
 * (docs/features.md section 13.7) -- used for Play ('p'); the pager and
 * $EDITOR will reuse this once the screens that need them exist. */
static void suspend_and_run_play(ui_app *app)
{
    if (!app->active_loaded) {
        ui_set_status(app, "no active instance to play");
        return;
    }
    if (!ui_require_write_access(app)) {
        return;
    }
    def_prog_mode();
    (void)endwin();
    nhp_err err = build_play(&app->active_inst);
    (void)reset_prog_mode();
    (void)clearok(curscr, TRUE);
    (void)refresh();
    if (err != NHP_OK) {
        ui_set_status(app, "could not play: %s", err_get());
    }
}

/* F-keys and quit, checked before the current screen's own handle_key()
 * (docs/features.md section 13.4). Returns true if `ch` was one of
 * these. */
static size_t screen_count(void)
{
    return sizeof(g_screens) / sizeof(g_screens[0]);
}

/* Sidebar navigation: arrows+Enter or a direct nav letter. Either path
 * returns focus to the content pane. */
static bool handle_sidebar_key(ui_app *app, wint_t ch)
{
    size_t count = screen_count();

    if (ch == KEY_UP || ch == L'k') {
        if (app->sidebar_cursor > 0) {
            app->sidebar_cursor--;
        }
        return true;
    }
    if (ch == KEY_DOWN || ch == L'j') {
        if (app->sidebar_cursor + 1 < count) {
            app->sidebar_cursor++;
        }
        return true;
    }
    if (ch == L'\n' || ch == KEY_ENTER || ch == KEY_RIGHT || ch == L'\t') {
        if (ch != KEY_RIGHT && ch != L'\t') {
            switch_to(app, app->sidebar_cursor);
        }
        app->focus = UI_FOCUS_CONTENT;
        return true;
    }
    if (ch == 27) { /* Esc: back to content without switching */
        app->focus = UI_FOCUS_CONTENT;
        return true;
    }
    for (size_t i = 0; i < count; i++) {
        if ((wchar_t)ch == (wchar_t)g_screens[i]->nav_letter) {
            switch_to(app, i);
            app->focus = UI_FOCUS_CONTENT;
            return true;
        }
    }
    return true; /* sidebar has focus: swallow everything else too */
}

static bool handle_global_key(ui_app *app, wint_t ch)
{
    /* A screen's own text box (e.g. Database's "/" search) owns every key
     * while active, so 'q' and Tab/Left don't hijack the edit before the
     * screen's handle_key() can commit (Enter) or clear (Esc) it. */
    if (app->text_input_active) {
        return false;
    }

    /* Checked before the sidebar dispatch below, which otherwise swallows
     * 'q' as an unmatched key once Tab/Left focused the sidebar. No F10
     * alternative: function keys aren't reliable across terminals. */
    if (ch == L'q') {
        app->quit_requested = true;
        return true;
    }

    if (app->focus == UI_FOCUS_SIDEBAR) {
        return handle_sidebar_key(app, ch);
    }

    if (ch == KEY_LEFT || ch == L'\t') {
        app->focus = UI_FOCUS_SIDEBAR;
        app->sidebar_cursor = (size_t)app->screen_index;
        return true;
    }
    if (ch == L'?') {
        for (size_t i = 0; i < screen_count(); i++) {
            if (g_screens[i] == &scr_help) {
                switch_to(app, i);
                return true;
            }
        }
        return true;
    }
    if (ch == L'p') {
        suspend_and_run_play(app);
        return true;
    }
    return false;
}

nhp_err ui_run(bool debug)
{
    if (debug) {
        open_debug_log();
    }

    memset(&g_app, 0, sizeof(g_app));
    cfg_app_defaults(&g_app.config);
    (void)cfg_app_load(&g_app.config);
    g_app.active_name = str_dup("");

    nhp_err err = ui_startup();
    if (err != NHP_OK) {
        cfg_app_free(&g_app.config);
        free(g_app.active_name);
        close_debug_log();
        return err;
    }

    ui_reload_active_instance(&g_app);
    relayout(&g_app);
    g_app.screen_index = 0;
    const ui_screen *screen = current_screen(&g_app);
    if (screen->enter != NULL) {
        screen->enter(&g_app);
    }

    while (!g_app.quit_requested) {
        screen = current_screen(&g_app);
        if (g_app.layout.too_small) {
            draw_too_small(&g_app);
        } else {
            draw_header(&g_app, screen);
            draw_sidebar(&g_app);
            if (screen->draw != NULL) {
                screen->draw(&g_app);
            }
            draw_status(&g_app);
            draw_keybar(&g_app, screen);
        }
        (void)doupdate();
        /* Belt-and-suspenders: some terminals/ncurses builds don't
         * actually flush doupdate()'s output until the next read, which
         * looks exactly like "nothing shows until a key is pressed". */
        (void)fflush(stdout);

        wint_t ch = 0;
        int r = wget_wch(stdscr, &ch);
        if (r == KEY_CODE_YES && ch == KEY_RESIZE) {
            relayout(&g_app);
            continue;
        }
        if (r == KEY_CODE_YES && ch == KEY_MOUSE) {
            MEVENT event;
            (void)getmouse(&event);
            continue;
        }
        if (handle_global_key(&g_app, ch)) {
            continue;
        }
        screen = current_screen(&g_app);
        if (screen->handle_key != NULL) {
            (void)screen->handle_key(&g_app, ch);
        }
    }

    const ui_screen *final_screen = current_screen(&g_app);
    if (final_screen->leave != NULL) {
        final_screen->leave(&g_app);
    }
    delete_layout_windows(&g_app.layout);
    if (g_app.active_loaded) {
        inst_free(&g_app.active_inst);
    }
    if (g_app.active_lock_held) {
        inst_lock_release(&g_app.active_lock);
    }
    free(g_app.active_lock_name);
    free(g_app.active_name);
    free(g_app.status_message);
    cfg_app_free(&g_app.config);
    end_curses();
    close_debug_log();
    return NHP_OK;
}
