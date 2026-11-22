/* version.c - parse "NetHack 3.6.0", read patchlevel.h, compare versions.
 * SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "version.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void ver_free(ver_version *v)
{
    free(v->raw);
    memset(v, 0, sizeof(*v));
}

nhp_err ver_parse(const char *text, ver_version *out)
{
    memset(out, 0, sizeof(*out));
    out->major = out->minor = out->patch = -1;
    out->raw = str_dup(text);
    if (out->raw == NULL) {
        return NHP_ENOMEM;
    }

    const char *p = text;
    while (isspace((unsigned char)*p)) {
        p++;
    }

    static const char prefix[] = "nethack";
    size_t prefix_len = strlen(prefix);
    bool has_prefix = strncasecmp(p, prefix, prefix_len) == 0 &&
                      (p[prefix_len] == '\0' || isspace((unsigned char)p[prefix_len]));
    if (has_prefix) {
        p += prefix_len;
        while (isspace((unsigned char)*p)) {
            p++;
        }
    }
    /* A bare number (e.g. "3.6.7") is vanilla, not a variant: no brand name
     * to mismatch. Leading non-digit text that isn't "NetHack" is a named
     * variant (SLASH'EM, UnNetHack, ...). */
    out->is_variant = !has_prefix && !isdigit((unsigned char)*p);

    char *end = NULL;
    long major = strtol(p, &end, 10);
    if (end == p || *end != '.') {
        return NHP_OK; /* no N.N[.N] here; leave major/minor/patch at -1 */
    }
    p = end + 1;

    long minor = strtol(p, &end, 10);
    if (end == p) {
        return NHP_OK;
    }
    p = end;

    long patch = 0;
    if (*p == '.') {
        p++;
        patch = strtol(p, &end, 10);
        if (end == p) {
            patch = 0;
        }
    }

    out->major = (int)major;
    out->minor = (int)minor;
    out->patch = (int)patch;
    return NHP_OK;
}

/* Finds "#define <macro> <number>" anywhere in `text`, not anchored to
 * line starts -- simpler than a line-by-line scan, and patchlevel.h has
 * no plausible false-positive substring match. */
static bool find_define_int(const char *text, const char *macro, long *out)
{
    size_t macro_len = strlen(macro);
    const char *p = text;
    while ((p = strstr(p, macro)) != NULL) {
        bool boundary_before = (p == text) || !(isalnum((unsigned char)p[-1]) || p[-1] == '_');
        bool boundary_after = !(isalnum((unsigned char)p[macro_len]) || p[macro_len] == '_');
        if (boundary_before && boundary_after) {
            const char *after = p + macro_len;
            while (*after == ' ' || *after == '\t') {
                after++;
            }
            char *end = NULL;
            long value = strtol(after, &end, 10);
            if (end != after) {
                *out = value;
                return true;
            }
        }
        p += macro_len;
    }
    return false;
}

nhp_err ver_read_patchlevel(const char *source_dir, ver_version *out)
{
    char *path = path_join(source_dir, "include/patchlevel.h");
    if (path == NULL) {
        return NHP_ENOMEM;
    }

    char *text = NULL;
    size_t len = 0;
    nhp_err err = fs_read_file(path, &text, &len);
    free(path);
    if (err != NHP_OK) {
        return err;
    }

    long major = 0;
    long minor = 0;
    long patch = 0;
    bool have_major = find_define_int(text, "VERSION_MAJOR", &major);
    bool have_minor = find_define_int(text, "VERSION_MINOR", &minor);
    bool have_patch = find_define_int(text, "PATCHLEVEL", &patch);
    free(text);

    if (!have_major || !have_minor) {
        err_set("could not find VERSION_MAJOR/VERSION_MINOR in %s/include/patchlevel.h",
                source_dir);
        return NHP_ENOENT;
    }

    memset(out, 0, sizeof(*out));
    out->major = (int)major;
    out->minor = (int)minor;
    out->patch = have_patch ? (int)patch : 0;
    out->is_variant = false; /* read straight from NetHack's own header */
    out->raw = ver_format(out);
    return (out->raw != NULL) ? NHP_OK : NHP_ENOMEM;
}

char *ver_format(const ver_version *v)
{
    if (v->major < 0) {
        return str_dup((v->raw != NULL) ? v->raw : "");
    }
    buf b;
    buf_init(&b);
    buf_append_fmt(&b, "%d.%d.%d", v->major, v->minor, v->patch);
    return buf_take(&b);
}

ver_compat ver_compatibility(const ver_version *instance_version, const ver_version *site_version)
{
    if (instance_version->is_variant || site_version->is_variant) {
        return VER_COMPAT_UNKNOWN;
    }
    if (instance_version->major < 0 || site_version->major < 0) {
        return VER_COMPAT_UNKNOWN;
    }
    if (instance_version->major != site_version->major ||
        instance_version->minor != site_version->minor) {
        return VER_COMPAT_DIFFERENT;
    }
    if (instance_version->patch == site_version->patch) {
        return VER_COMPAT_EXACT;
    }
    return VER_COMPAT_SERIES;
}

char ver_compat_mark(ver_compat compat)
{
    switch (compat) {
        case VER_COMPAT_EXACT:
            return '=';
        case VER_COMPAT_SERIES:
            return '~';
        case VER_COMPAT_DIFFERENT:
            return '!';
        case VER_COMPAT_UNKNOWN:
        default:
            return '?';
    }
}
