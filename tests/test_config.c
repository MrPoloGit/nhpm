/* test_config.c - unit tests for config.c's INI reader/writer and the app
 * config file (no network, no terminal).
 * SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "config.h"
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

static void test_flat_parse(void)
{
    const char *text = "# a comment\n"
                       "\n"
                       "index_max_age_days = 7\n"
                       "pager   = less\n";

    cfg_doc doc;
    cfg_doc_init(&doc);
    CHECK(cfg_doc_parse(text, &doc) == NHP_OK);
    CHECK(cfg_get_long(&doc, NULL, "index_max_age_days", -1) == 7);
    CHECK(strcmp(cfg_get(&doc, NULL, "pager", ""), "less") == 0);
    CHECK(strcmp(cfg_get(&doc, NULL, "missing", "fallback"), "fallback") == 0);
    cfg_doc_free(&doc);
}

static void test_sections_and_repeats(void)
{
    const char *text = "[instance]\n"
                       "name = 367-colorful\n"
                       "\n"
                       "[patch]\n"
                       "id = local-menucolors\n"
                       "enabled = yes\n"
                       "\n"
                       "[patch]\n"
                       "id = 498\n"
                       "enabled = no\n";

    cfg_doc doc;
    cfg_doc_init(&doc);
    CHECK(cfg_doc_parse(text, &doc) == NHP_OK);

    CHECK(cfg_doc_section_count(&doc, "patch") == 2);
    CHECK(cfg_doc_section_count(&doc, "instance") == 1);
    CHECK(cfg_doc_section_count(&doc, "nonexistent") == 0);

    cfg_section *p0 = cfg_doc_section(&doc, "patch", 0);
    cfg_section *p1 = cfg_doc_section(&doc, "patch", 1);
    CHECK(p0 != NULL && strcmp(cfg_section_get(p0, "id", ""), "local-menucolors") == 0);
    CHECK(p1 != NULL && strcmp(cfg_section_get(p1, "id", ""), "498") == 0);
    CHECK(cfg_section_get_bool(p0, "enabled", false) == true);
    CHECK(cfg_section_get_bool(p1, "enabled", true) == false);
    CHECK(cfg_doc_section(&doc, "patch", 2) == NULL);

    cfg_doc_free(&doc);
}

static void test_bad_input(void)
{
    cfg_doc doc;
    cfg_doc_init(&doc);
    CHECK(cfg_doc_parse("not a valid line\n", &doc) == NHP_EPARSE);
    CHECK(err_get()[0] != '\0');
    cfg_doc_free(&doc);

    cfg_doc_init(&doc);
    CHECK(cfg_doc_parse("= novalue\n", &doc) == NHP_EPARSE);
    cfg_doc_free(&doc);

    cfg_doc_init(&doc);
    CHECK(cfg_doc_parse("", &doc) == NHP_OK);
    CHECK(cfg_doc_section_count(&doc, NULL) == 0);
    cfg_doc_free(&doc);
}

static void test_round_trip(void)
{
    const char *text = "[instance]\n"
                       "name = 367-colorful\n"
                       "notes = Vanilla plus colors.\n"
                       "\n"
                       "[patch]\n"
                       "id = 498\n"
                       "enabled = yes\n";

    cfg_doc doc1;
    cfg_doc_init(&doc1);
    CHECK(cfg_doc_parse(text, &doc1) == NHP_OK);
    char *formatted1 = cfg_doc_format(&doc1);
    cfg_doc_free(&doc1);
    CHECK(formatted1 != NULL);

    cfg_doc doc2;
    cfg_doc_init(&doc2);
    CHECK(cfg_doc_parse(formatted1, &doc2) == NHP_OK);
    char *formatted2 = cfg_doc_format(&doc2);
    cfg_doc_free(&doc2);
    CHECK(formatted2 != NULL);

    CHECK(formatted1 != NULL && formatted2 != NULL && strcmp(formatted1, formatted2) == 0);
    free(formatted1);
    free(formatted2);
}

static void test_set_updates_in_place(void)
{
    cfg_doc doc;
    cfg_doc_init(&doc);
    CHECK(cfg_set(&doc, NULL, "key", "first") == NHP_OK);
    CHECK(cfg_set(&doc, NULL, "key", "second") == NHP_OK);
    CHECK(cfg_doc_section_count(&doc, NULL) == 1);
    CHECK(strcmp(cfg_get(&doc, NULL, "key", ""), "second") == 0);
    cfg_doc_free(&doc);
}

static void test_app_config_round_trip(void)
{
    char *tmp_home = fs_make_temp_dir("nhp-test-xdg");
    CHECK(tmp_home != NULL);
    setenv("XDG_CONFIG_HOME", tmp_home, 1);

    cfg_app app;
    cfg_app_defaults(&app);
    free(app.author_name);
    app.author_name = str_dup("Test Author");
    app.index_max_age_days = 14;
    app.wayback_fallback = false;

    CHECK(cfg_app_save(&app) == NHP_OK);
    cfg_app_free(&app);

    cfg_app loaded;
    cfg_app_defaults(&loaded);
    CHECK(cfg_app_load(&loaded) == NHP_OK);
    CHECK(loaded.index_max_age_days == 14);
    CHECK(loaded.wayback_fallback == false);
    CHECK(loaded.author_name != NULL && strcmp(loaded.author_name, "Test Author") == 0);
    cfg_app_free(&loaded);

    unsetenv("XDG_CONFIG_HOME");
    fs_remove_tree(tmp_home);
    free(tmp_home);
}

static void test_app_config_missing_file(void)
{
    char *tmp_home = fs_make_temp_dir("nhp-test-xdg");
    CHECK(tmp_home != NULL);
    setenv("XDG_CONFIG_HOME", tmp_home, 1);

    cfg_app app;
    cfg_app_defaults(&app);
    CHECK(cfg_app_load(&app) == NHP_OK);
    CHECK(app.index_max_age_days == 7);
    CHECK(app.wayback_fallback == true);
    cfg_app_free(&app);

    unsetenv("XDG_CONFIG_HOME");
    fs_remove_tree(tmp_home);
    free(tmp_home);
}

/* docs/features.md 12.5's "i": the Changes dialog's backend entry point
 * for adding a pattern to config's own `ignore` key on the spot. */
