/* version.h - parse "NetHack 3.6.0", read patchlevel.h, compare versions.
 * SPDX-License-Identifier: NGPL */
#ifndef NHPM_VERSION_H
#define NHPM_VERSION_H

#include "util.h"

typedef struct ver_version {
    int major; /* -1 if no numeric version could be determined */
    int minor;
    int patch;
    bool is_variant; /* not vanilla NetHack (docs/features.md section 9.3) */
    char *raw;       /* the original text parsed, or "major.minor.patch" */
} ver_version;

void ver_free(ver_version *v);

/* Parses strings like "NetHack 3.6.0" or "3.6.7". Sets is_variant if the
 * text doesn't start with "NetHack"; major/minor/patch are -1 if no
 * N.N[.N] pattern is found. Never fails (becomes an unknown variant). */
nhp_err ver_parse(const char *text, ver_version *out);

/* Reads VERSION_MAJOR/VERSION_MINOR/PATCHLEVEL from
 * <source_dir>/include/patchlevel.h. NHP_ENOENT if the file is missing or
 * doesn't define at least VERSION_MAJOR and VERSION_MINOR. */
nhp_err ver_read_patchlevel(const char *source_dir, ver_version *out);

/* "major.minor.patch", e.g. "3.6.7"; falls back to the original raw text
 * if no numeric version is available. */
char *ver_format(const ver_version *v);

typedef enum ver_compat {
    VER_COMPAT_EXACT,     /* '=' */
    VER_COMPAT_SERIES,    /* '~' same major.minor */
    VER_COMPAT_DIFFERENT, /* '!' */
    VER_COMPAT_UNKNOWN,   /* '?' not vanilla, or unparseable */
} ver_compat;

/* Compares a site "For" version against an instance's base version
 * (docs/features.md section 9.4; these are hints -- the real answer is a
 * dry run against the instance, section 11.4). */
ver_compat ver_compatibility(const ver_version *instance_version, const ver_version *site_version);

char ver_compat_mark(ver_compat compat);

#endif /* NHPM_VERSION_H */
