/* widgets.c - list view, text view, input line, progress, dialogs.
 * SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "widgets.h"

#include <panel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ui_list_init(ui_list *list)
{
    list->selected = 0;
    list->top = 0;
}

void ui_list_move(ui_list *list, int delta, size_t count, int visible_rows)
{
    if (count == 0) {
        list->selected = 0;
        list->top = 0;
        return;
    }

    long next = (long)list->selected + delta;
    if (next < 0) {
        next = 0;
    }
    if (next >= (long)count) {
        next = (long)count - 1;
    }
    list->selected = (size_t)next;

    if (visible_rows <= 0) {
        return;
    }
    if (list->selected < list->top) {
        list->top = list->selected;
    } else if (list->selected >= list->top + (size_t)visible_rows) {
        list->top = list->selected - (size_t)visible_rows + 1;
    }
}

void ui_list_draw(const ui_list *list, WINDOW *win, int first_line, int visible_rows, int width,
                  size_t count, ui_list_draw_row_fn draw_row, void *user_data)
{
    if (win == NULL) {
        return;
    }
    for (int row = 0; row < visible_rows; row++) {
        size_t index = list->top + (size_t)row;
        if (index >= count) {
            break;
        }
        draw_row(win, first_line + row, width, index, index == list->selected, user_data);
    }
}

void ui_text_view_init(ui_text_view *view)
{
    view->top = 0;
}

static size_t text_view_max_top(size_t count, int visible_rows)
{
    if (visible_rows <= 0 || count <= (size_t)visible_rows) {
        return 0;
    }
    return count - (size_t)visible_rows;
}

void ui_text_view_scroll(ui_text_view *view, int delta, size_t count, int visible_rows)
{
    if (count == 0) {
        view->top = 0;
        return;
    }
    size_t top_max = text_view_max_top(count, visible_rows);
    long next = (long)view->top + delta;
    if (next < 0) {
        next = 0;
    }
    if (next > (long)top_max) {
        next = (long)top_max;
    }
    view->top = (size_t)next;
}

void ui_text_view_scroll_to(ui_text_view *view, size_t index, size_t count, int visible_rows)
{
    if (count == 0 || visible_rows <= 0) {
        view->top = 0;
        return;
    }
    if (index < view->top) {
        view->top = index;
    } else if (index >= view->top + (size_t)visible_rows) {
        view->top = index - (size_t)visible_rows + 1;
    }
    size_t top_max = text_view_max_top(count, visible_rows);
    if (view->top > top_max) {
        view->top = top_max;
    }
}

void ui_text_view_scroll_to_end(ui_text_view *view, size_t count, int visible_rows)
{
    view->top = text_view_max_top(count, visible_rows);
}

void ui_text_view_draw(const ui_text_view *view, WINDOW *win, int first_line, int visible_rows,
                       int width, size_t count, ui_text_view_draw_line_fn draw_line,
                       void *user_data)
{
    if (win == NULL) {
        return;
    }
    for (int row = 0; row < visible_rows; row++) {
        size_t index = view->top + (size_t)row;
        if (index >= count) {
            break;
        }
        draw_line(win, first_line + row, width, index, user_data);
    }
}

void ui_draw_panel(WINDOW *win, const char *title)
{
    if (win == NULL) {
        return;
    }
    /* Plain ASCII border -- '|', '-' and '+' corners, not ncurses' ACS
     * line-drawing glyphs, to match NetHack's own tty look rather than
     * a fancier Unicode-box style. */
    (void)wborder(win, '|', '|', '-', '-', '+', '+', '+', '+');
    if (title == NULL || title[0] == '\0') {
        return;
    }

    char label[128];
    (void)snprintf(label, sizeof(label), " %s ", title);
    int len = (int)strlen(label);
    int width = getmaxx(win);

    int col = (width - len) / 2;
    if (col < 1) {
        col = 1;
    }
    if (col + len > width - 1) {
        len = width - 1 - col;
    }
    if (len > 0) {
        (void)mvwaddnstr(win, 0, col, label, len);
    }
}

