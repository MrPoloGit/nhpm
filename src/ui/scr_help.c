/* scr_help.c - '?' help screen.
 * SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "screen.h"

#include "widgets.h"

#include <curses.h>

/* What's actually wired up today, screen by screen -- kept in sync with
 * each scr_*.c's own key_bar so this never describes a dead key. Scrolls
 * via ui_text_view, the same widget Build's streamed output uses. */
static const char *const HELP_LINES[] = {
    "Global keys",
    "  Up/Down, k/j, PgUp/PgDn, Home/End   move the selection",
    "  Mouse wheel                          scrolls the same way (no clicking yet)",
    "  Shift+drag (most terminals)          selects/copies text normally -- plain drag goes",
    "                                       to nhpm instead, since the wheel needs that on",
    "  Tab or Left arrow                   focus the sidebar (Up/Down+Enter, or a",
    "                                       letter, jumps screens; Right/Enter returns)",
    "  ?                                    this help",
    "  q                                    quit immediately, on any screen (even mid-build)",
    "",
    "F2  Instances -- create, clone, rename, delete, snapshot, export/import, play",
    "  Enter                               make the selected instance active",
    "  p                                   play the active instance (once it's built)",
    "  n                                   new instance: name, Managed (from a base",
    "                                       source) or Linked (an existing folder), recipe",
    "  c                                   clone the selected instance",
    "  r                                   rename the selected instance",
    "  x                                   delete the selected instance (asks first)",
    "  t                                   snapshots dialog: Enter restores, x deletes,",
    "                                       a adds a new one (own label), Esc closes",
    "  e                                   export to a .nhinst file",
    "  i                                   import a .nhinst file as a new instance",
    "  C                                   combined patch: save to file, and/or add to the",
    "                                       library, and/or flatten into a new instance",
    "",
    "F3  Database -- browse and search the NetHack Patch Database",
    "  /                                   open the search box (shown in the status",
    "                                       line); Enter commits it, Esc clears it;",
    "                                       v: a: r:>=N id: mk:<=/~/!/?> syntax",
    "  Enter or d                          download the selected patch (replaces an",
    "                                       already-downloaded copy with a fresh one)",
    "  D                                   download it and add it to the active",
    "                                       instance's patch stack, enabled",
    "  u                                   uninstall: remove it from the library",
    "  R                                   refresh the whole index from the site",
    "  s/S                                 sort by name/author/for/rating/id, S reverses",
    "  Mk column: = exact match  ~ fuzzy  ! conflicts  ? unknown (colored to match)",
    "",
    "F4  Library -- every downloaded patch and version, independent of any instance",
    "  /                                   search: v: a: id: in:<instance> mk:<=/~/!/?>",
    "                                       syntax",
    "  s/S                                 sort by name/version/for/id, S reverses",
    "  Enter                               view the selected version's touched files",
    "  a                                   add the selected version to the active",
    "                                       instance's patch stack, enabled",
    "  x                                   delete the selected version",
    "  X                                   delete the patch (every downloaded version)",
    "  u                                   check the site for a newer version",
    "  i                                   import a local .diff that isn't on the site",
    "  m                                   make a patch from a modified folder (base, another",
    "                                       folder, a tarball, or a git ref); prompted form",
    "  Mk column: = exact match  ~ fuzzy  ! conflicts  ? unknown (colored to match)",
    "",
    "F5  Patches -- the active instance's Quilt stack",
    "  Space                               enable / disable the selected patch",
    "  J / K                               move the selected patch down / up",
    "  d                                   show the selected patch's actual applied diff",
    "  Mk column: = exact match  ~ fuzzy  ! conflicts  ? unknown (colored to match)",
    "  w                                   Changes dialog: review uncaptured edits, Space",
    "                                       tick, Enter full diff, S save ticked as a patch",
    "  x                                   remove the selected patch",
    "  c                                   dry-run every disabled patch (fills Status column)",
    "  t                                   take a manual, labeled snapshot",
    "  u                                   undo the last Enable/Disable/Move/Add/Remove",
    "  N / E / o / R                       new / edit / open file / refresh a patch (12.3-12.4)",
    "  '*' by the column headers means uncaptured edits are sitting in src/ (w to capture)",
    "",
    "F6  Build -- streams the active instance's recipe output",
    "  b                                   build / rebuild; Esc while it's running cancels it",
    "  e                                   jump to the next error: line (wraps around)",
    "  c                                   edit instance.conf's custom_commands (recipe custom)",
    "  s                                   export build.log to a file you name",
    "  Up/Down, PgUp/PgDn, Home/End         scroll the output",
    "  error:/warning: lines colored red/yellow; play from Instances once it succeeds",
    "",
    "F7  Sources -- base NetHack sources: folder, tarball, git",
    "  a                                   add from a local folder",
    "  t                                   add from a tarball (.tar.gz/.tgz/.zip/...)",
    "  g                                   add from a git tag (clones NetHack/NetHack)",
    "  x                                   remove the selected source (asks first; an",
    "                                       instance built from it keeps its own copy)",
};
static const size_t HELP_LINE_COUNT = sizeof(HELP_LINES) / sizeof(HELP_LINES[0]);

static ui_text_view g_view;

static int visible_rows(const ui_app *app)
{
    WINDOW *w = app->layout.main_win;
    return (w != NULL) ? getmaxy(w) - 2 : 1;
}

// cppcheck-suppress constParameterCallback -- must match ui_text_view_draw_line_fn's signature
static void draw_line(WINDOW *win, int y, int width, size_t index, void *user_data)
{
    (void)user_data;
    (void)mvwaddnstr(win, y, 1, HELP_LINES[index], width - 1);
}

// cppcheck-suppress constParameterCallback -- must match ui_screen.enter's signature
static void enter(ui_app *app)
{
    (void)app;
    ui_text_view_init(&g_view); /* always from the top, same as opening fresh */
}

static void draw(ui_app *app)
{
    WINDOW *w = app->layout.main_win;
    if (w == NULL) {
        return;
    }
    (void)werase(w);
    ui_draw_panel(w, "Help");
    int inner_width = getmaxx(w) - 2;
    ui_text_view_draw(&g_view, w, 1, visible_rows(app), inner_width, HELP_LINE_COUNT, draw_line,
                      NULL);
    (void)wnoutrefresh(w);
}

static bool handle_key(ui_app *app, wint_t ch)
{
    int visible = visible_rows(app);

    if (ch == KEY_UP || ch == L'k') {
        ui_text_view_scroll(&g_view, -1, HELP_LINE_COUNT, visible);
        return true;
    }
    if (ch == KEY_DOWN || ch == L'j') {
        ui_text_view_scroll(&g_view, 1, HELP_LINE_COUNT, visible);
        return true;
    }
    if (ch == KEY_PPAGE) {
        ui_text_view_scroll(&g_view, -visible, HELP_LINE_COUNT, visible);
        return true;
    }
    if (ch == KEY_NPAGE) {
        ui_text_view_scroll(&g_view, visible, HELP_LINE_COUNT, visible);
        return true;
    }
    if (ch == KEY_HOME) {
        ui_text_view_scroll(&g_view, -(int)HELP_LINE_COUNT, HELP_LINE_COUNT, visible);
        return true;
    }
    if (ch == KEY_END) {
        ui_text_view_scroll_to_end(&g_view, HELP_LINE_COUNT, visible);
        return true;
    }
    return false;
}

const ui_screen scr_help = {
    .name = "Help",
    .nav_letter = 'h',
    .key_bar = "Tab/Left menu  q quit",
    .enter = enter,
    .leave = NULL,
    .draw = draw,
    .handle_key = handle_key,
};
