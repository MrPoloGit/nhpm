/* test_version.c - unit tests for version.c (no network, no terminal).
 * SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "version.h"

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

static void test_parse_nethack_version(void)
{
    ver_version v;
    CHECK(ver_parse("NetHack 3.6.0", &v) == NHP_OK);
    CHECK(v.major == 3 && v.minor == 6 && v.patch == 0);
    CHECK(!v.is_variant);
    char *fmt = ver_format(&v);
    CHECK(strcmp(fmt, "3.6.0") == 0);
    free(fmt);
    ver_free(&v);
}

static void test_parse_bare_number_is_not_a_variant(void)
{
    /* an instance's own stored version, e.g. "source = 3.6.7" in
     * instance.conf, has no "NetHack" prefix but is still vanilla. */
    ver_version v;
    CHECK(ver_parse("3.6.7", &v) == NHP_OK);
    CHECK(v.major == 3 && v.minor == 6 && v.patch == 7);
    CHECK(!v.is_variant);
    ver_free(&v);
}

static void test_parse_two_part_version(void)
{
    ver_version v;
    CHECK(ver_parse("NetHack 3.6", &v) == NHP_OK);
    CHECK(v.major == 3 && v.minor == 6 && v.patch == 0);
    ver_free(&v);
}

static void test_parse_variants(void)
{
    ver_version v;
    CHECK(ver_parse("SLASH'EM 0.0.7E7", &v) == NHP_OK);
    CHECK(v.is_variant);
    CHECK(v.major == -1);
    ver_free(&v);

    CHECK(ver_parse("UnNetHack 5.1.0", &v) == NHP_OK);
    CHECK(v.is_variant);
    ver_free(&v);

    CHECK(ver_parse("garbage", &v) == NHP_OK);
    CHECK(v.is_variant);
    CHECK(v.major == -1);
    ver_free(&v);
}

static void check_patchlevel(const char *dir, int major, int minor, int patch)
{
    ver_version v;
    CHECK(ver_read_patchlevel(dir, &v) == NHP_OK);
    CHECK(v.major == major);
    CHECK(v.minor == minor);
    CHECK(v.patch == patch);
    ver_free(&v);
}

static void test_read_patchlevel(void)
{
    check_patchlevel("tests/fixtures/source343", 3, 4, 3);
    check_patchlevel("tests/fixtures/source367", 3, 6, 7);
    check_patchlevel("tests/fixtures/source37", 3, 7, 0);
}

static void test_read_patchlevel_missing(void)
{
    ver_version v;
    CHECK(ver_read_patchlevel("tests/fixtures/does-not-exist", &v) == NHP_ENOENT);
    CHECK(err_get()[0] != '\0');
}

static void check_compat(const char *inst, const char *site, char expect_mark)
{
    ver_version vi, vs;
    ver_parse(inst, &vi);
    ver_parse(site, &vs);
    char mark = ver_compat_mark(ver_compatibility(&vi, &vs));
    if (mark != expect_mark) {
        (void)fprintf(stderr, "FAIL: inst=%s site=%s got=%c want=%c\n", inst, site, mark,
                      expect_mark);
        failures++;
    }
    ver_free(&vi);
    ver_free(&vs);
}

static void test_compatibility_marks(void)
{
    /* docs/features.md section 9.4's own table, instance on 3.6.7 */
    check_compat("3.6.7", "NetHack 3.6.7", '=');
    check_compat("3.6.7", "NetHack 3.6.0", '~');
    check_compat("3.6.7", "NetHack 3.4.3", '!');
    check_compat("3.6.7", "SLASH'EM", '?');
}

int main(void)
{
    test_parse_nethack_version();
    test_parse_bare_number_is_not_a_variant();
    test_parse_two_part_version();
    test_parse_variants();
    test_read_patchlevel();
    test_read_patchlevel_missing();
    test_compatibility_marks();

    if (failures == 0) {
        (void)printf("ok: all version tests passed\n");
    } else {
        (void)printf("FAILED: %d check(s) failed\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
