/* index.c - index.tsv, local search and filters.
 * SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "index.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "net.h"
#include "site.h"

/* ---- idx_index ---- */

void idx_init(idx_index *idx)
{
    idx->entries = NULL;
    idx->count = 0;
    idx->cap = 0;
}

static void idx_entry_free_contents(idx_entry *e)
{
    free(e->id);
    free(e->name);
    free(e->author);
    free(e->target);
    free(e->rating);
    free(e->short_description);
}

void idx_free(idx_index *idx)
{
    for (size_t i = 0; i < idx->count; i++) {
        idx_entry_free_contents(&idx->entries[i]);
    }
    free(idx->entries);
    idx->entries = NULL;
    idx->count = 0;
    idx->cap = 0;
}

static bool idx_append(idx_index *idx, const idx_entry *entry)
{
    if (idx->count == idx->cap) {
        size_t new_cap = (idx->cap == 0) ? 64 : idx->cap * 2;
        idx_entry *grown = realloc(idx->entries, new_cap * sizeof(*grown));
        if (grown == NULL) {
            return false;
        }
        idx->entries = grown;
        idx->cap = new_cap;
    }
    idx->entries[idx->count++] = *entry;
    return true;
}

/* ---- TSV escaping (docs/file-formats.md: tabs, newlines, backslashes
 * inside fields escaped as \t \n \\, so splitting a row on raw tab bytes is
 * always safe) ---- */

static char *tsv_escape(const char *s)
{
    buf b;
    buf_init(&b);
    for (const char *p = s; *p != '\0'; p++) {
        switch (*p) {
            case '\t':
                buf_append_str(&b, "\\t");
                break;
            case '\n':
                buf_append_str(&b, "\\n");
                break;
            case '\\':
                buf_append_str(&b, "\\\\");
                break;
            default:
                buf_append(&b, p, 1);
                break;
        }
    }
    return buf_take(&b);
}

static char *tsv_unescape(const char *s)
{
    buf b;
    buf_init(&b);
    for (const char *p = s; *p != '\0'; p++) {
        if (*p == '\\' && p[1] != '\0') {
            p++;
            switch (*p) {
                case 't':
                    buf_append_str(&b, "\t");
                    break;
                case 'n':
                    buf_append_str(&b, "\n");
                    break;
                case '\\':
                    buf_append_str(&b, "\\");
                    break;
                default:
                    buf_append(&b, p, 1);
                    break;
            }
        } else {
            buf_append(&b, p, 1);
        }
    }
    return buf_take(&b);
}

/* ---- index.tsv ---- */

static char *index_tsv_path(void)
{
    char *dir = cfg_xdg_cache_dir();
    if (dir == NULL) {
        return NULL;
    }
    char *path = path_join(dir, "index.tsv");
    free(dir);
    return path;
}

nhp_err idx_save(const idx_index *idx)
{
    char *dir = cfg_xdg_cache_dir();
    if (dir == NULL) {
        return NHP_ENOMEM;
    }
    nhp_err err = fs_mkdir_p(dir, 0700);
    free(dir);
    if (err != NHP_OK) {
        return err;
    }

    char *path = index_tsv_path();
    if (path == NULL) {
        return NHP_ENOMEM;
    }

    buf b;
    buf_init(&b);
    buf_append_str(&b, "id\tname\tauthor\ttarget\trating\tshort_description\n");

    for (size_t i = 0; i < idx->count && err == NHP_OK; i++) {
        const idx_entry *e = &idx->entries[i];
        char *id = tsv_escape(e->id);
        char *name = tsv_escape(e->name);
        char *author = tsv_escape(e->author);
        char *target = tsv_escape(e->target);
        char *rating = tsv_escape(e->rating);
        char *desc = tsv_escape(e->short_description);
        if (id == NULL || name == NULL || author == NULL || target == NULL || rating == NULL ||
            desc == NULL ||
            !buf_append_fmt(&b, "%s\t%s\t%s\t%s\t%s\t%s\n", id, name, author, target, rating,
                            desc)) {
            err = NHP_ENOMEM;
        }
        free(id);
        free(name);
        free(author);
        free(target);
        free(rating);
        free(desc);
    }

    if (err == NHP_OK) {
        char *text = buf_take(&b);
        err = fs_write_atomic(path, text, strlen(text));
        free(text);
    } else {
        buf_free(&b);
    }
    free(path);
    return err;
}

