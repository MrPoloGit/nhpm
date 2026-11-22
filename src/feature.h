/* feature.h - portability macros, included first in every .c file.
 * SPDX-License-Identifier: NGPL */
#ifndef NHPM_FEATURE_H
#define NHPM_FEATURE_H

/* POSIX.1-2008 / XSI, defined before any system header is pulled in. Also
 * enables the wide-character ncurses functions (add_wch, mvaddwstr,
 * get_wch); see docs/style.md section 6.1. */
#define _XOPEN_SOURCE 700

/* On macOS, _XOPEN_SOURCE alone hides BSD/Darwin extensions we still need
 * (mkdtemp, among others); ask for those back without relaxing _XOPEN_SOURCE
 * itself, and without affecting non-Darwin builds. */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#endif /* NHPM_FEATURE_H */
