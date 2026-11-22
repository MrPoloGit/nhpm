/* config.c - the config file, XDG paths, key=value/INI reader+writer.
 * SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- XDG base directories ---- */

static char *xdg_dir(const char *env_name, const char *home_fallback_suffix)
{
    const char *env_val = getenv(env_name);
    char *base;
    if (env_val != NULL && env_val[0] != '\0') {
        base = str_dup(env_val);
    } else {
        const char *home = getenv("HOME");
        if (home == NULL || home[0] == '\0') {
            err_set("could not determine the home directory ($HOME is not set)");
            return NULL;
        }
        base = path_join(home, home_fallback_suffix);
    }
    if (base == NULL) {
        return NULL;
    }
    char *full = path_join(base, "nhpm");
    free(base);
    return full;
}

char *cfg_xdg_config_dir(void)
{
    return xdg_dir("XDG_CONFIG_HOME", ".config");
}

char *cfg_xdg_cache_dir(void)
{
    return xdg_dir("XDG_CACHE_HOME", ".cache");
}

char *cfg_xdg_data_dir(void)
{
    return xdg_dir("XDG_DATA_HOME", ".local/share");
}

/* ---- documents ---- */

void cfg_doc_init(cfg_doc *doc)
{
    doc->sections = NULL;
    doc->section_count = 0;
    doc->section_cap = 0;
}

static void section_free_contents(cfg_section *s)
{
    for (size_t i = 0; i < s->item_count; i++) {
        free(s->items[i].key);
        free(s->items[i].value);
    }
    free(s->items);
    free(s->name);
}

void cfg_doc_free(cfg_doc *doc)
{
    for (size_t i = 0; i < doc->section_count; i++) {
        section_free_contents(&doc->sections[i]);
    }
    free(doc->sections);
    doc->sections = NULL;
    doc->section_count = 0;
    doc->section_cap = 0;
}

cfg_section *cfg_doc_add_section(cfg_doc *doc, const char *name)
{
    if (doc->section_count == doc->section_cap) {
        size_t new_cap = (doc->section_cap == 0) ? 4 : doc->section_cap * 2;
        cfg_section *grown = realloc(doc->sections, new_cap * sizeof(*grown));
        if (grown == NULL) {
            err_set("out of memory adding a section");
            return NULL;
        }
        doc->sections = grown;
        doc->section_cap = new_cap;
    }

    /* section_count == section_cap here is impossible: it was already
     * false, or the growth above just fixed it, so a slot always exists. */
    cfg_section *s = &doc->sections[doc->section_count];
    s->name = (name != NULL) ? str_dup(name) : NULL; // NOLINT(clang-analyzer-core.NullDereference)
    if (name != NULL && s->name == NULL) {
        return NULL;
    }
    s->items = NULL;
    s->item_count = 0;
    s->item_cap = 0;
    doc->section_count++;
    return s;
}

static bool section_matches(const cfg_section *s, const char *name)
{
    return (name == NULL) ? (s->name == NULL) : (s->name != NULL && str_eq(s->name, name));
}

/* Returns the index of the `occurrence`'th section named `name`, or -1. */
static long section_index(const cfg_doc *doc, const char *name, size_t occurrence)
{
    size_t seen = 0;
    for (size_t i = 0; i < doc->section_count; i++) {
        if (section_matches(&doc->sections[i], name)) {
            if (seen == occurrence) {
                return (long)i;
            }
            seen++;
        }
    }
    return -1;
}

cfg_section *cfg_doc_section(cfg_doc *doc, const char *name, size_t occurrence)
{
    long idx = section_index(doc, name, occurrence);
    return (idx < 0) ? NULL : &doc->sections[idx];
}

size_t cfg_doc_section_count(const cfg_doc *doc, const char *name)
{
    size_t count = 0;
    for (size_t i = 0; i < doc->section_count; i++) {
        if (section_matches(&doc->sections[i], name)) {
            count++;
        }
    }
    return count;
}

const char *cfg_section_get(const cfg_section *section, const char *key, const char *fallback)
{
    if (section == NULL) {
        return fallback;
    }
    for (size_t i = 0; i < section->item_count; i++) {
        if (str_eq(section->items[i].key, key)) {
            return section->items[i].value;
        }
    }
    return fallback;
}

long cfg_section_get_long(const cfg_section *section, const char *key, long fallback)
{
    const char *raw = cfg_section_get(section, key, NULL);
    if (raw == NULL || raw[0] == '\0') {
        return fallback;
    }
    errno = 0;
    char *end = NULL;
    long value = strtol(raw, &end, 10);
    if (end == raw || *end != '\0' || errno == ERANGE) {
        return fallback;
    }
    return value;
}