static nhp_err idx_parse_line(const char *line, idx_entry *out)
{
    const char *fields_start[6];
    size_t fields_len[6];
    const char *p = line;
    for (size_t i = 0; i < 6; i++) {
        fields_start[i] = p;
        const char *tab = (i < 5) ? strchr(p, '\t') : NULL;
        if (tab == NULL && i < 5) {
            err_set("index.tsv: a row has fewer than 6 tab-separated fields");
            return NHP_EPARSE;
        }
        fields_len[i] = tab ? (size_t)(tab - p) : strlen(p);
        p = tab ? tab + 1 : p + fields_len[i];
    }

    char *raw[6];
    for (size_t i = 0; i < 6; i++) {
        raw[i] = str_ndup(fields_start[i], fields_len[i]);
        if (raw[i] == NULL) {
            for (size_t j = 0; j < i; j++) {
                free(raw[j]);
            }
            return NHP_ENOMEM;
        }
    }

    out->id = tsv_unescape(raw[0]);
    out->name = tsv_unescape(raw[1]);
    out->author = tsv_unescape(raw[2]);
    out->target = tsv_unescape(raw[3]);
    out->rating = tsv_unescape(raw[4]);
    out->short_description = tsv_unescape(raw[5]);
    for (size_t i = 0; i < 6; i++) {
        free(raw[i]);
    }

    if (out->id == NULL || out->name == NULL || out->author == NULL || out->target == NULL ||
        out->rating == NULL || out->short_description == NULL) {
        idx_entry_free_contents(out);
        return NHP_ENOMEM;
    }
    return NHP_OK;
}

nhp_err idx_load(idx_index *idx)
{
    char *path = index_tsv_path();
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

    idx_free(idx);
    idx_init(idx);

    const char *p = text;
    bool first_line = true;
    while (*p != '\0' && err == NHP_OK) {
        const char *line_start = p;
        while (*p != '\0' && *p != '\n') {
            p++;
        }
        size_t line_len = (size_t)(p - line_start);
        if (*p == '\n') {
            p++;
        }

        if (first_line) {
            first_line = false;
            continue;
        }
        if (line_len == 0) {
            continue;
        }

        char *line = str_ndup(line_start, line_len);
        if (line == NULL) {
            err = NHP_ENOMEM;
            break;
        }

        idx_entry entry = {0};
        err = idx_parse_line(line, &entry);
        free(line);
        if (err != NHP_OK) {
            break;
        }
        if (!idx_append(idx, &entry)) {
            idx_entry_free_contents(&entry);
            err = NHP_ENOMEM;
        }
    }

    free(text);
    if (err != NHP_OK) {
        idx_free(idx);
    }
    return err;
}

/* ---- index.meta ---- */

static char *index_meta_path(void)
{
    char *dir = cfg_xdg_cache_dir();
    if (dir == NULL) {
        return NULL;
    }
    char *path = path_join(dir, "index.meta");
    free(dir);
    return path;
}

static nhp_err idx_save_meta(void)
{
    char *dir = cfg_xdg_cache_dir();
    if (dir == NULL) {
        return NHP_ENOMEM;
    }
    nhp_err err = fs_mkdir_p(dir, 0700);
    free(dir);
    if (err != NHP_OK) {
        return err;
    }

    char *path = index_meta_path();
    if (path == NULL) {
        return NHP_ENOMEM;
    }

    time_t now = time(NULL);
    struct tm tm_utc;
    (void)gmtime_r(&now, &tm_utc);
    char stamp[32];
    (void)strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);

    cfg_doc doc;
    cfg_doc_init(&doc);
    err = cfg_set(&doc, NULL, "format", "1");
    if (err == NHP_OK) {
        err = cfg_set(&doc, NULL, "refreshed", stamp);
    }
    if (err == NHP_OK) {
        err = cfg_doc_save(path, &doc);
    }
    cfg_doc_free(&doc);
    free(path);
    return err;
}

/* Howard Hinnant's days-from-civil-date algorithm (proleptic Gregorian);
 * avoids depending on timegm()/strptime(), which aren't guaranteed under
 * strict POSIX feature-test macros on every platform. */