static void test_add_ignore_pattern(void)
{
    char *tmp_home = fs_make_temp_dir("nhp-test-xdg");
    CHECK(tmp_home != NULL);
    setenv("XDG_CONFIG_HOME", tmp_home, 1);

    cfg_app app;
    cfg_app_defaults(&app);
    CHECK(str_eq(app.ignore, ""));

    CHECK(cfg_app_add_ignore_pattern(&app, "*.bak") == NHP_OK);
    CHECK(str_eq(app.ignore, "*.bak"));

    CHECK(cfg_app_add_ignore_pattern(&app, "*.tmp") == NHP_OK);
    CHECK(str_eq(app.ignore, "*.bak *.tmp"));

    /* already there -- a no-op, not a duplicate */
    CHECK(cfg_app_add_ignore_pattern(&app, "*.bak") == NHP_OK);
    CHECK(str_eq(app.ignore, "*.bak *.tmp"));

    CHECK(cfg_app_add_ignore_pattern(&app, "") == NHP_EINVAL);
    CHECK(cfg_app_add_ignore_pattern(&app, NULL) == NHP_EINVAL);

    /* persisted, not just in memory */
    cfg_app_free(&app);
    cfg_app loaded;
    cfg_app_defaults(&loaded);
    CHECK(cfg_app_load(&loaded) == NHP_OK);
    CHECK(str_eq(loaded.ignore, "*.bak *.tmp"));
    cfg_app_free(&loaded);

    unsetenv("XDG_CONFIG_HOME");
    fs_remove_tree(tmp_home);
    free(tmp_home);
}

static void test_active_instance_round_trip(void)
{
    char *tmp_home = fs_make_temp_dir("nhp-test-xdg");
    CHECK(tmp_home != NULL);
    setenv("XDG_CONFIG_HOME", tmp_home, 1);

    char *none = cfg_get_active_instance();
    CHECK(none == NULL); /* no ui-state file yet */

    CHECK(cfg_set_active_instance("367-colorful") == NHP_OK);
    char *got = cfg_get_active_instance();
    CHECK(got != NULL && str_eq(got, "367-colorful"));
    free(got);

    /* a second write preserves other keys already in the file (M7's
     * eventual last_screen/db_filter/etc., simulated here directly) */
    char *path = path_join(tmp_home, "nhpm/ui-state");
    char *data = NULL;
    size_t len = 0;
    CHECK(fs_read_file(path, &data, &len) == NHP_OK);
    buf b;
    buf_init(&b);
    buf_append_str(&b, data);
    buf_append_str(&b, "last_screen = patches\n");
    char *augmented = buf_take(&b);
    CHECK(fs_write_atomic(path, augmented, strlen(augmented)) == NHP_OK);
    free(data);
    free(augmented);

    CHECK(cfg_set_active_instance("another") == NHP_OK);
    got = cfg_get_active_instance();
    CHECK(got != NULL && str_eq(got, "another"));
    free(got);

    char *final_data = NULL;
    size_t final_len = 0;
    CHECK(fs_read_file(path, &final_data, &final_len) == NHP_OK);
    CHECK(strstr(final_data, "last_screen") != NULL);
    free(final_data);
    free(path);

    unsetenv("XDG_CONFIG_HOME");
    fs_remove_tree(tmp_home);
    free(tmp_home);
}

int main(void)
{
    test_flat_parse();
    test_sections_and_repeats();
    test_bad_input();
    test_round_trip();
    test_set_updates_in_place();
    test_app_config_round_trip();
    test_app_config_missing_file();
    test_add_ignore_pattern();
    test_active_instance_round_trip();

    if (failures == 0) {
        (void)printf("ok: all config tests passed\n");
    } else {
        (void)printf("FAILED: %d check(s) failed\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