bool ui_confirm(ui_app *app, const char *message, void (*redraw)(ui_app *app))
{
    app->text_input_active = true;
    for (;;) {
        if (redraw != NULL) {
            redraw(app);
        }
        ui_set_status(app, "%s  (y/N)", message);
        ui_flush_status(app);

        wint_t ch = 0;
        int r = wget_wch(stdscr, &ch);
        if (r == KEY_CODE_YES && (ch == KEY_RESIZE || ch == KEY_MOUSE)) {
            continue;
        }
        if (ch == L'y' || ch == L'Y') {
            app->text_input_active = false;
            return true;
        }
        app->text_input_active = false;
        return false;
    }
}

typedef struct text_dialog_ctx {
    char **lines;
} text_dialog_ctx;

// cppcheck-suppress constParameterCallback -- must match ui_text_view_draw_line_fn's signature
static void draw_text_dialog_line(WINDOW *win, int y, int width, size_t index, void *user_data)
{
    const text_dialog_ctx *ctx = user_data;
    (void)mvwaddnstr(win, y, 1, ctx->lines[index], width - 1);
}

void ui_show_text_dialog(ui_app *app, const char *title, const char *text)
{
    size_t line_cap = 0;
    size_t line_count = 0;
    char **lines = NULL;
    const char *p = text;
    while (p != NULL && *p != '\0') {
        const char *nl = strchr(p, '\n');
        size_t len = (nl != NULL) ? (size_t)(nl - p) : strlen(p);
        if (line_count == line_cap) {
            size_t new_cap = (line_cap == 0) ? 256 : line_cap * 2;
            char **grown = realloc(lines, new_cap * sizeof(*grown));
            if (grown == NULL) {
                break;
            }
            lines = grown;
            line_cap = new_cap;
        }
        lines[line_count] = str_ndup(p, len);
        if (lines[line_count] == NULL) {
            break;
        }
        line_count++;
        p = (nl != NULL) ? nl + 1 : p + len;
    }

    ui_text_view view;
    ui_text_view_init(&view);
    app->text_input_active = true;
    for (;;) {
        WINDOW *w = app->layout.main_win;
        int visible = (w != NULL) ? getmaxy(w) - 2 : 1;
        if (w != NULL) {
            (void)werase(w);
            ui_draw_panel(w, title);
            text_dialog_ctx ctx = {.lines = lines};
            ui_text_view_draw(&view, w, 1, visible, getmaxx(w) - 2, line_count,
                              draw_text_dialog_line, &ctx);
            (void)wnoutrefresh(w);
        }
        ui_set_status(app, "line %zu/%zu -- Up/Down scroll, Esc/Enter/q close",
                      (line_count > 0) ? view.top + 1 : 0, line_count);
        ui_flush_status(app);

        wint_t ch = 0;
        int r = wget_wch(stdscr, &ch);
        if (r == KEY_CODE_YES && (ch == KEY_RESIZE || ch == KEY_MOUSE)) {
            continue;
        }
        if (ch == KEY_UP || ch == L'k') {
            ui_text_view_scroll(&view, -1, line_count, visible);
            continue;
        }
        if (ch == KEY_DOWN || ch == L'j') {
            ui_text_view_scroll(&view, 1, line_count, visible);
            continue;
        }
        if (ch == KEY_PPAGE) {
            ui_text_view_scroll(&view, -visible, line_count, visible);
            continue;
        }
        if (ch == KEY_NPAGE) {
            ui_text_view_scroll(&view, visible, line_count, visible);
            continue;
        }
        if (ch == KEY_HOME) {
            ui_text_view_scroll(&view, -(int)line_count, line_count, visible);
            continue;
        }
        if (ch == KEY_END) {
            ui_text_view_scroll_to_end(&view, line_count, visible);
            continue;
        }
        if (ch == 27 || ch == L'\n' || ch == KEY_ENTER || ch == L'q') {
            break;
        }
    }
    app->text_input_active = false;

    for (size_t i = 0; i < line_count; i++) {
        free(lines[i]);
    }
    free(lines);
}

