/* test_util.c - unit tests for util.c (no network, no terminal).
 * SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "util.h"

#include <stdint.h>
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

static void test_str_helpers(void)
{
    char *s = str_dup("hello");
    CHECK(s != NULL && strcmp(s, "hello") == 0);
    free(s);

    CHECK(str_dup(NULL) == NULL);

    char padded[] = "  padded  ";
    CHECK(strcmp(str_trim(padded), "padded") == 0);

    char not_padded[] = "no-pad";
    CHECK(strcmp(str_trim(not_padded), "no-pad") == 0);

    char all_space[] = "   ";
    CHECK(strcmp(str_trim(all_space), "") == 0);

    CHECK(str_eq("a", "a"));
    CHECK(!str_eq("a", "b"));
    CHECK(str_eq(NULL, NULL));
    CHECK(!str_eq(NULL, "a"));
}

static void test_str_replace_all(void)
{
    char *r = str_replace_all("make install PREFIX=$PREFIX", "$PREFIX", "/opt/x");
    CHECK(r != NULL && str_eq(r, "make install PREFIX=/opt/x"));
    free(r);

    /* several occurrences, non-overlapping, left to right */
    r = str_replace_all("$PREFIX/a;$PREFIX/b", "$PREFIX", "/x");
    CHECK(r != NULL && str_eq(r, "/x/a;/x/b"));
    free(r);

    /* no match: unchanged copy */
    r = str_replace_all("nothing here", "$PREFIX", "/x");
    CHECK(r != NULL && str_eq(r, "nothing here"));
    free(r);

    /* empty needle: unchanged copy, not an infinite loop */
    r = str_replace_all("abc", "", "/x");
    CHECK(r != NULL && str_eq(r, "abc"));
    free(r);

    CHECK(str_replace_all(NULL, "$PREFIX", "/x") == NULL);
}

static void test_str_split_whitespace(void)
{
    char **tokens = NULL;
    size_t count = 0;

    CHECK(str_split_whitespace("a  b\tc", &tokens, &count) == NHP_OK);
    CHECK(count == 3);
    CHECK(str_eq(tokens[0], "a") && str_eq(tokens[1], "b") && str_eq(tokens[2], "c"));
    str_tokens_free(tokens, count);

    tokens = NULL;
    count = SIZE_MAX;
    CHECK(str_split_whitespace("", &tokens, &count) == NHP_OK);
    CHECK(tokens == NULL && count == 0);
    str_tokens_free(tokens, count);

    tokens = NULL;
    count = SIZE_MAX;
    CHECK(str_split_whitespace(NULL, &tokens, &count) == NHP_OK);
    CHECK(tokens == NULL && count == 0);
}

static void test_str_matches_all_words(void)
{
    const char *fields[] = {"367-colorful", "nethack-3.6-linux", "streaming setup"};

    CHECK(str_matches_all_words("", fields, 3));    /* empty filter: always matches */
    CHECK(str_matches_all_words(NULL, fields, 3));  /* NULL filter: same */
    CHECK(str_matches_all_words("color", fields, 3));
    CHECK(str_matches_all_words("COLOR", fields, 3)); /* case-insensitive */
    CHECK(str_matches_all_words("linux stream", fields, 3)); /* each word, any field */
    CHECK(!str_matches_all_words("linux nonexistentword", fields, 3)); /* one word fails */
    CHECK(!str_matches_all_words("zzz", fields, 3));

    const char *with_null[] = {NULL, "second"};
    CHECK(str_matches_all_words("second", with_null, 2)); /* a NULL field is skipped, not a crash */
    CHECK(!str_matches_all_words("first", with_null, 2));
}

static void test_buf(void)
{
    buf b;
    buf_init(&b);
    CHECK(buf_append_str(&b, "hello "));
    CHECK(buf_append_fmt(&b, "%s %d", "world", 42));
    char *s = buf_take(&b);
    CHECK(strcmp(s, "hello world 42") == 0);
    free(s);
    buf_free(&b);

    buf empty;
    buf_init(&empty);
    char *taken = buf_take(&empty);
    CHECK(taken != NULL && taken[0] == '\0');
    free(taken);
}

static void test_path_join(void)
{
    char *p1 = path_join("a", "b");
    CHECK(p1 != NULL && strcmp(p1, "a/b") == 0);
    free(p1);

    char *p2 = path_join("a/", "b");
    CHECK(p2 != NULL && strcmp(p2, "a/b") == 0);
    free(p2);

    char *p3 = path_join("a", "/b");
    CHECK(p3 != NULL && strcmp(p3, "a/b") == 0);
    free(p3);

    char *p4 = path_join("", "b");
    CHECK(p4 != NULL && strcmp(p4, "b") == 0);
    free(p4);
}