static long days_from_civil(long y, int m, int d)
{
    y -= (m <= 2) ? 1 : 0;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static bool parse_iso8601_utc(const char *s, time_t *out)
{
    int year, month, day, hour, min, sec;
    if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2dZ", &year, &month, &day, &hour, &min, &sec) != 6) {
        return false;
    }
    long days = days_from_civil(year, month, day);
    *out = (time_t)days * 86400 + (time_t)hour * 3600 + (time_t)min * 60 + (time_t)sec;
    return true;
}

long idx_age_seconds(void)
{
    char *path = index_meta_path();
    if (path == NULL) {
        return -1;
    }

    cfg_doc doc;
    cfg_doc_init(&doc);
    nhp_err err = cfg_doc_load(path, &doc);
    free(path);
    if (err != NHP_OK) {
        cfg_doc_free(&doc);
        return -1;
    }

    long age = -1;
    const char *stamp = cfg_get(&doc, NULL, "refreshed", NULL);
    time_t then;
    if (stamp != NULL && parse_iso8601_utc(stamp, &then)) {
        time_t now = time(NULL);
        if (now >= then) {
            age = (long)(now - then);
        }
    }
    cfg_doc_free(&doc);
    return age;
}

/* ---- refresh ---- */

static nhp_err append_rows(idx_index *idx, const site_browse_page *page)
{
    for (size_t i = 0; i < page->row_count; i++) {
        const site_row *row = &page->rows[i];
        idx_entry entry;
        entry.id = str_dup(row->id);
        entry.name = str_dup(row->name);
        entry.author = str_dup(row->author);
        entry.target = str_dup(row->target);
        entry.rating = str_dup(row->rating);
        entry.short_description = str_dup(row->short_description);
        if (entry.id == NULL || entry.name == NULL || entry.author == NULL ||
            entry.target == NULL || entry.rating == NULL || entry.short_description == NULL) {
            idx_entry_free_contents(&entry);
            return NHP_ENOMEM;
        }
        if (!idx_append(idx, &entry)) {
            idx_entry_free_contents(&entry);
            return NHP_ENOMEM;
        }
    }
    return NHP_OK;
}

/* Fetches and parses one browse page, appending its rows to *idx. */
static nhp_err fetch_and_append_page(idx_index *idx, size_t page_number, size_t *out_total_pages)
{
    char *url = site_browse_url(page_number);
    if (url == NULL) {
        return NHP_ENOMEM;
    }

    net_resp resp = {0};
    nhp_err err = net_get(url, &resp, NULL, NULL);
    free(url);
    if (err != NHP_OK) {
        net_resp_free(&resp);
        return err;
    }

    site_browse_page page = {0};
    err = site_parse_browse_page(resp.data, resp.len, &page);
    net_resp_free(&resp);
    if (err != NHP_OK) {
        return err;
    }

    err = append_rows(idx, &page);
    if (out_total_pages != NULL) {
        *out_total_pages = page.total_pages;
    }
    site_browse_page_free(&page);
    return err;
}

nhp_err idx_refresh(idx_index *idx, const cfg_app *app, idx_progress_fn progress, void *user_data)
{
    idx_index fresh;
    idx_init(&fresh);

    size_t total_pages = 0;
    nhp_err err = fetch_and_append_page(&fresh, 1, &total_pages);
    if (err != NHP_OK) {
        idx_free(&fresh);
        return err;
    }
    if (progress != NULL) {
        progress(user_data, 1, (total_pages > 0) ? total_pages : 1);
    }

    struct timespec delay = {
        .tv_sec = app->request_delay_ms / 1000,
        .tv_nsec = (app->request_delay_ms % 1000) * 1000000L,
    };

    for (size_t page_number = 2; page_number <= total_pages; page_number++) {
        nanosleep(&delay, NULL);

        err = fetch_and_append_page(&fresh, page_number, NULL);
        if (err != NHP_OK) {
            idx_free(&fresh);
            return err;
        }
        if (progress != NULL) {
            progress(user_data, page_number, total_pages);
        }
    }

    idx_free(idx);
    *idx = fresh;

    err = idx_save(idx);
    if (err != NHP_OK) {
        return err;
    }
    return idx_save_meta();
}

/* ---- search ---- */

