/* test_site.c - unit tests for site.c's HTML parsing (no network, no
 * terminal; runs against saved fixtures in tests/fixtures/).
 * SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "site.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            (void)fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                  \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static char *read_fixture(const char *name, size_t *len)
{
    char *path = path_join("tests/fixtures", name);
    char *data = NULL;
    if (fs_read_file(path, &data, len) != NHP_OK) {
        (void)fprintf(stderr, "FAIL: could not read fixture %s: %s\n", path, err_get());
        failures++;
    }
    free(path);
    return data;
}

static void test_browse_page_real(void)
{
    size_t len;
    char *html = read_fixture("browse_page1.html", &len);
    CHECK(html != NULL);
    if (html == NULL) {
        return;
    }

    site_browse_page page;
    CHECK(site_parse_browse_page(html, len, &page) == NHP_OK);
    free(html);

    CHECK(page.row_count == 20);
    CHECK(page.total_pages == 17);

    /* the plan's own canonical examples (docs/features.md section 7.3),
     * confirmed against the real site. */
    bool found_64bit = false;
    bool found_categories = false;
    for (size_t i = 0; i < page.row_count; i++) {
        const site_row *r = &page.rows[i];
        if (str_eq(r->id, "327")) {
            found_64bit = true;
            CHECK(str_eq(r->name, "64bit 1.0.16"));
            CHECK(str_eq(r->author, "Clive Crous"));
            CHECK(str_eq(r->target, "NetHack 3.4.3"));
            CHECK(str_eq(r->rating, "-"));
            CHECK(str_eq(r->short_description, "Fixes NetHack for 64bit"));
        }
        if (str_eq(r->id, "71")) {
            found_categories = true;
            CHECK(str_eq(r->name, "Additional Item Categories v5.2"));
        }
        if (str_eq(r->id, "340")) {
            CHECK(str_eq(r->rating, "100")); /* "100%" with the % stripped */
        }
    }
    CHECK(found_64bit);
    CHECK(found_categories);

    site_browse_page_free(&page);
}

static void test_browse_page_mojibake(void)
{
    size_t len;
    char *html = read_fixture("browse_page10_mojibake.html", &len);
    CHECK(html != NULL);
    if (html == NULL) {
        return;
    }

    site_browse_page page;
    CHECK(site_parse_browse_page(html, len, &page) == NHP_OK);
    free(html);

    bool found = false;
    for (size_t i = 0; i < page.row_count; i++) {
        if (str_eq(page.rows[i].id, "39")) {
            found = true;
            /* The real page mis-declares UTF-8 as windows-1252; decoding
             * it that way (features.md 7.1) reproduces the site's own
             * "Â've" mojibake rather than silently "fixing" it. */
            CHECK(strstr(page.rows[i].short_description, "you\xc3\x82\xe2\x80\x99ve") != NULL);
            break;
        }
    }
    CHECK(found);

    site_browse_page_free(&page);
}

static void test_browse_page_no_table(void)
{
    size_t len;
    char *html = read_fixture("browse_no_table.html", &len);
    CHECK(html != NULL);
    if (html == NULL) {
        return;
    }

    site_browse_page page;
    nhp_err err = site_parse_browse_page(html, len, &page);
    free(html);

    CHECK(err == NHP_EPARSE);
    CHECK(err_get()[0] != '\0');
}

static void test_browse_page_cloudflare_challenge(void)
{
    size_t len;
    char *html = read_fixture("browse_cloudflare_challenge.html", &len);
    CHECK(html != NULL);
    if (html == NULL) {
        return;
    }

    site_browse_page page;
    nhp_err err = site_parse_browse_page(html, len, &page);
    free(html);

    CHECK(err == NHP_ENET);
}

static void test_browse_page_empty(void)
{
    site_browse_page page;
    nhp_err err = site_parse_browse_page("", 0, &page);
    CHECK(err == NHP_EPARSE);
}

static void test_browse_url(void)
{
    char *u1 = site_browse_url(1);
    char *u2 = site_browse_url(2);
    char *u17 = site_browse_url(17);
    CHECK(strcmp(u1, "https://nhpatchdb.alt.org/?browse") == 0);
    CHECK(strcmp(u2, "https://nhpatchdb.alt.org/?browse&page=1") == 0);
    CHECK(strcmp(u17, "https://nhpatchdb.alt.org/?browse&page=16") == 0);
    free(u1);
    free(u2);
    free(u17);
}