bool cfg_section_get_bool(const cfg_section *section, const char *key, bool fallback)
{
    const char *raw = cfg_section_get(section, key, NULL);
    if (raw == NULL) {
        return fallback;
    }
    if (str_eq(raw, "yes") || str_eq(raw, "true") || str_eq(raw, "1")) {
        return true;
    }
    if (str_eq(raw, "no") || str_eq(raw, "false") || str_eq(raw, "0")) {
        return false;
    }
    return fallback;
}

nhp_err cfg_section_set(cfg_section *section, const char *key, const char *value)
{
    for (size_t i = 0; i < section->item_count; i++) {
        if (str_eq(section->items[i].key, key)) {
            char *new_value = str_dup(value);
            if (new_value == NULL) {
                return NHP_ENOMEM;
            }
            free(section->items[i].value);
            section->items[i].value = new_value;
            return NHP_OK;
        }
    }

    if (section->item_count == section->item_cap) {
        size_t new_cap = (section->item_cap == 0) ? 4 : section->item_cap * 2;
        cfg_kv *grown = realloc(section->items, new_cap * sizeof(*grown));
        if (grown == NULL) {
            return NHP_ENOMEM;
        }
        section->items = grown;
        section->item_cap = new_cap;
    }

    char *key_dup = str_dup(key);
    char *value_dup = str_dup(value);
    if (key_dup == NULL || value_dup == NULL) {
        free(key_dup);
        free(value_dup);
        return NHP_ENOMEM;
    }
    section->items[section->item_count].key = key_dup;
    section->items[section->item_count].value = value_dup;
    section->item_count++;
    return NHP_OK;
}

const char *cfg_get(const cfg_doc *doc, const char *section_name, const char *key,
                    const char *fallback)
{
    long idx = section_index(doc, section_name, 0);
    return (idx < 0) ? fallback : cfg_section_get(&doc->sections[idx], key, fallback);
}

long cfg_get_long(const cfg_doc *doc, const char *section_name, const char *key, long fallback)
{
    long idx = section_index(doc, section_name, 0);
    return (idx < 0) ? fallback : cfg_section_get_long(&doc->sections[idx], key, fallback);
}

bool cfg_get_bool(const cfg_doc *doc, const char *section_name, const char *key, bool fallback)
{
    long idx = section_index(doc, section_name, 0);
    return (idx < 0) ? fallback : cfg_section_get_bool(&doc->sections[idx], key, fallback);
}

nhp_err cfg_set(cfg_doc *doc, const char *section_name, const char *key, const char *value)
{
    cfg_section *s = cfg_doc_section(doc, section_name, 0);
    if (s == NULL) {
        s = cfg_doc_add_section(doc, section_name);
        if (s == NULL) {
            return NHP_ENOMEM;
        }
    }
    return cfg_section_set(s, key, value);
}

/* ---- parsing / formatting ---- */

/* Handles one already-trimmed, non-blank, non-comment line: a "[section]"
 * header or a "key = value" pair. `current` tracks the section lines are
 * currently being added to (created lazily for top-level keys). */
static nhp_err parse_line(char *trimmed, size_t line_no, cfg_doc *out, cfg_section **current)
{
    size_t len = strlen(trimmed);
    if (trimmed[0] == '[' && trimmed[len - 1] == ']') {
        trimmed[len - 1] = '\0';
        const char *name = str_trim(trimmed + 1);
        *current = cfg_doc_add_section(out, name);
        return (*current == NULL) ? NHP_ENOMEM : NHP_OK;
    }

    char *eq = strchr(trimmed, '=');
    if (eq == NULL) {
        err_set("line %zu: expected \"key = value\", \"[section]\" or a \"#\" comment: %s", line_no,
                trimmed);
        return NHP_EPARSE;
    }
    *eq = '\0';
    const char *key = str_trim(trimmed);
    const char *value = str_trim(eq + 1);
    if (key[0] == '\0') {
        err_set("line %zu: empty key", line_no);
        return NHP_EPARSE;
    }

    if (*current == NULL) {
        *current = cfg_doc_add_section(out, NULL);
        if (*current == NULL) {
            return NHP_ENOMEM;
        }
    }
    return cfg_section_set(*current, key, value);
}

