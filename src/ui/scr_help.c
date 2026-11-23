/* scr_help.c - '?' help screen.
 * SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "screen.h"

#include "widgets.h"

#include <curses.h>

/* What's actually wired up today, screen by screen -- kept in sync with
 * each scr_*.c's own key_bar so this never describes a dead key. Plain
 * static text for now, clipped to whatever fits (see draw() below). */
static const char *const HELP_LINES[] = {
    "Global keys",
    "  Up/Down, k/j, PgUp/PgDn, Home/End   move the selection",
    "  Tab or Left arrow                   focus the sidebar (Up/Down+Enter, or a",
    "                                       letter, jumps screens; Right/Enter returns)",
    "  ?                                    this help",
    "  p                                    play the active instance (once it's built)",
    "  q                                    quit immediately, on any screen (even mid-build)",
    "",
    "F2  Instances -- create, clone, rename, delete, snapshot, export/import",
    "  Enter                               make the selected instance active",
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
    "                                       v: a: r:>=N id: syntax",
    "  Enter or d                          download the selected patch (replaces an",
    "                                       already-downloaded copy with a fresh one)",
    "  D                                   download it and add it to the active",
    "                                       instance's patch stack, enabled",
    "  u                                   uninstall: remove it from the library",
    "  R                                   refresh the whole index from the site",
    "  Mk column: = exact match  ~ fuzzy  ! conflicts  ? unknown (colored to match)",
    "",
    "F4  Library -- every downloaded patch and version, independent of any instance",
    "  /                                   search: v: a: id: in:<instance> syntax",
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
    "  Up/Down, PgUp/PgDn, Home/End         scroll the output",
    "  error:/warning: lines colored red/yellow; p plays once a build succeeds",
    "",
    "F7  Sources -- base NetHack sources: folder, tarball, git",
    "  a                                   add from a local folder",
    "  t                                   add from a tarball (.tar.gz/.tgz/.zip/...)",
    "  g                                   add from a git tag (clones NetHack/NetHack)",
    "  x                                   remove the selected source (asks first; an",
    "                                       instance built from it keeps its own copy)",
};
static const size_t HELP_LINE_COUNT = sizeof(HELP_LINES) / sizeof(HELP_LINES[0]);

static void draw(ui_app *app)
{
    WINDOW *w = app->layout.main_win;
    if (w == NULL) {
        return;
    }
    (void)werase(w);
    ui_draw_panel(w, "Help");
    int inner_height = getmaxy(w) - 2;
    int inner_width = getmaxx(w) - 2;
    for (int i = 0; i < inner_height && (size_t)i < HELP_LINE_COUNT; i++) {
        (void)mvwaddnstr(w, i + 1, 1, HELP_LINES[i], inner_width);
    }
    (void)wnoutrefresh(w);
}

const ui_screen scr_help = {
    .name = "Help",
    .nav_letter = 'h',
    .key_bar = "Tab/Left menu  q quit",
    .enter = NULL,
    .leave = NULL,
    .draw = draw,
    .handle_key = NULL,
};
