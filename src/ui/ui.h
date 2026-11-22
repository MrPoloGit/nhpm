/* ui.h - ncurses setup/teardown, colors, main loop, resize, key bar.
 * SPDX-License-Identifier: NGPL */
#ifndef NHPM_UI_H
#define NHPM_UI_H

#include "util.h"

/* Runs the full ncurses interface until the user quits ('q'). NHP_EIO
 * if the terminal couldn't be initialized. `debug` opens a timestamped
 * activity log of every status-line message, printed on exit. */
nhp_err ui_run(bool debug);

#endif /* NHPM_UI_H */
