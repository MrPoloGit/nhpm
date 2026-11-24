/* screen.h - shared app state and the screen vtable (docs/features.md
 * section 13.4), internal to src/ui/.
 * SPDX-License-Identifier: NGPL */
#ifndef NHPM_UI_SCREEN_H
#define NHPM_UI_SCREEN_H

#include "config.h"
#include "instance.h"
#include "mkpatch.h"
#include "stack.h"
#include "util.h"

#include <curses.h>
#include <stddef.h>

/* The windows every screen draws into, top to bottom: header, sidebar |
 * main, details, status, key bar (docs 13.3, plus a sidebar for screen
 * nav -- see ui_focus). Created by relayout(), recreated on KEY_RESIZE. */
typedef struct ui_layout {
    WINDOW *header;
    WINDOW *sidebar;
    WINDOW *main_win;
    WINDOW *details;
    WINDOW *status;
    WINDOW *keybar;
    int rows, cols;
    bool too_small; /* below 80x24 (section 13.11); screens skip drawing */
} ui_layout;

/* Which pane arrow keys currently control; Tab or Left/Right at the pane
 * edges switches. Nav letters only jump screens while the sidebar has
 * focus, so they never shadow a screen's own action letters (e.g. "d"). */
typedef enum ui_focus {
    UI_FOCUS_CONTENT,
    UI_FOCUS_SIDEBAR,
} ui_focus;

/* Color pairs, defined once by role, not by color (section 13.8). 0 is
 * ncurses' built-in "default" pair, so roles start at 1. */
enum ui_color_pair {
    UI_COLOR_HEADER = 1, /* black on cyan */
    UI_COLOR_SELECTED,   /* black on cyan (or reverse, monochrome) */
    UI_COLOR_OK,         /* green: enabled / applied / "=" */
    UI_COLOR_WARN,       /* yellow: warning / fuzz / "~" */
    UI_COLOR_ERROR,      /* red: conflict / error / "!" */
    UI_COLOR_DIM,        /* dim/grey: disabled / unknown / "?" */
};

typedef struct ui_app {
    ui_layout layout;
    cfg_app config;

    inst_instance active_inst; /* zeroed (not loaded) if active_name is "" */
    char *active_name;         /* "" if no active instance */
    bool active_loaded;

    /* docs 10.8: held as long as active_name stays the same, not
     * re-acquired on every refresh (avoids thrashing flock()).
     * active_lock_name tracks which name it belongs to. */
    inst_lock active_lock;
    bool active_lock_held;
    char *active_lock_name; /* "" if active_lock_held is false and nothing
                             * is locked out either */
    /* set instead of active_lock_held when another nhpm already holds the
     * lock; ui_require_write_access() checks active_read_only first. */
    bool active_read_only;
    pid_t active_lock_other_pid;

    char *status_message; /* shown in the status line until replaced */
    bool quit_requested;
    bool monochrome; /* NO_COLOR or no color support (section 13.8) */

    int screen_index;      /* which of g_screens[] is current */
    ui_focus focus;        /* which pane arrow keys control right now */
    size_t sidebar_cursor; /* the sidebar's own selection, independent of
                            * screen_index until Enter commits it */

    /* Set by a screen while capturing raw text (e.g. the Database "/"
     * search box) -- handle_global_key() then leaves every key to the
     * screen's own handle_key() instead of stealing them first. */
    bool text_input_active;
} ui_app;

typedef struct ui_screen {
    const char *name;    /* e.g. "Instances", shown in the header */
    char nav_letter;     /* jumps here when the sidebar has focus -- never
                          * active otherwise, so it can't shadow a screen's
                          * own action letters (e.g. Patches' "d" for Diff) */
    const char *key_bar; /* this screen's key bar label */
    void (*enter)(ui_app *app);
    void (*leave)(ui_app *app);
    void (*draw)(ui_app *app);
    /* Returns true if `ch` was handled (so the main loop won't also try
     * global keys a second time -- though global keys are checked
     * first, per section 13.4). */
    bool (*handle_key)(ui_app *app, wint_t ch);
} ui_screen;

/* Replaces the status line's message (freed on the next call or at
 * shutdown). "" clears it. */
void ui_set_status(ui_app *app, const char *fmt, ...) NHP_PRINTF(2, 3);

/* Redraws just the status line and calls doupdate() immediately -- for
 * progress callbacks during a long, blocking operation (docs 13.6), which
 * can't wait for the main loop's own next redraw. */
void ui_flush_status(ui_app *app);

/* (Re)loads ui_app's active_inst; safe to call repeatedly to pick up the
 * latest saved state. On a genuine instance switch, also takes/checks its
 * lock (10.8) and offers Adopt/Restore if the folder has drifted (10.4). */
void ui_reload_active_instance(ui_app *app);

/* docs 10.8: false (with a status message) if another nhpm holds the
 * active instance's lock, true otherwise; false with no message if
 * there's no active instance. Every mutating screen action calls this first. */
bool ui_require_write_access(ui_app *app);

/* Shows a conflict's full detail (docs 11.6's "[V]iew the failing hunks"):
 * id/version, each file's hunk tally, then the raw quilt output, in
 * ui_show_text_dialog(). Called by every screen that can hit NHP_ECONFLICT. */
void ui_show_conflict(ui_app *app, const stack_conflict *conflict);

/* docs 11.6's Force/View/Cancel for a conflict on the patch at `index`
 * itself (conflict->is_requested_patch -- otherwise fall back to
 * ui_show_conflict()). Returns true only if Force completed. */
bool ui_resolve_conflict(ui_app *app, inst_instance *inst, size_t index,
                         const stack_conflict *conflict);

/* Resolves $EDITOR (then config's own `editor`, else "vi") and hands the
 * terminal to it with every path in `paths` as an argument (13.7). */
nhp_err ui_run_editor(ui_app *app, const char *const *paths, size_t count);

/* The Changes dialog (docs 12.5): review the active instance's uncaptured
 * edits and save some as a new patch. Not a screen (no F-key); opened by
 * 'w' on Patches, blocking until Esc. Defined in scr_changes.c. */
void ui_show_changes_dialog(ui_app *app);

/* docs 12.6's "compares... shows the Changes screen" step: same dialog as
 * ui_show_changes_dialog(), but reviews `folder` against `base` and saves
 * via mkp_from_folder(). Opened by 'm' on Library; neither pointer is kept. */
void ui_show_changes_dialog_for_folder(ui_app *app, const char *folder, const mkp_base_spec *base);

/* Hands the terminal to the active instance's game and back (13.7); Instances'
 * 'p' Play. Defined in ui.c since it needs def_prog_mode()/endwin()/refresh(). */
void ui_suspend_and_run_play(ui_app *app);

/* One extern per screen (docs/repo-structure.md: "adding a screen means
 * adding one file"), each a const ui_screen defined in its own
 * scr_*.c. */
extern const ui_screen scr_help;
extern const ui_screen scr_instances;
extern const ui_screen scr_db;
extern const ui_screen scr_library;
extern const ui_screen scr_patches;
extern const ui_screen scr_build;
extern const ui_screen scr_sources;

#endif /* NHPM_UI_SCREEN_H */