bool ui_show_popup_menu(ui_app *app, const char *title, const char *const *options, size_t count,
                        size_t *out_index, void (*redraw)(ui_app *app))
{
    if (count == 0) {
        return false;
    }

    size_t max_option_len = strlen(title);
    for (size_t i = 0; i < count; i++) {
        size_t len = strlen(options[i]);
        if (len > max_option_len) {
            max_option_len = len;
        }
    }

    int max_y = 0;
    int max_x = 0;
    getmaxyx(stdscr, max_y, max_x);

    int win_width = (int)max_option_len + 4; /* border + one space padding each side */
    if (win_width > max_x) {
        win_width = max_x;
    }
    /* No scrolling: every caller picks from a handful of options, well under
     * the 80x24 minimum terminal (section 13.11). Revisit if a caller ever
     * needs more rows than that. */
    int win_height = (int)count + 2;
    if (win_height > max_y) {
        win_height = max_y;
    }
    if (win_width < 3 || win_height < 3) {
        return false; /* terminal too small to show anything useful */
    }

    int start_y = (max_y - win_height) / 2;
    int start_x = (max_x - win_width) / 2;
    if (start_y < 0) {
        start_y = 0;
    }
    if (start_x < 0) {
        start_x = 0;
    }

    WINDOW *win = newwin(win_height, win_width, start_y, start_x);
    if (win == NULL) {
        return false;
    }
    PANEL *panel = new_panel(win);
    if (panel == NULL) {
        (void)delwin(win);
        return false;
    }

    size_t selected = 0;
    bool committed = false;
    app->text_input_active = true;
    for (;;) {
        /* Drawn every loop, like ui_confirm()/popup_input_loop(), so
         * whatever's underneath stays current instead of a stale frame. */
        if (redraw != NULL) {
            redraw(app);
        }

        (void)werase(win);
        ui_draw_panel(win, title);
        int visible = win_height - 2;
        for (int i = 0; i < visible && (size_t)i < count; i++) {
            bool row_selected = ((size_t)i == selected);
            if (row_selected) {
                (void)wattron(win, A_REVERSE);
            }
            (void)mvwaddnstr(win, i + 1, 1, options[i], win_width - 2);
            if (row_selected) {
                (void)wattroff(win, A_REVERSE);
            }
        }
        /* update_panels(), not wnoutrefresh(win) directly -- the panel
         * library owns compositing the stack into the virtual screen. */
        (void)update_panels();
        (void)doupdate();

        wint_t ch = 0;
        int r = wget_wch(stdscr, &ch);
        if (r == KEY_CODE_YES && (ch == KEY_RESIZE || ch == KEY_MOUSE)) {
            continue;
        }
        if (ch == KEY_UP || ch == L'k') {
            if (selected > 0) {
                selected--;
            }
            continue;
        }
        if (ch == KEY_DOWN || ch == L'j') {
            if (selected + 1 < count) {
                selected++;
            }
            continue;
        }
        if (ch == L'\n' || ch == KEY_ENTER) {
            committed = true;
            break;
        }
        if (ch == 27) { /* Esc */
            break;
        }
    }
    app->text_input_active = false;

    (void)del_panel(panel);
    (void)delwin(win);
    /* Removing the panel doesn't redraw the plain windows underneath, so
     * without this the vacated spot flashes blank for one frame. */
    if (redraw != NULL) {
        redraw(app);
    }
    (void)update_panels();
    (void)doupdate();

    if (committed && out_index != NULL) {
        *out_index = selected;
    }
    return committed;
}

/* Shared by ui_show_popup_input() and ui_show_popup_input_live() below;
 * `on_change` (optional) runs after every edit, before Enter/Esc end the
 * loop, so a live filter box can re-run its search on each keystroke. */
