/* site.h - all knowledge of the website: URLs and XPath expressions.
 * SPDX-License-Identifier: NGPL */
#ifndef NHPM_SITE_H
#define NHPM_SITE_H

#include "util.h"

#include <stddef.h>

/* One row of a browse page. `name` is the combined "name + version" text
 * as-is; split with site_split_name_version() if needed. `rating` is "-"
 * if not rated, else a bare number like "100" (no "%"). */
typedef struct site_row {
    char *id;
    char *name;
    char *author;
    char *target;
    char *rating;
    char *short_description;
} site_row;

typedef struct site_browse_page {
    site_row *rows;
    size_t row_count;
    size_t total_pages; /* from the page selector; 0 if the page had none */
} site_browse_page;

void site_browse_page_free(site_browse_page *page);

/* Parses one browse page's HTML (7.3). NHP_ENET if it looks like a
 * Cloudflare challenge; NHP_EPARSE if the expected table is missing
 * (layout may have changed). `html` must be NUL-terminated at html[html_len]. */
nhp_err site_parse_browse_page(const char *html, size_t html_len, site_browse_page *out);

/* The URL for browse page `page_number` (1-based; page 1 is "/?browse"). */
char *site_browse_url(size_t page_number);

typedef struct site_detail {
    char *name;
    char *rating;
    char *author;
    char *target;
    char *description;
    char *long_description; /* "" if the page has none; "\n" for each <br> */
    bool download_hosted;
    char *download_url; /* always absolute */
    char *added;        /* raw text, e.g. "December 22, 2016 22:52" */
    char *changed;      /* NULL if the page has no Changed row */
} site_detail;

void site_detail_free(site_detail *detail);

/* Parses one patch's detail page. Same error cases as
 * site_parse_browse_page(), plus NHP_EPARSE if there's no Download row. */
nhp_err site_parse_detail_page(const char *html, size_t html_len, site_detail *out);

char *site_detail_url(const char *id);
char *site_download_url(const char *id);

/* Splits "Name Version" (e.g. "(New) Dumplog 1.0.0") into *out_name and
 * *out_version. If the last word doesn't look like a version, *out_name
 * is a copy of `combined` and *out_version is "unversioned" (7.3). */
nhp_err site_split_name_version(const char *combined, char **out_name, char **out_version);

#endif /* NHPM_SITE_H */
