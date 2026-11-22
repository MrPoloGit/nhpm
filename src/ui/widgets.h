/* widgets.h - list view, text view, input line, progress, dialogs.
 * SPDX-License-Identifier: NGPL */
#ifndef NHPM_UI_WIDGETS_H
#define NHPM_UI_WIDGETS_H

#include "screen.h"

#include <curses.h>
#include <stdbool.h>
#include <stddef.h>

/* A scrolling, single-selection list (docs/features.md section 13.5).
 * The caller owns the actual row data; this only tracks which row is
 * selected and scrolled into view. */
typedef struct ui_list {
    size_t selected;
    size_t top; /* index of the first visible row */
} ui_list;

void ui_list_init(ui_list *list);

/* Moves the selection by `delta` rows (negative = up), clamped to
 * [0, count) (a no-op if count == 0), and scrolls `top` so the
 * selection stays within a window `visible_rows` tall. */
void ui_list_move(ui_list *list, int delta, size_t count, int visible_rows);

/* Draws one row at screen line `y`, up to `width` columns, highlighted
 * if `selected` is true. `user_data` is whatever ui_list_draw()'s
 * caller passed it. */
typedef void (*ui_list_draw_row_fn)(WINDOW *win, int y, int width, size_t index, bool selected,
                                    void *user_data);

/* Draws rows [list->top .. list->top + visible_rows) of `count` total
 * into `win` at `first_line`, calling `draw_row` for each visible one.
 * `monochrome` selects reverse-video instead of a color pair (13.8). */
void ui_list_draw(const ui_list *list, WINDOW *win, int first_line, int visible_rows, int width,
                  size_t count, ui_list_draw_row_fn draw_row, void *user_data);

/* A scrolling, read-only view over lines (13.5; Build's streamed output,
 * later Changes/diff). Unlike ui_list, no persistent "selected" row -- a
 * screen that needs one keeps its own index and scrolls via ui_text_view_scroll_to(). */
typedef struct ui_text_view {
    size_t top; /* index of the first visible line */
} ui_text_view;

void ui_text_view_init(ui_text_view *view);

/* Scrolls `top` by `delta` lines (negative = up), clamped so the view
 * never scrolls past the last line (a no-op if count == 0). */
void ui_text_view_scroll(ui_text_view *view, int delta, size_t count, int visible_rows);

/* Scrolls just enough that line `index` is visible -- up if it's above
 * the window, down if it's below, untouched if it's already in view. */
void ui_text_view_scroll_to(ui_text_view *view, size_t index, size_t count, int visible_rows);

/* Scrolls to the last page, for "follow" behavior while more lines keep
 * arriving (the Build screen's streamed output). */
void ui_text_view_scroll_to_end(ui_text_view *view, size_t count, int visible_rows);

/* Draws one line at screen line `y`, up to `width` columns; `user_data`
 * is whatever ui_text_view_draw()'s caller passed it. */
typedef void (*ui_text_view_draw_line_fn)(WINDOW *win, int y, int width, size_t index,
                                          void *user_data);

/* Draws lines [view->top .. view->top + visible_rows) of `count` total
 * into `win`, starting at line `first_line`, calling `draw_line` for each
 * one actually visible. */
void ui_text_view_draw(const ui_text_view *view, WINDOW *win, int first_line, int visible_rows,
                       int width, size_t count, ui_text_view_draw_line_fn draw_line,
                       void *user_data);

/* Draws a bordered panel filling `win`, with `title` centered in the top
 * border ("" for a plain border). Screens draw content starting at (1, 1),
 * (height-2) rows by (width-2) columns, after calling this. */
void ui_draw_panel(WINDOW *win, const char *title);

/* A blocking single-line prompt in the status line (13.5); `redraw` runs
 * before every key so what's behind stays current. `out` is pre-filled for
 * editing; Enter commits (returns true), Esc leaves it untouched (false). */
bool ui_prompt_line(ui_app *app, const char *prompt, char *out, size_t out_size,
                    void (*redraw)(ui_app *app));

/* A blocking y/N confirmation in the status line. 'y'/'Y' confirms;
 * anything else (including Enter and Esc) declines -- there's no key
 * that destroys something by accident. */
bool ui_confirm(ui_app *app, const char *message, void (*redraw)(ui_app *app));

/* A blocking, scrollable view of `text` (split on '\n'), filling the
 * screen's main panel under `title` -- for a diff or long text without a
 * dedicated screen (12.5). Up/Down/k/j/PgUp/PgDn/Home/End scroll; Esc/Enter/q closes. */
void ui_show_text_dialog(ui_app *app, const char *title, const char *text);

/* A small bordered popup menu, floating over the screen via panelw,
 * centered and sized to its longest option. Up/Down/k/j move; Enter
 * selects (*out_index set, true); Esc cancels (false, untouched). */
bool ui_show_popup_menu(ui_app *app, const char *title, const char *const *options, size_t count,
                        size_t *out_index, void (*redraw)(ui_app *app));

/* Like ui_prompt_line(), but in the floating panel-backed box from
 * ui_show_popup_menu() -- for free text (a name, path, git tag). Enter
 * commits (out holds the text, true); Esc leaves it untouched (false). */
bool ui_show_popup_input(ui_app *app, const char *title, char *out, size_t out_size,
                         void (*redraw)(ui_app *app));

#endif /* NHPM_UI_WIDGETS_H */