static void test_detail_hosted(void)
{
    size_t len;
    char *html = read_fixture("detail_hosted.html", &len);
    CHECK(html != NULL);
    if (html == NULL) {
        return;
    }

    site_detail d;
    CHECK(site_parse_detail_page(html, len, &d) == NHP_OK);
    free(html);

    CHECK(str_eq(d.name, "(New) Dumplog 1.0.0"));
    CHECK(str_eq(d.rating, "-"));
    CHECK(str_eq(d.author, "Nax"));
    CHECK(str_eq(d.target, "NetHack 3.6.0"));
    CHECK(str_eq(d.description, "A rewrite of the famous dumplog patch for NetHack 3.6.0"));
    CHECK(strstr(d.long_description, "complete rewrite") != NULL);
    CHECK(strchr(d.long_description, '\n') != NULL); /* <br> became \n */
    CHECK(d.download_hosted == true);
    CHECK(str_eq(d.download_url, "https://nhpatchdb.alt.org/?download=485"));
    CHECK(str_eq(d.added, "December 22, 2016 22:52"));
    CHECK(d.changed == NULL);

    site_detail_free(&d);
}

static void test_detail_external_with_comments(void)
{
    size_t len;
    char *html = read_fixture("detail_external_with_comments.html", &len);
    CHECK(html != NULL);
    if (html == NULL) {
        return;
    }

    site_detail d;
    CHECK(site_parse_detail_page(html, len, &d) == NHP_OK);
    free(html);

    CHECK(str_eq(d.name, "messagetype option v0.2"));
    CHECK(str_eq(d.rating, "100"));
    CHECK(str_eq(d.author, "Pasi Kallinen"));
    CHECK(d.download_hosted == false);
    CHECK(str_eq(d.download_url, "https://bilious.alt.org/~paxed/nethack/nh343-msgtype.diff"));
    CHECK(str_eq(d.added, "September 22, 2010 11:09"));
    CHECK(d.changed != NULL && str_eq(d.changed, "January 12, 2011 20:16"));
    /* the comments table after </table class="patchshow"> must not leak
     * into any field. */
    CHECK(strstr(d.long_description, "scummos") == NULL);
    CHECK(strstr(d.description, "scummos") == NULL);

    site_detail_free(&d);
}

static void test_detail_url_helpers(void)
{
    char *detail = site_detail_url("485");
    char *download = site_download_url("485");
    CHECK(strcmp(detail, "https://nhpatchdb.alt.org/?485") == 0);
    CHECK(strcmp(download, "https://nhpatchdb.alt.org/?download=485") == 0);
    free(detail);
    free(download);
}

static void check_split(const char *combined, const char *expect_name, const char *expect_version)
{
    char *name = NULL;
    char *version = NULL;
    CHECK(site_split_name_version(combined, &name, &version) == NHP_OK);
    CHECK(name != NULL && strcmp(name, expect_name) == 0);
    CHECK(version != NULL && strcmp(version, expect_version) == 0);
    free(name);
    free(version);
}

static void test_split_name_version(void)
{
    /* every example from docs/features.md section 7.3, confirmed against
     * real entries on the site. */
    check_split("(New) Dumplog 1.0.0", "(New) Dumplog", "1.0.0");
    check_split("Additional Item Categories v5.2", "Additional Item Categories", "v5.2");
    check_split("Building V1.1", "Building", "V1.1");
    check_split("initpoly", "initpoly", "unversioned");
    check_split("Coloured Walls and Floor v2", "Coloured Walls and Floor", "v2");
}

int main(void)
{
    test_browse_page_real();
    test_browse_page_mojibake();
    test_browse_page_no_table();
    test_browse_page_cloudflare_challenge();
    test_browse_page_empty();
    test_browse_url();
    test_detail_hosted();
    test_detail_external_with_comments();
    test_detail_url_helpers();
    test_split_name_version();

    if (failures == 0) {
        (void)printf("ok: all site tests passed\n");
    } else {
        (void)printf("FAILED: %d check(s) failed\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