nhp_err cfg_doc_parse(const char *text, cfg_doc *out)
{
    cfg_section *current = NULL;
    size_t line_no = 0;
    const char *p = text;

    while (*p != '\0') {
        const char *line_start = p;
        while (*p != '\0' && *p != '\n') {
            p++;
        }
        size_t raw_len = (size_t)(p - line_start);
        if (*p == '\n') {
            p++;
        }
        line_no++;

        char *line = str_ndup(line_start, raw_len);
        if (line == NULL) {
            return NHP_ENOMEM;
        }

        char *trimmed = str_trim(line);
        if (trimmed[0] != '\0' && trimmed[0] != '#') {
            nhp_err err = parse_line(trimmed, line_no, out, &current);
            if (err != NHP_OK) {
                free(line);
                return err;
            }
        }
        free(line);
    }

    return NHP_OK;
}

nhp_err cfg_doc_load(const char *path, cfg_doc *out)
{
    char *text = NULL;
    size_t len = 0;
    nhp_err err = fs_read_file(path, &text, &len);
    if (err != NHP_OK) {
        return err;
    }
    err = cfg_doc_parse(text, out);
    free(text);
    return err;
}

char *cfg_doc_format(const cfg_doc *doc)
{
    buf b;
    buf_init(&b);
    for (size_t i = 0; i < doc->section_count; i++) {
        const cfg_section *s = &doc->sections[i];
        if (s->name != NULL && !buf_append_fmt(&b, "[%s]\n", s->name)) {
            buf_free(&b);
            return NULL;
        }
        for (size_t j = 0; j < s->item_count; j++) {
            if (!buf_append_fmt(&b, "%s = %s\n", s->items[j].key, s->items[j].value)) {
                buf_free(&b);
                return NULL;
            }
        }
        if (i + 1 < doc->section_count && !buf_append_str(&b, "\n")) {
            buf_free(&b);
            return NULL;
        }
    }
    return buf_take(&b);
}

nhp_err cfg_doc_save(const char *path, const cfg_doc *doc)
{
    char *text = cfg_doc_format(doc);
    if (text == NULL) {
        return NHP_ENOMEM;
    }
    nhp_err err = fs_write_atomic(path, text, strlen(text));
    free(text);
    return err;
}

/* ---- the app's own config file ---- */

void cfg_app_defaults(cfg_app *app)
{
    app->index_max_age_days = 7;
    app->request_delay_ms = 1000;
    app->wayback_fallback = true;
    app->max_download_mb = 5;
    app->author_name = str_dup("");
    app->ignore = str_dup("");
    app->pager = str_dup("less");
    app->editor = str_dup("vi");
}

static char *app_config_path(void)
{
    char *dir = cfg_xdg_config_dir();
    if (dir == NULL) {
        return NULL;
    }
    char *path = path_join(dir, "config");
    free(dir);
    return path;
}

/* Replaces *field with a copy of `raw` if `raw` is non-NULL (i.e. the key
 * was present in the loaded file); leaves *field untouched otherwise. */
static nhp_err apply_string_override(char **field, const char *raw)
{
    if (raw == NULL) {
        return NHP_OK;
    }
    char *copy = str_dup(raw);
    if (copy == NULL) {
        return NHP_ENOMEM;
    }
    free(*field);
    *field = copy;
    return NHP_OK;
}

nhp_err cfg_app_load(cfg_app *app)
{
    char *path = app_config_path();
    if (path == NULL) {
        return NHP_ENOMEM;
    }

    cfg_doc doc;
    cfg_doc_init(&doc);
    nhp_err err = cfg_doc_load(path, &doc);
    free(path);
    if (err == NHP_ENOENT) {
        cfg_doc_free(&doc);
        return NHP_OK;
    }
    if (err != NHP_OK) {
        cfg_doc_free(&doc);
        return err;
    }

    app->index_max_age_days =
        cfg_get_long(&doc, NULL, "index_max_age_days", app->index_max_age_days);
    app->request_delay_ms = cfg_get_long(&doc, NULL, "request_delay_ms", app->request_delay_ms);
    app->wayback_fallback = cfg_get_bool(&doc, NULL, "wayback_fallback", app->wayback_fallback);
    app->max_download_mb = cfg_get_long(&doc, NULL, "max_download_mb", app->max_download_mb);

    err = apply_string_override(&app->author_name, cfg_get(&doc, NULL, "author_name", NULL));
    if (err == NHP_OK) {
        err = apply_string_override(&app->ignore, cfg_get(&doc, NULL, "ignore", NULL));
    }
    if (err == NHP_OK) {
        err = apply_string_override(&app->pager, cfg_get(&doc, NULL, "pager", NULL));
    }
    if (err == NHP_OK) {
        err = apply_string_override(&app->editor, cfg_get(&doc, NULL, "editor", NULL));
    }

    cfg_doc_free(&doc);
    return err;
}

