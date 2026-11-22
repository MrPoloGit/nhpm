/* index.h - index.tsv, local search and filters.
 * SPDX-License-Identifier: NGPL */
#ifndef NHPM_INDEX_H
#define NHPM_INDEX_H

#include "config.h"
#include "util.h"

#include <stddef.h>

/* One patch as listed on the site. `name` is the combined "name + version"
 * text (see site.h); `rating` is "-" or a bare number like "100". */
typedef struct idx_entry {
    char *id;
    char *name;
    char *author;
    char *target;
    char *rating;
    char *short_description;
} idx_entry;

typedef struct idx_index {
    idx_entry *entries;
    size_t count;
    size_t cap;
} idx_index;

void idx_init(idx_index *idx);
void idx_free(idx_index *idx);

/* ~/.cache/nhpm/index.tsv. NHP_ENOENT if it hasn't been fetched yet. */
nhp_err idx_load(idx_index *idx);
nhp_err idx_save(const idx_index *idx);

/* Seconds since the index was last refreshed (index.meta); -1 if that's
 * unknown (no successful refresh yet, or the file is missing/unreadable) --
 * callers should treat unknown the same as "very old". */
long idx_age_seconds(void);

/* Called once per browse page fetched during idx_refresh(), 1-based. */
typedef void (*idx_progress_fn)(void *user_data, size_t page, size_t total_pages);

/* Fetches every browse page (one request at a time, request_delay_ms apart,
 * docs/features.md 7.5), replaces *idx, writes index.tsv/.meta. All-or-nothing:
 * on failure partway through, *idx and the on-disk files are untouched. */
nhp_err idx_refresh(idx_index *idx, const cfg_app *app, idx_progress_fn progress, void *user_data);

/* nhpm's search syntax (docs/features.md 13.10). `min_rating` is -1 if no
 * r: token. lib:/in: aren't applied here -- library.c/instance.c layer them on
 * top of idx_search(). */
typedef struct idx_query {
    char *text;            /* plain words; all must match name/author/description */
    char *target_prefix;   /* v: */
    char *author_contains; /* a: */
    long min_rating;       /* r:>=N */
    char *exact_id;        /* id: */
} idx_query;

nhp_err idx_query_parse(const char *raw, idx_query *out);
void idx_query_free(idx_query *q);

/* Entries matching `query`, in original order. *out is a newly allocated
 * array of pointers (free() it, not the pointees) into *idx's entries,
 * owned by *idx and invalidated by any call that changes it. */
nhp_err idx_search(const idx_index *idx, const idx_query *query, const idx_entry ***out,
                   size_t *out_count);

#endif /* NHPM_INDEX_H */