nhp_err idx_query_parse(const char *raw, idx_query *out)
{
    memset(out, 0, sizeof(*out));
    out->min_rating = -1;

    char *copy = str_dup(raw);
    if (copy == NULL) {
        return NHP_ENOMEM;
    }

    buf text_buf;
    buf_init(&text_buf);

    char *saveptr = NULL;
    char *token = strtok_r(copy, " \t", &saveptr);
    while (token != NULL) {
        if (strncasecmp(token, "v:", 2) == 0) {
            free(out->target_prefix);
            out->target_prefix = str_dup(token + 2);
        } else if (strncasecmp(token, "a:", 2) == 0) {
            free(out->author_contains);
            out->author_contains = str_dup(token + 2);
        } else if (strncasecmp(token, "id:", 3) == 0) {
            free(out->exact_id);
            out->exact_id = str_dup(token + 3);
        } else if (strncasecmp(token, "r:", 2) == 0) {
            const char *num = token + 2;
            if (num[0] == '>' && num[1] == '=') {
                num += 2;
            }
            char *end = NULL;
            long value = strtol(num, &end, 10);
            if (end != num && *end == '\0') {
                out->min_rating = value;
            }
        } else {
            if (text_buf.len > 0) {
                buf_append_str(&text_buf, " ");
            }
            buf_append_str(&text_buf, token);
        }
        token = strtok_r(NULL, " \t", &saveptr);
    }
    free(copy);

    out->text = buf_take(&text_buf);
    return NHP_OK;
}

void idx_query_free(idx_query *q)
{
    free(q->text);
    free(q->target_prefix);
    free(q->author_contains);
    free(q->exact_id);
    memset(q, 0, sizeof(*q));
}

/* The target's version, e.g. "NetHack 3.6.0" -> "3.6.0" (the part at and
 * after the first digit), for the v: filter. */
static const char *target_version_part(const char *target)
{
    const char *p = target;
    while (*p != '\0' && !isdigit((unsigned char)*p)) {
        p++;
    }
    return p;
}

static bool entry_matches_words(const idx_entry *e, const char *words)
{
    if (words == NULL || words[0] == '\0') {
        return true;
    }
    char *copy = str_dup(words);
    if (copy == NULL) {
        return false;
    }

    bool all_match = true;
    char *saveptr = NULL;
    const char *word = strtok_r(copy, " \t", &saveptr);
    while (word != NULL) {
        bool found = str_contains_ci(e->name, word) || str_contains_ci(e->author, word) ||
                     str_contains_ci(e->short_description, word);
        if (!found) {
            all_match = false;
            break;
        }
        word = strtok_r(NULL, " \t", &saveptr);
    }
    free(copy);
    return all_match;
}

static bool entry_matches_query(const idx_entry *e, const idx_query *q)
{
    if (q->exact_id != NULL && q->exact_id[0] != '\0' && !str_eq(e->id, q->exact_id)) {
        return false;
    }
    if (q->target_prefix != NULL && q->target_prefix[0] != '\0') {
        const char *version = target_version_part(e->target);
        if (strncasecmp(version, q->target_prefix, strlen(q->target_prefix)) != 0) {
            return false;
        }
    }
    if (q->author_contains != NULL && q->author_contains[0] != '\0' &&
        !str_contains_ci(e->author, q->author_contains)) {
        return false;
    }
    if (q->min_rating >= 0) {
        char *end = NULL;
        long rating_value = strtol(e->rating, &end, 10);
        if (end == e->rating || rating_value < q->min_rating) {
            return false;
        }
    }
    return entry_matches_words(e, q->text);
}

nhp_err idx_search(const idx_index *idx, const idx_query *query, const idx_entry ***out,
                   size_t *out_count)
{
    *out = NULL;
    *out_count = 0;

    if (idx->count == 0) {
        return NHP_OK;
    }

    const idx_entry **matches = calloc(idx->count, sizeof(*matches));
    if (matches == NULL) {
        return NHP_ENOMEM;
    }

    size_t n = 0;
    for (size_t i = 0; i < idx->count; i++) {
        if (entry_matches_query(&idx->entries[i], query)) {
            matches[n++] = &idx->entries[i];
        }
    }

    *out = matches;
    *out_count = n;
    return NHP_OK;
}