nhp_err cfg_app_save(const cfg_app *app)
{
    char *dir = cfg_xdg_config_dir();
    if (dir == NULL) {
        return NHP_ENOMEM;
    }
    nhp_err err = fs_mkdir_p(dir, 0700);
    if (err != NHP_OK) {
        free(dir);
        return err;
    }
    char *path = path_join(dir, "config");
    free(dir);
    if (path == NULL) {
        return NHP_ENOMEM;
    }

    cfg_doc doc;
    cfg_doc_init(&doc);

    char age_buf[32];
    char delay_buf[32];
    char size_buf[32];
    snprintf(age_buf, sizeof(age_buf), "%ld", app->index_max_age_days);
    snprintf(delay_buf, sizeof(delay_buf), "%ld", app->request_delay_ms);
    snprintf(size_buf, sizeof(size_buf), "%ld", app->max_download_mb);

    const char *keys[] = {"index_max_age_days",
                          "request_delay_ms",
                          "wayback_fallback",
                          "max_download_mb",
                          "author_name",
                          "ignore",
                          "pager",
                          "editor"};
    const char *values[] = {age_buf,    delay_buf,        app->wayback_fallback ? "yes" : "no",
                            size_buf,   app->author_name, app->ignore,
                            app->pager, app->editor};

    err = NHP_OK;
    for (size_t i = 0; i < NHP_ARRAY_LEN(keys) && err == NHP_OK; i++) {
        err = cfg_set(&doc, NULL, keys[i], values[i]);
    }
    if (err != NHP_OK) {
        free(path);
        cfg_doc_free(&doc);
        return err;
    }

    err = cfg_doc_save(path, &doc);
    free(path);
    cfg_doc_free(&doc);
    return err;
}

nhp_err cfg_app_add_ignore_pattern(cfg_app *app, const char *pattern)
{
    if (pattern == NULL || pattern[0] == '\0') {
        return NHP_EINVAL;
    }
    size_t pattern_len = strlen(pattern);

    const char *p = (app->ignore != NULL) ? app->ignore : "";
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        const char *start = p;
        while (*p != '\0' && *p != ' ' && *p != '\t') {
            p++;
        }
        size_t tok_len = (size_t)(p - start);
        if (tok_len == pattern_len && strncmp(start, pattern, tok_len) == 0) {
            return NHP_OK; /* already there */
        }
    }

    buf b;
    buf_init(&b);
    if (app->ignore != NULL && app->ignore[0] != '\0') {
        buf_append_str(&b, app->ignore);
        buf_append(&b, " ", 1);
    }
    buf_append_str(&b, pattern);
    char *joined = buf_take(&b);
    if (joined == NULL) {
        return NHP_ENOMEM;
    }
    free(app->ignore);
    app->ignore = joined;

    return cfg_app_save(app);
}

void cfg_app_free(cfg_app *app)
{
    free(app->author_name);
    free(app->ignore);
    free(app->pager);
    free(app->editor);
    app->author_name = NULL;
    app->ignore = NULL;
    app->pager = NULL;
    app->editor = NULL;
}

static char *ui_state_path(void)
{
    char *dir = cfg_xdg_config_dir();
    if (dir == NULL) {
        return NULL;
    }
    char *path = path_join(dir, "ui-state");
    free(dir);
    return path;
}

char *cfg_get_active_instance(void)
{
    char *path = ui_state_path();
    if (path == NULL) {
        return NULL;
    }

    cfg_doc doc;
    cfg_doc_init(&doc);
    nhp_err err = cfg_doc_load(path, &doc);
    free(path);
    if (err != NHP_OK) {
        cfg_doc_free(&doc);
        return NULL;
    }

    const char *value = cfg_get(&doc, NULL, "active_instance", NULL);
    char *result = (value != NULL && value[0] != '\0') ? str_dup(value) : NULL;
    cfg_doc_free(&doc);
    return result;
}

nhp_err cfg_set_active_instance(const char *name)
{
    char *dir = cfg_xdg_config_dir();
    if (dir == NULL) {
        return NHP_ENOMEM;
    }
    nhp_err err = fs_mkdir_p(dir, 0700);
    free(dir);
    if (err != NHP_OK) {
        return err;
    }

    char *path = ui_state_path();
    if (path == NULL) {
        return NHP_ENOMEM;
    }

    cfg_doc doc;
    cfg_doc_init(&doc);
    err = cfg_doc_load(path, &doc);
    if (err != NHP_OK && err != NHP_ENOENT) {
        free(path);
        cfg_doc_free(&doc);
        return err;
    }

    err = cfg_set(&doc, NULL, "active_instance", name);
    if (err == NHP_OK) {
        err = cfg_doc_save(path, &doc);
    }
    free(path);
    cfg_doc_free(&doc);
    return err;
}