static bool popup_input_loop(ui_app *app, const char *title, char *out, size_t out_size,
                             void (*redraw)(ui_app *app),
                             void (*on_change)(ui_app *app, const char *text))
{
    char edit[256];
    (void)snprintf(edit, sizeof(edit), "%s", out);

    int max_y = 0;
    int max_x = 0;
    getmaxyx(stdscr, max_y, max_x);

    int win_width = (int)strlen(title) + 4;
    if (win_width < 44) {
        win_width = 44;
    }
    if (win_width > max_x) {
        win_width = max_x;
    }
    int win_height = 4; /* border, input line, hint line, border */
    if (win_height > max_y) {
        win_height = max_y;
    }
    if (win_width < 3 || win_height < 3) {
        return false; /* terminal too small to show anything useful */
    }

    int start_y = (max_y - win_height) / 2;
    int start_x = (max_x - win_width) / 2;
    if (start_y < 0) {
        start_y = 0;
    }
    if (start_x < 0) {
        start_x = 0;
    }

    WINDOW *win = newwin(win_height, win_width, start_y, start_x);
    if (win == NULL) {
        return false;
    }
    PANEL *panel = new_panel(win);
    if (panel == NULL) {
        (void)delwin(win);
        return false;
    }

    int inner_width = win_width - 2;
    app->text_input_active = true;
    size_t cursor = strlen(edit); /* starts at the end, same as the old append-only behavior */
    (void)curs_set(1);            /* a real cursor here, hidden everywhere else (13.2) */
    bool committed = false;
    for (;;) {
        if (redraw != NULL) {
            redraw(app);
        }

        (void)werase(win);
        ui_draw_panel(win, title);

        /* Keeps the cursor in view, scrolling right as it passes the edge;
         * recomputed fresh from `cursor` each draw, so it scrolls back left
         * correctly too, with no extra state to track. */
        size_t edit_len = strlen(edit);
        size_t offset = (cursor >= (size_t)inner_width) ? cursor - (size_t)inner_width + 1 : 0;
        (void)mvwaddnstr(win, 1, 1, edit + offset, inner_width);
        (void)mvwaddnstr(
            win, 2, 1, (on_change != NULL) ? "Enter apply  Esc clear" : "Enter confirm  Esc cancel",
            inner_width);
        (void)wmove(win, 1, 1 + (int)(cursor - offset));

        (void)update_panels();
        (void)doupdate();

        wint_t ch = 0;
        int r = wget_wch(stdscr, &ch);
        if (r == KEY_CODE_YES && (ch == KEY_RESIZE || ch == KEY_MOUSE)) {
            continue;
        }
        if (ch == L'\n' || ch == KEY_ENTER) {
            committed = true;
            break;
        }
        if (ch == 27) { /* Esc */
            break;
        }
        if (ch == KEY_LEFT) {
            if (cursor > 0) {
                cursor--;
            }
            continue;
        }
        if (ch == KEY_RIGHT) {
            if (cursor < edit_len) {
                cursor++;
            }
            continue;
        }
        if (ch == KEY_HOME) {
            cursor = 0;
            continue;
        }
        if (ch == KEY_END) {
            cursor = edit_len;
            continue;
        }
        bool changed = false;
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (cursor > 0) {
                memmove(edit + cursor - 1, edit + cursor, edit_len - cursor + 1);
                cursor--;
                changed = true;
            }
        } else if (ch == KEY_DC) {
            if (cursor < edit_len) {
                memmove(edit + cursor, edit + cursor + 1, edit_len - cursor);
                changed = true;
            }
        } else if (ch >= 0x20 && ch < 0x7f) {
            if (edit_len + 1 < sizeof(edit)) {
                memmove(edit + cursor + 1, edit + cursor, edit_len - cursor + 1);
                edit[cursor] = (char)ch;
                cursor++;
                changed = true;
            }
        }
        if (changed && on_change != NULL) {
            on_change(app, edit);
        }
    }
    (void)curs_set(0);
    app->text_input_active = false;

    (void)del_panel(panel);
    (void)delwin(win);
    if (redraw != NULL) {
        redraw(app);
    }
    (void)update_panels();
    (void)doupdate();

    if (committed) {
        (void)snprintf(out, out_size, "%s", edit);
    }
    return committed;
}

bool ui_show_popup_input(ui_app *app, const char *title, char *out, size_t out_size,
                         void (*redraw)(ui_app *app))
{
    return popup_input_loop(app, title, out, out_size, redraw, NULL);
}

bool ui_show_popup_input_live(ui_app *app, const char *title, char *out, size_t out_size,
                              void (*redraw)(ui_app *app),
                              void (*on_change)(ui_app *app, const char *text))
{
    return popup_input_loop(app, title, out, out_size, redraw, on_change);
}