static void test_path_is_safe(void)
{
    CHECK(path_is_safe("src/foo.c"));
    CHECK(path_is_safe("foo"));
    CHECK(!path_is_safe("../etc/passwd"));
    CHECK(!path_is_safe("/usr/include/x.h"));
    CHECK(!path_is_safe("a/src/../../x"));
    CHECK(!path_is_safe(""));
    CHECK(!path_is_safe(NULL));
}

static void test_fs_atomic(void)
{
    char *dir = fs_make_temp_dir("nhp-test");
    CHECK(dir != NULL);

    char *file = path_join(dir, "f.txt");
    CHECK(fs_write_atomic(file, "hello", 5) == NHP_OK);

    char *data = NULL;
    size_t len = 0;
    CHECK(fs_read_file(file, &data, &len) == NHP_OK);
    CHECK(len == 5 && memcmp(data, "hello", 5) == 0);
    free(data);
    data = NULL;

    CHECK(fs_write_atomic(file, "updated!", 8) == NHP_OK);
    CHECK(fs_read_file(file, &data, &len) == NHP_OK);
    CHECK(len == 8 && memcmp(data, "updated!", 8) == 0);
    free(data);

    CHECK(fs_read_file("/does/not/exist", &data, &len) == NHP_ENOENT);

    free(file);
    CHECK(fs_remove_tree(dir) == NHP_OK);
    CHECK(!fs_exists(dir));
    free(dir);
}

static void test_fs_mkdir_p(void)
{
    char *root = fs_make_temp_dir("nhp-test");
    CHECK(root != NULL);

    char *nested = path_join(root, "a/b/c");
    CHECK(fs_mkdir_p(nested, 0700) == NHP_OK);
    CHECK(fs_exists(nested));
    CHECK(fs_mkdir_p(nested, 0700) == NHP_OK); /* already exists: still OK */
    free(nested);

    CHECK(fs_remove_tree(root) == NHP_OK);
    free(root);
}

static void test_err(void)
{
    err_clear();
    CHECK(strcmp(err_get(), "") == 0);
    err_set("something %s", "broke");
    CHECK(strcmp(err_get(), "something broke") == 0);
}

static void check_sha256(const char *input, size_t len, const char *expect)
{
    char hex[65];
    sha256_hex(input, len, hex);
    CHECK(strcmp(hex, expect) == 0);
}

static void test_sha256(void)
{
    /* the standard empty/"abc" vectors, the two-block NIST vector, and
     * every block-boundary length (55/56/63/64/65/120 bytes) to exercise
     * the padding logic right where it's easiest to get off by one. */
    check_sha256("", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check_sha256("abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    check_sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
                 "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    char fill[121];
    for (size_t n = 55; n <= 65; n++) {
        memset(fill, 'x', n);
        char hex[65];
        sha256_hex(fill, n, hex);
        CHECK(strlen(hex) == 64);
    }

    memset(fill, '1', 55);
    check_sha256(fill, 55, "31f713cc40a632a9ce454b3029b727e9bb6d0eea11460f1d7818852c2565b9cd");
    memset(fill, '2', 56);
    check_sha256(fill, 56, "a049ddf2a81794fba471d423f0c9d654d68378cf70a175404ef96225d700c4e4");
    memset(fill, '3', 63);
    check_sha256(fill, 63, "a579f8c8f994667cdb3a0b3a25bbe4f817725a107bdec4f81776f8eacafdad02");
    memset(fill, '4', 64);
    check_sha256(fill, 64, "f435f2420b50c70ec80865cf8e20b169874165fb8576b48633caf2a8176d2e4a");
    memset(fill, '6', 120);
    check_sha256(fill, 120, "f4d6dbee3b717ceef4c1dbd94c47556b6f840c513829e43f83f3f11d4ac69874");
}

int main(void)
{
    test_str_helpers();
    test_str_replace_all();
    test_str_split_whitespace();
    test_str_matches_all_words();
    test_buf();
    test_path_join();
    test_path_is_safe();
    test_fs_atomic();
    test_fs_mkdir_p();
    test_err();
    test_sha256();

    if (failures == 0) {
        (void)printf("ok: all util tests passed\n");
    } else {
        (void)printf("FAILED: %d check(s) failed\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
