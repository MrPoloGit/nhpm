/* test_index.c - unit tests for index.c's TSV reader/writer and local
 * search (no network, no terminal).
 * SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "index.h"
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

/* idx_index has no public "append one entry" function (idx_refresh builds
 * it internally from parsed site rows), so tests build a fixture index
 * directly from the struct instead. */
static void set_entry(idx_entry *e, const char *id, const char *name, const char *author,
                      const char *target, const char *rating, const char *desc)
{
    e->id = str_dup(id);
    e->name = str_dup(name);
    e->author = str_dup(author);
    e->target = str_dup(target);
    e->rating = str_dup(rating);
    e->short_description = str_dup(desc);
}

static void build_fixture_index(idx_index *idx)
{
    idx_init(idx);
    idx->entries = calloc(3, sizeof(idx_entry));
    idx->cap = 3;
    idx->count = 3;
    set_entry(&idx->entries[0], "485", "(New) Dumplog 1.0.0", "Nax", "NetHack 3.6.0", "-",
              "A rewrite of the famous dumplog patch for NetHack 3.6.0");
    set_entry(&idx->entries[1], "397", "messagetype option v0.2", "Pasi Kallinen", "NetHack 3.4.3",
              "100", "Add a new configuration option MSGTYPE");
    set_entry(&idx->entries[2], "999", "tab\tand\nnewline and \\backslash", "Weird\tAuthor",
              "NetHack 3.6.7", "-", "a field with a literal tab\t, newline\n, and \\backslash");
}

/* Isolates the real XDG_CACHE_HOME-based idx_load/idx_save/idx_age_seconds
 * under a fresh temp dir for the duration of `body`. */
#define WITH_TEMP_CACHE(body)                                                                      \
    do {                                                                                           \
        char *tmp_cache_dir = fs_make_temp_dir("nhp-test-cache");                                  \
        CHECK(tmp_cache_dir != NULL);                                                              \
        setenv("XDG_CACHE_HOME", tmp_cache_dir, 1);                                                \
        body;                                                                                      \
        unsetenv("XDG_CACHE_HOME");                                                                \
        fs_remove_tree(tmp_cache_dir);                                                             \
        free(tmp_cache_dir);                                                                       \
    } while (0)

static void test_tsv_round_trip(void)
{
    WITH_TEMP_CACHE({
        idx_index idx;
        build_fixture_index(&idx);

        CHECK(idx_save(&idx) == NHP_OK);

        idx_index loaded;
        idx_init(&loaded);
        CHECK(idx_load(&loaded) == NHP_OK);
        CHECK(loaded.count == 3);

        for (size_t i = 0; i < loaded.count && i < idx.count; i++) {
            CHECK(str_eq(loaded.entries[i].id, idx.entries[i].id));
            CHECK(str_eq(loaded.entries[i].name, idx.entries[i].name));
            CHECK(str_eq(loaded.entries[i].author, idx.entries[i].author));
            CHECK(str_eq(loaded.entries[i].target, idx.entries[i].target));
            CHECK(str_eq(loaded.entries[i].rating, idx.entries[i].rating));
            CHECK(str_eq(loaded.entries[i].short_description, idx.entries[i].short_description));
        }

        /* the entry with embedded tabs/newlines/backslashes must survive
         * the round trip exactly (docs/file-formats.md's TSV escaping). */
        CHECK(str_eq(loaded.entries[2].name, "tab\tand\nnewline and \\backslash"));
        CHECK(str_eq(loaded.entries[2].author, "Weird\tAuthor"));

        idx_free(&idx);
        idx_free(&loaded);
    });
}

static void test_load_missing_index(void)
{
    WITH_TEMP_CACHE({
        idx_index idx;
        idx_init(&idx);
        CHECK(idx_load(&idx) == NHP_ENOENT);
        idx_free(&idx);
    });
}

static void test_age_unknown_without_meta(void)
{
    WITH_TEMP_CACHE({
        idx_index idx;
        build_fixture_index(&idx);
        CHECK(idx_save(&idx) == NHP_OK); /* writes index.tsv, not index.meta */
        CHECK(idx_age_seconds() == -1);
        idx_free(&idx);
    });
}

