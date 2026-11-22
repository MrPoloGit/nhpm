/* config.h - the config file, XDG paths, key=value/INI reader+writer.
 * SPDX-License-Identifier: NGPL */
#ifndef NHPM_CONFIG_H
#define NHPM_CONFIG_H

#include "util.h"

#include <stdbool.h>
#include <stddef.h>

/* ---- XDG base directories (docs/file-formats.md), "nhpm" already joined in ---- */

char *cfg_xdg_config_dir(void); /* $XDG_CONFIG_HOME/nhpm, falling back to ~/.config/nhpm */
char *cfg_xdg_cache_dir(void);  /* $XDG_CACHE_HOME/nhpm, falling back to ~/.cache/nhpm */
char *cfg_xdg_data_dir(void);   /* $XDG_DATA_HOME/nhpm, falling back to ~/.local/share/nhpm */

/* ---- generic key=value / INI documents (docs/style.md 6.12) ----
 * Ordered list of sections; top-level (name == NULL) holds lines before the
 * first [section]. Names may repeat (e.g. [patch]); each occurrence is its own cfg_section. */

typedef struct cfg_kv {
    char *key;
    char *value;
} cfg_kv;

typedef struct cfg_section {
    char *name; /* NULL for the top-level section */
    cfg_kv *items;
    size_t item_count;
    size_t item_cap;
} cfg_section;

typedef struct cfg_doc {
    cfg_section *sections;
    size_t section_count;
    size_t section_cap;
} cfg_doc;

void cfg_doc_init(cfg_doc *doc);
void cfg_doc_free(cfg_doc *doc);

/* Parses `text` into *out (already cfg_doc_init()'d). NHP_EPARSE if a line
 * is none of: blank, "# comment", "[section]", "key = value". */
nhp_err cfg_doc_parse(const char *text, cfg_doc *out);

/* Reads `path` and parses it; NHP_ENOENT if it doesn't exist. */
nhp_err cfg_doc_load(const char *path, cfg_doc *out);

/* Serializes *doc back to text. The caller owns the returned string. */
char *cfg_doc_format(const cfg_doc *doc);

/* Formats *doc and writes it to `path` with fs_write_atomic(). */
nhp_err cfg_doc_save(const char *path, const cfg_doc *doc);

/* The (0-based) `occurrence`'th section named `name` (NULL = top-level),
 * or NULL if there aren't that many. Owned by `doc`; invalidated by any
 * call that adds a section to it. */
cfg_section *cfg_doc_section(cfg_doc *doc, const char *name, size_t occurrence);

/* Number of sections named `name` (NULL for the top-level section). */
size_t cfg_doc_section_count(const cfg_doc *doc, const char *name);

/* Always appends a new section (for repeatable sections like [patch]);
 * `name` may be NULL to add a/another top-level section. */
cfg_section *cfg_doc_add_section(cfg_doc *doc, const char *name);

const char *cfg_section_get(const cfg_section *section, const char *key, const char *fallback);
long cfg_section_get_long(const cfg_section *section, const char *key, long fallback);
bool cfg_section_get_bool(const cfg_section *section, const char *key, bool fallback);

/* Sets key = value in `section`, replacing it if already present. */
nhp_err cfg_section_set(cfg_section *section, const char *key, const char *value);

/* Convenience wrappers that look up the first (occurrence 0) section named
 * `section_name` (NULL for the top-level section). */
const char *cfg_get(const cfg_doc *doc, const char *section_name, const char *key,
                    const char *fallback);
long cfg_get_long(const cfg_doc *doc, const char *section_name, const char *key, long fallback);
bool cfg_get_bool(const cfg_doc *doc, const char *section_name, const char *key, bool fallback);

/* Sets key = value in the first section named `section_name`, creating the
 * section (and the document's top-level section, for section_name == NULL)
 * if it doesn't exist yet. */
nhp_err cfg_set(cfg_doc *doc, const char *section_name, const char *key, const char *value);

/* ---- the app's own config file: ~/.config/nhpm/config (docs/file-formats.md) ---- */

typedef struct cfg_app {
    long index_max_age_days;
    long request_delay_ms;
    bool wayback_fallback;
    long max_download_mb;
    char *author_name; /* "" if not set */
    char *ignore;      /* extra ignore patterns (docs/features.md 12.1), "" if not set */
    char *pager;
    char *editor; /* "" if not set; $EDITOR wins when it's set */
} cfg_app;

void cfg_app_defaults(cfg_app *app);

/* Loads ~/.config/nhpm/config over whatever *app already holds (normally
 * the defaults); keys missing from the file keep their current value. The
 * file not existing at all is not an error. */
nhp_err cfg_app_load(cfg_app *app);

nhp_err cfg_app_save(const cfg_app *app);

/* Appends `pattern` to app->ignore and saves (docs/features.md 12.5's
 * Changes-dialog "i"). No-op if already present; NHP_EINVAL for "" or NULL. */
nhp_err cfg_app_add_ignore_pattern(cfg_app *app, const char *pattern);

void cfg_app_free(cfg_app *app);

/* ---- the app's own UI state: ~/.config/nhpm/ui-state (docs/features.md
 * 10.7) -- only active_instance is implemented so far (what CLI mode needs);
 * the rest (last_screen, db_filter, ...) is UI-only, added when M7 needs it. ---- */

/* The caller frees the result; NULL if ui-state doesn't exist or has no
 * active_instance set. */
char *cfg_get_active_instance(void);

/* Writes just the active_instance key, preserving any other keys already
 * in the file (so M7's other ui-state fields survive a CLI-mode "inst
 * use"). */
nhp_err cfg_set_active_instance(const char *name);

#endif /* NHPM_CONFIG_H */