static void test_query_parse(void)
{
    idx_query q;
    CHECK(idx_query_parse("v:3.6 a:nax r:>=80 id:485 candle", &q) == NHP_OK);
    CHECK(str_eq(q.target_prefix, "3.6"));
    CHECK(str_eq(q.author_contains, "nax"));
    CHECK(q.min_rating == 80);
    CHECK(str_eq(q.exact_id, "485"));
    CHECK(str_eq(q.text, "candle"));
    idx_query_free(&q);

    idx_query q2;
    CHECK(idx_query_parse("", &q2) == NHP_OK);
    CHECK(q2.target_prefix == NULL);
    CHECK(q2.author_contains == NULL);
    CHECK(q2.exact_id == NULL);
    CHECK(q2.min_rating == -1);
    CHECK(str_eq(q2.text, ""));
    idx_query_free(&q2);

    idx_query q3;
    CHECK(idx_query_parse("r:90", &q3) == NHP_OK); /* no ">=" prefix also parses */
    CHECK(q3.min_rating == 90);
    idx_query_free(&q3);
}

static void test_search_tokens(void)
{
    idx_index idx;
    build_fixture_index(&idx);

    const idx_entry **matches;
    size_t count;
    idx_query q;

    /* plain words: all must match name/author/description, case-insensitive */
    CHECK(idx_query_parse("dumplog", &q) == NHP_OK);
    CHECK(idx_search(&idx, &q, &matches, &count) == NHP_OK);
    CHECK(count == 1 && str_eq(matches[0]->id, "485"));
    free(matches);
    idx_query_free(&q);

    CHECK(idx_query_parse("NAX", &q) == NHP_OK); /* case-insensitive */
    CHECK(idx_search(&idx, &q, &matches, &count) == NHP_OK);
    CHECK(count == 1 && str_eq(matches[0]->id, "485"));
    free(matches);
    idx_query_free(&q);

    /* v: filters on the target's version part, ignoring "NetHack " */
    CHECK(idx_query_parse("v:3.4", &q) == NHP_OK);
    CHECK(idx_search(&idx, &q, &matches, &count) == NHP_OK);
    CHECK(count == 1 && str_eq(matches[0]->id, "397"));
    free(matches);
    idx_query_free(&q);

    /* a: */
    CHECK(idx_query_parse("a:kallinen", &q) == NHP_OK);
    CHECK(idx_search(&idx, &q, &matches, &count) == NHP_OK);
    CHECK(count == 1 && str_eq(matches[0]->id, "397"));
    free(matches);
    idx_query_free(&q);

    /* r:>=N excludes "-" (not rated) entries */
    CHECK(idx_query_parse("r:>=1", &q) == NHP_OK);
    CHECK(idx_search(&idx, &q, &matches, &count) == NHP_OK);
    CHECK(count == 1 && str_eq(matches[0]->id, "397"));
    free(matches);
    idx_query_free(&q);

    /* id: exact match */
    CHECK(idx_query_parse("id:999", &q) == NHP_OK);
    CHECK(idx_search(&idx, &q, &matches, &count) == NHP_OK);
    CHECK(count == 1 && str_eq(matches[0]->id, "999"));
    free(matches);
    idx_query_free(&q);

    /* combining tokens ANDs them */
    CHECK(idx_query_parse("v:3.6 a:nax", &q) == NHP_OK);
    CHECK(idx_search(&idx, &q, &matches, &count) == NHP_OK);
    CHECK(count == 1 && str_eq(matches[0]->id, "485"));
    free(matches);
    idx_query_free(&q);

    CHECK(idx_query_parse("v:3.6 a:kallinen", &q) == NHP_OK); /* no entry matches both */
    CHECK(idx_search(&idx, &q, &matches, &count) == NHP_OK);
    CHECK(count == 0);
    free(matches);
    idx_query_free(&q);

    /* empty query matches everything */
    CHECK(idx_query_parse("", &q) == NHP_OK);
    CHECK(idx_search(&idx, &q, &matches, &count) == NHP_OK);
    CHECK(count == 3);
    free(matches);
    idx_query_free(&q);

    idx_free(&idx);
}

int main(void)
{
    test_tsv_round_trip();
    test_load_missing_index();
    test_age_unknown_without_meta();
    test_query_parse();
    test_search_tokens();

    if (failures == 0) {
        (void)printf("ok: all index tests passed\n");
    } else {
        (void)printf("FAILED: %d check(s) failed\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
