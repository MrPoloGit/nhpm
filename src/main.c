/* main.c - arguments, start the ncurses interface or run a CLI command.
 * SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "build.h"
#include "config.h"
#include "index.h"
#include "instance.h"
#include "library.h"
#include "mkpatch.h"
#include "site.h"
#include "sources.h"
#include "stack.h"
#include "ui/ui.h"
#include "util.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Forward declarations: these (defined further down) are used by
 * cmd_inst_clone()/cmd_inst_rm()/cmd_inst_restore(), which appear earlier
 * in the file. */
static nhp_err resolve_instance_locked(int argc, char **argv, int from, inst_instance *out,
                                       inst_lock *out_lock);
static nhp_err lock_instance_by_name(const char *name, inst_lock *out_lock);
static void print_patch_conflict(const stack_conflict *c);

static void print_usage(FILE *out)
{
    (void)fprintf(out, "usage: nhpm [--version] [--help] [--debug]\n");
    (void)fprintf(out, "       --debug (interface mode only): writes a timestamped activity log\n");
    (void)fprintf(out, "       to $XDG_CACHE_HOME/nhpm/ (every status-line message, success or\n");
    (void)fprintf(out, "       failure); its path is printed once you quit\n");
    (void)fprintf(out, "       nhpm refresh\n");
    (void)fprintf(out, "       nhpm search [query]\n");
    (void)fprintf(out, "       nhpm info <id>\n");
    (void)fprintf(out, "       nhpm download <id>\n");
    (void)fprintf(out, "       nhpm library [query]\n");
    (void)fprintf(out, "       nhpm import <file> <name> [author] [target] [version]\n");
    (void)fprintf(out, "       nhpm rm <id> [version] [--yes]\n");
    (void)fprintf(out, "\n");
    (void)fprintf(out, "       nhpm source add <folder|tarball> [version]\n");
    (void)fprintf(out, "       nhpm source git <tag>\n");
    (void)fprintf(out, "       nhpm source list\n");
    (void)fprintf(out, "       nhpm source rm <version>\n");
    (void)fprintf(out, "\n");
    (void)fprintf(out, "       nhpm inst new <name> --source <version> [--recipe <recipe>]\n");
    (void)fprintf(out, "       nhpm inst link <name> <folder> [recipe]\n");
    (void)fprintf(out, "       nhpm inst list\n");
    (void)fprintf(out, "       nhpm inst use <name>\n");
    (void)fprintf(out, "       nhpm inst clone <name> <new-name> [--with-saves]\n");
    (void)fprintf(out, "       nhpm inst rename <name> <new-name>\n");
    (void)fprintf(out, "       nhpm inst rm <name> [--keep-saves] [--yes]\n");
    (void)fprintf(out, "       nhpm inst snapshot <name> [label]\n");
    (void)fprintf(out, "       nhpm inst snapshots <name>\n");
    (void)fprintf(out, "       nhpm inst restore <name> <snapshot>\n");
    (void)fprintf(out, "       nhpm inst export <name> [file]\n");
    (void)fprintf(out, "       nhpm inst import <file> <new-name> [--source <version>]\n");
    (void)fprintf(out, "\n");
    (void)fprintf(out, "       nhpm enable <id>[@version] [--inst <name>]\n");
    (void)fprintf(out, "       nhpm disable <id> [--inst <name>]\n");
    (void)fprintf(out, "       nhpm move <id> up|down|<position> [--inst <name>]\n");
    (void)fprintf(out, "       nhpm undo [--inst <name>]\n");
    (void)fprintf(out, "       nhpm edit-begin <id> [--inst <name>]\n");
    (void)fprintf(out, "       nhpm edit-refresh <id> [--version V] [--name NAME]\n"
                       "               [--author A] [--target T] [--inst <name>]\n");
    (void)fprintf(out, "       nhpm new-patch <name> [--inst <name>]\n");
    (void)fprintf(out, "       nhpm track <path> [--inst <name>]\n");
    (void)fprintf(out, "       nhpm check [--adopt|--restore] [--inst <name>]\n");
    (void)fprintf(out, "       nhpm status [--check] [--inst <name>]\n");
    (void)fprintf(out, "       nhpm build [--inst <name>]\n");
    (void)fprintf(out, "       nhpm play [--inst <name>]\n");
    (void)fprintf(out, "\n");
    (void)fprintf(out, "       nhpm combined [--out FILE] [--to-library] [--flatten NEW] "
                       "[--inst <name>]\n");
    (void)fprintf(out, "       nhpm changes [--hidden] [--inst <name>]\n");
    (void)fprintf(out, "       nhpm capture <name> [--version V] [--author A] [--target T]\n"
                       "               [--desc TEXT] [--long-desc TEXT] [--files F...] "
                       "[--inst <name>]\n");
    (void)fprintf(out,
                  "       nhpm mkpatch <folder> <name> [--from base|folder:P|tarball:P|git:REF]\n"
                  "               [--version V] [--author A] [--target T] [--desc TEXT]\n"
                  "               [--long-desc TEXT] [--files F...]\n");
    (void)fprintf(out, "       nhpm export-patch <id>[@version] [FILE]\n");
    (void)fprintf(out, "\n");
    (void)fprintf(out, "Run with no arguments to start the ncurses interface (docs/features.md\n");
    (void)fprintf(out, "section 13).\n");
}

/* Opens the log file at ~/.cache/nhpm/nhpm.log, creating its directory
 * if needed. Not being able to log is never fatal: the caller just carries
 * on without it. */
static nhp_err init_logging(void)
{
    char *cache_dir = cfg_xdg_cache_dir();
    if (cache_dir == NULL) {
        return NHP_ENOMEM;
    }

    nhp_err err = fs_mkdir_p(cache_dir, 0700);
    if (err != NHP_OK) {
        free(cache_dir);
        return err;
    }

    char *log_path = path_join(cache_dir, "nhpm.log");
    free(cache_dir);
    if (log_path == NULL) {
        return NHP_ENOMEM;
    }

    log_init(log_path, LOG_INFO);
    free(log_path);
    return NHP_OK;
}

static void refresh_progress(void *user_data, size_t page, size_t total_pages)
{
    (void)user_data;
    (void)printf("  page %zu/%zu\n", page, total_pages);
}

static int cmd_refresh(void)
{
    cfg_app app;
    cfg_app_defaults(&app);
    if (cfg_app_load(&app) != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not load the config file: %s\n", err_get());
        /* Not fatal: carry on with defaults. */
    }

    (void)printf("Refreshing the patch index...\n");
    idx_index idx;
    idx_init(&idx);
    nhp_err err = idx_refresh(&idx, &app, refresh_progress, NULL);
    cfg_app_free(&app);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not refresh the index: %s\n", err_get());
        idx_free(&idx);
        return (err == NHP_ENET) ? 2 : 1;
    }

    (void)printf("Done: %zu patches indexed.\n", idx.count);
    idx_free(&idx);
    return 0;
}

static void print_entry(const idx_entry *e)
{
    (void)printf("%-6s %-45s %-16s %-16s %5s  %s\n", e->id, e->name, e->author, e->target,
                 e->rating, e->short_description);
}

static int cmd_search(const char *raw_query)
{
    idx_index idx;
    idx_init(&idx);
    nhp_err err = idx_load(&idx);
    if (err == NHP_ENOENT) {
        (void)fprintf(stderr, "nhpm: no index yet; run 'nhpm refresh' first\n");
        idx_free(&idx);
        return 1;
    }
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not load the index: %s\n", err_get());
        idx_free(&idx);
        return 1;
    }

    idx_query query;
    if (idx_query_parse(raw_query, &query) != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not parse the search query: %s\n", err_get());
        idx_free(&idx);
        return 1;
    }

    const idx_entry **matches = NULL;
    size_t match_count = 0;
    err = idx_search(&idx, &query, &matches, &match_count);
    idx_query_free(&query);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: search failed: %s\n", err_get());
        idx_free(&idx);
        return 1;
    }

    for (size_t i = 0; i < match_count; i++) {
        print_entry(matches[i]);
    }
    (void)printf("%zu of %zu patches matched\n", match_count, idx.count);

    free(matches);
    idx_free(&idx);
    return 0;
}

static int cmd_info(const char *id)
{
    site_detail detail;
    nhp_err err = lib_fetch_detail(id, &detail);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not fetch patch %s: %s\n", id, err_get());
        return (err == NHP_ENET) ? 2 : 1;
    }

    (void)printf("Name:        %s\n", detail.name);
    (void)printf("Author:      %s\n", detail.author);
    (void)printf("For:         %s\n", detail.target);
    (void)printf("Rating:      %s\n", detail.rating);
    (void)printf("Description: %s\n", detail.description);
    (void)printf("Download:    %s (%s)\n", detail.download_url,
                 detail.download_hosted ? "hosted" : "external");
    (void)printf("Added:       %s\n", detail.added);
    if (detail.changed != NULL) {
        (void)printf("Changed:     %s\n", detail.changed);
    }

    site_detail_free(&detail);
    return 0;
}

static int cmd_download(const char *id)
{
    cfg_app app;
    cfg_app_defaults(&app);
    if (cfg_app_load(&app) != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not load the config file: %s\n", err_get());
    }

    char *version = NULL;
    nhp_err err = lib_download(id, &app, &version);
    cfg_app_free(&app);
    if (err == NHP_EEXIST) {
        (void)fprintf(stderr, "nhpm: %s\n", err_get());
        free(version);
        return 1;
    }
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not download %s: %s\n", id, err_get());
        return (err == NHP_ENET) ? 2 : 1;
    }

    (void)printf("Downloaded %s version %s into the library.\n", id, version);
    free(version);
    return 0;
}

static void print_library_patch(const lib_patch *p)
{
    (void)printf("%-25s %-30s %-16s %-16s %zu version%s\n", p->id, p->name, p->author, p->target,
                 p->version_count, (p->version_count == 1) ? "" : "s");
    for (size_t i = 0; i < p->version_count; i++) {
        const lib_version *v = &p->versions[i];
        (void)printf("  %-10s downloaded %s%s\n", v->version, v->downloaded,
                     v->via_wayback ? " (via the Wayback Machine)" : "");
    }
}

static bool patch_matches_filter(const lib_patch *p, const char *filter)
{
    if (filter == NULL || filter[0] == '\0') {
        return true;
    }
    return str_contains_ci(p->id, filter) || str_contains_ci(p->name, filter) ||
           str_contains_ci(p->author, filter) || str_contains_ci(p->target, filter);
}

static int cmd_library(const char *filter)
{
    lib_patch *patches = NULL;
    size_t count = 0;
    nhp_err err = lib_list(&patches, &count);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not list the library: %s\n", err_get());
        return 1;
    }

    size_t shown = 0;
    for (size_t i = 0; i < count; i++) {
        if (patch_matches_filter(&patches[i], filter)) {
            print_library_patch(&patches[i]);
            shown++;
        }
    }
    (void)printf("%zu of %zu patches shown\n", shown, count);

    lib_list_free(patches, count);
    return 0;
}

static bool confirm(const char *prompt, bool assume_yes)
{
    if (assume_yes) {
        return true;
    }
    (void)printf("%s [y/N] ", prompt);
    (void)fflush(stdout);
    char line[16];
    if (fgets(line, sizeof(line), stdin) == NULL) {
        return false;
    }
    return line[0] == 'y' || line[0] == 'Y';
}

static int cmd_rm(const char *id, const char *version, bool assume_yes)
{
    buf prompt_buf;
    buf_init(&prompt_buf);
    if (version != NULL) {
        buf_append_fmt(&prompt_buf, "Delete %s version %s?", id, version);
    } else {
        buf_append_fmt(&prompt_buf, "Delete all of %s (every version)?", id);
    }
    char *prompt = buf_take(&prompt_buf);
    bool ok = confirm(prompt, assume_yes);
    free(prompt);
    if (!ok) {
        (void)printf("Not deleted.\n");
        return 0;
    }

    nhp_err err = (version != NULL) ? lib_delete_version(id, version) : lib_delete_patch(id);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not delete: %s\n", err_get());
        return 1;
    }
    (void)printf("Deleted.\n");
    return 0;
}

static int cmd_import(int argc, char **argv)
{
    if (argc < 4) {
        (void)fprintf(stderr, "usage: nhpm import <file> <name> [author] [target] [version]\n");
        return 1;
    }
    const char *path = argv[2];
    const char *name = argv[3];
    const char *author = (argc > 4) ? argv[4] : "";
    const char *target = (argc > 5) ? argv[5] : "";
    const char *version = (argc > 6) ? argv[6] : NULL;

    char *id = NULL;
    nhp_err err = lib_import_local(path, name, author, target, version, &id);
    if (err == NHP_EEXIST) {
        (void)fprintf(stderr, "nhpm: %s\n", err_get());
        free(id);
        return 1;
    }
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not import %s: %s\n", path, err_get());
        return 1;
    }

    (void)printf("Imported as %s.\n", id);
    free(id);
    return 0;
}

static void print_source_row(const bsrc_source *s)
{
    (void)printf("%-10s %-45s %s%s\n", s->version, s->origin, s->added,
                 s->is_variant ? " (variant)" : "");
}

static int cmd_source(int argc, char **argv)
{
    if (argc < 3) {
        print_usage(stderr);
        return 1;
    }
    const char *sub = argv[2];

    if (str_eq(sub, "add")) {
        if (argc < 4) {
            (void)fprintf(stderr, "usage: nhpm source add <folder|tarball> [version]\n");
            return 1;
        }
        const char *path = argv[3];
        const char *version_hint = (argc > 4) ? argv[4] : NULL;
        struct stat st;
        bool is_dir = stat(path, &st) == 0 && S_ISDIR(st.st_mode);

        char *version = NULL;
        nhp_err err = is_dir ? bsrc_add_from_folder(path, version_hint, &version)
                             : bsrc_add_from_tarball(path, version_hint, &version);
        if (err != NHP_OK) {
            (void)fprintf(stderr, "nhpm: could not add %s: %s\n", path, err_get());
            free(version);
            return 1;
        }
        (void)printf("Added base source %s.\n", version);
        free(version);
        return 0;
    }

    if (str_eq(sub, "git")) {
        if (argc < 4) {
            (void)fprintf(stderr, "usage: nhpm source git <tag>\n");
            return 1;
        }
        char *version = NULL;
        nhp_err err = bsrc_add_from_git(argv[3], &version);
        if (err != NHP_OK) {
            (void)fprintf(stderr, "nhpm: could not clone %s: %s\n", argv[3], err_get());
            free(version);
            return (err == NHP_ENET) ? 2 : 1;
        }
        (void)printf("Added base source %s.\n", version);
        free(version);
        return 0;
    }

    if (str_eq(sub, "list")) {
        bsrc_source *sources = NULL;
        size_t count = 0;
        nhp_err err = bsrc_list(&sources, &count);
        if (err != NHP_OK) {
            (void)fprintf(stderr, "nhpm: could not list base sources: %s\n", err_get());
            return 1;
        }
        for (size_t i = 0; i < count; i++) {
            print_source_row(&sources[i]);
        }
        (void)printf("%zu base source(s)\n", count);
        bsrc_list_free(sources, count);
        return 0;
    }

    if (str_eq(sub, "rm")) {
        if (argc < 4) {
            (void)fprintf(stderr, "usage: nhpm source rm <version>\n");
            return 1;
        }
        nhp_err err = bsrc_remove(argv[3]);
        if (err != NHP_OK) {
            (void)fprintf(stderr, "nhpm: could not remove %s: %s\n", argv[3], err_get());
            return 1;
        }
        (void)printf("Removed.\n");
        return 0;
    }

    (void)fprintf(stderr, "nhpm: unknown 'source' subcommand '%s'\n", sub);
    return 1;
}

static void print_instance_row(const inst_instance *i)
{
    (void)printf("%-20s %-8s %-10s %-20s %zu patch(es)\n", i->name,
                 (i->kind == INST_LINKED) ? "linked" : "managed", i->source, i->recipe,
                 i->patch_count);
}

/* Shared by "inst new" and "inst import": pulls "--source <version>" and
 * "--recipe <recipe>" out of argv[from..argc), leaving *out_recipe "" if
 * --recipe wasn't given. */
static void parse_source_and_recipe_flags(int argc, char **argv, int from, const char **out_source,
                                          const char **out_recipe)
{
    *out_source = NULL;
    *out_recipe = "";
    for (int i = from; i < argc; i++) {
        if (str_eq(argv[i], "--source") && i + 1 < argc) {
            *out_source = argv[++i];
        } else if (str_eq(argv[i], "--recipe") && i + 1 < argc) {
            *out_recipe = argv[++i];
        }
    }
}

static int cmd_inst_new(int argc, char **argv)
{
    if (argc < 4) {
        (void)fprintf(stderr, "usage: nhpm inst new <name> --source <version> [--recipe "
                              "<recipe>]\n");
        return 1;
    }
    const char *source_version = NULL;
    const char *recipe = NULL;
    parse_source_and_recipe_flags(argc, argv, 4, &source_version, &recipe);
    if (source_version == NULL) {
        (void)fprintf(stderr, "usage: nhpm inst new <name> --source <version> [--recipe "
                              "<recipe>]\n");
        return 1;
    }

    inst_instance inst;
    nhp_err err = inst_new(argv[3], source_version, recipe, &inst);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not create %s: %s\n", argv[3], err_get());
        return 1;
    }
    inst_free(&inst);
    (void)cfg_set_active_instance(argv[3]); /* a fresh instance becomes the active one */
    (void)printf("Created and switched to instance %s.\n", argv[3]);
    return 0;
}

static int cmd_inst_link(int argc, char **argv)
{
    if (argc < 5) {
        (void)fprintf(stderr, "usage: nhpm inst link <name> <folder> [recipe]\n");
        return 1;
    }
    inst_instance inst;
    nhp_err err = inst_link(argv[3], argv[4], (argc > 5) ? argv[5] : "", &inst);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not link %s: %s\n", argv[3], err_get());
        return 1;
    }
    (void)printf("Linked and switched to instance %s -> %s (NetHack %s).\n", argv[3], argv[4],
                 inst.source);
    inst_free(&inst);
    (void)cfg_set_active_instance(argv[3]); /* a fresh instance becomes the active one */
    return 0;
}

static int cmd_inst_list(void)
{
    inst_instance *list = NULL;
    size_t count = 0;
    nhp_err err = inst_list(&list, &count);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not list instances: %s\n", err_get());
        return 1;
    }
    for (size_t i = 0; i < count; i++) {
        print_instance_row(&list[i]);
    }
    (void)printf("%zu instance(s)\n", count);
    inst_list_free(list, count);
    return 0;
}

static int cmd_inst_use(int argc, char **argv)
{
    if (argc < 4) {
        (void)fprintf(stderr, "usage: nhpm inst use <name>\n");
        return 1;
    }
    inst_instance inst;
    nhp_err err = inst_load(argv[3], &inst);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not use %s: %s\n", argv[3], err_get());
        return 1;
    }
    inst_free(&inst);

    err = cfg_set_active_instance(argv[3]);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not save the active instance: %s\n", err_get());
        return 1;
    }
    (void)printf("Using %s.\n", argv[3]);
    return 0;
}

static int cmd_inst_clone(int argc, char **argv)
{
    if (argc < 5) {
        (void)fprintf(stderr, "usage: nhpm inst clone <name> <new-name> [--with-saves]\n");
        return 1;
    }
    bool with_saves = false;
    for (int i = 5; i < argc; i++) {
        if (str_eq(argv[i], "--with-saves")) {
            with_saves = true;
        }
    }

    inst_lock lock;
    nhp_err err = lock_instance_by_name(argv[3], &lock);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: %s\n", err_get());
        return (err == NHP_ELOCKED) ? 5 : 1;
    }
    err = inst_clone(argv[3], argv[4], with_saves);
    inst_lock_release(&lock);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not clone %s: %s\n", argv[3], err_get());
        return 1;
    }
    (void)printf("Cloned %s to %s.\n", argv[3], argv[4]);
    return 0;
}

static int cmd_inst_rename(int argc, char **argv)
{
    if (argc < 5) {
        (void)fprintf(stderr, "usage: nhpm inst rename <name> <new-name>\n");
        return 1;
    }
    nhp_err err = inst_rename(argv[3], argv[4]);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not rename %s: %s\n", argv[3], err_get());
        return (err == NHP_ELOCKED) ? 5 : 1;
    }
    (void)printf("Renamed %s to %s.\n", argv[3], argv[4]);
    return 0;
}

static int cmd_inst_rm(int argc, char **argv)
{
    if (argc < 4) {
        (void)fprintf(stderr, "usage: nhpm inst rm <name> [--keep-saves] [--yes]\n");
        return 1;
    }
    bool keep_saves = false;
    bool assume_yes = false;
    for (int i = 4; i < argc; i++) {
        if (str_eq(argv[i], "--keep-saves")) {
            keep_saves = true;
        } else if (str_eq(argv[i], "--yes")) {
            assume_yes = true;
        }
    }

    buf prompt_buf;
    buf_init(&prompt_buf);
    buf_append_fmt(&prompt_buf, "Delete instance %s%s?", argv[3],
                   keep_saves ? " (keeping its save games)" : " (and its save games)");
    char *prompt = buf_take(&prompt_buf);
    bool ok = confirm(prompt, assume_yes);
    free(prompt);
    if (!ok) {
        (void)printf("Not deleted.\n");
        return 0;
    }

    inst_lock lock;
    nhp_err err = lock_instance_by_name(argv[3], &lock);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: %s\n", err_get());
        return (err == NHP_ELOCKED) ? 5 : 1;
    }
    err = inst_delete(argv[3], keep_saves);
    inst_lock_release(&lock);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not delete %s: %s\n", argv[3], err_get());
        return 1;
    }
    (void)printf("Deleted.\n");
    return 0;
}

static int cmd_inst_snapshot(int argc, char **argv)
{
    if (argc < 4) {
        (void)fprintf(stderr, "usage: nhpm inst snapshot <name> [label]\n");
        return 1;
    }
    char *filename = NULL;
    nhp_err err = inst_snapshot_create(argv[3], (argc > 4) ? argv[4] : NULL, &filename);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not snapshot %s: %s\n", argv[3], err_get());
        return 1;
    }
    (void)printf("Snapshot: %s\n", filename);
    free(filename);
    return 0;
}

static int cmd_inst_snapshots(int argc, char **argv)
{
    if (argc < 4) {
        (void)fprintf(stderr, "usage: nhpm inst snapshots <name>\n");
        return 1;
    }
    inst_snapshot *snaps = NULL;
    size_t count = 0;
    nhp_err err = inst_snapshot_list(argv[3], &snaps, &count);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not list snapshots for %s: %s\n", argv[3], err_get());
        return 1;
    }
    for (size_t i = 0; i < count; i++) {
        (void)printf("%-45s %-20s %s\n", snaps[i].filename, snaps[i].label, snaps[i].created);
    }
    (void)printf("%zu snapshot(s)\n", count);
    inst_snapshot_list_free(snaps, count);
    return 0;
}

static int cmd_inst_restore(int argc, char **argv)
{
    if (argc < 5) {
        (void)fprintf(stderr, "usage: nhpm inst restore <name> <snapshot>\n");
        return 1;
    }
    inst_lock lock;
    nhp_err err = lock_instance_by_name(argv[3], &lock);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: %s\n", err_get());
        return (err == NHP_ELOCKED) ? 5 : 1;
    }
    inst_instance inst;
    err = inst_load(argv[3], &inst);
    if (err == NHP_OK) {
        stack_conflict conflict = {0};
        err = stack_restore_snapshot(&inst, argv[4], &conflict);
        if (err == NHP_ECONFLICT) {
            print_patch_conflict(&conflict);
            stack_conflict_free(&conflict);
        }
        inst_free(&inst);
    }
    inst_lock_release(&lock);
    if (err != NHP_OK) {
        if (err != NHP_ECONFLICT) {
            (void)fprintf(stderr, "nhpm: could not restore %s: %s\n", argv[4], err_get());
        }
        return 1;
    }
    (void)printf("Restored %s from %s.\n", argv[3], argv[4]);
    return 0;
}

static int cmd_inst_export(int argc, char **argv)
{
    if (argc < 4) {
        (void)fprintf(stderr, "usage: nhpm inst export <name> [file]\n");
        return 1;
    }
    char *default_path = NULL;
    const char *out_path;
    if (argc > 4) {
        out_path = argv[4];
    } else {
        buf b;
        buf_init(&b);
        buf_append_fmt(&b, "%s.nhinst", argv[3]);
        default_path = buf_take(&b);
        out_path = default_path;
    }

    nhp_err err = inst_export(argv[3], out_path);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not export %s: %s\n", argv[3], err_get());
        free(default_path);
        return 1;
    }
    (void)printf("Exported to %s.\n", out_path);
    free(default_path);
    return 0;
}

static int cmd_inst_import(int argc, char **argv)
{
    if (argc < 5) {
        (void)fprintf(stderr, "usage: nhpm inst import <file> <new-name> [--source "
                              "<version>]\n");
        return 1;
    }
    const char *source_version = NULL;
    const char *recipe = NULL;
    parse_source_and_recipe_flags(argc, argv, 5, &source_version, &recipe);

    inst_instance inst;
    nhp_err err = inst_import(argv[3], argv[4], source_version, &inst);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not import %s: %s\n", argv[3], err_get());
        return 1;
    }
    (void)printf("Imported %s as %s.\n", argv[3], argv[4]);
    inst_free(&inst);
    return 0;
}

static int cmd_inst(int argc, char **argv)
{
    if (argc < 3) {
        print_usage(stderr);
        return 1;
    }
    const char *sub = argv[2];

    if (str_eq(sub, "new")) {
        return cmd_inst_new(argc, argv);
    }
    if (str_eq(sub, "link")) {
        return cmd_inst_link(argc, argv);
    }
    if (str_eq(sub, "list")) {
        return cmd_inst_list();
    }
    if (str_eq(sub, "use")) {
        return cmd_inst_use(argc, argv);
    }
    if (str_eq(sub, "clone")) {
        return cmd_inst_clone(argc, argv);
    }
    if (str_eq(sub, "rename")) {
        return cmd_inst_rename(argc, argv);
    }
    if (str_eq(sub, "rm")) {
        return cmd_inst_rm(argc, argv);
    }
    if (str_eq(sub, "snapshot")) {
        return cmd_inst_snapshot(argc, argv);
    }
    if (str_eq(sub, "snapshots")) {
        return cmd_inst_snapshots(argc, argv);
    }
    if (str_eq(sub, "restore")) {
        return cmd_inst_restore(argc, argv);
    }
    if (str_eq(sub, "export")) {
        return cmd_inst_export(argc, argv);
    }
    if (str_eq(sub, "import")) {
        return cmd_inst_import(argc, argv);
    }

    (void)fprintf(stderr, "nhpm: unknown 'inst' subcommand '%s'\n", sub);
    return 1;
}

/* Finds "--inst <name>" anywhere in argv[from..argc) and loads that
 * instance; otherwise loads ui-state's active instance (docs/features.md
 * 10.7). Returns NHP_EINVAL if neither is available. */
static nhp_err resolve_instance(int argc, char **argv, int from, inst_instance *out)
{
    const char *name = NULL;
    for (int i = from; i < argc; i++) {
        if (str_eq(argv[i], "--inst") && i + 1 < argc) {
            name = argv[i + 1];
            break;
        }
    }
    char *active = NULL;
    if (name == NULL) {
        active = cfg_get_active_instance();
        name = active;
    }
    if (name == NULL) {
        err_set("no active instance -- run 'nhpm inst use <name>' first, or pass --inst "
                "<name>");
        return NHP_EINVAL;
    }
    nhp_err err = inst_load(name, out);
    free(active);
    return err;
}

/* Like resolve_instance(), but also takes the instance's exclusive lock
 * (docs/features.md 10.8) for commands that mutate it, so a racing nhpm
 * gets NHP_ELOCKED. Read-only commands use plain resolve_instance(). */
static nhp_err resolve_instance_locked(int argc, char **argv, int from, inst_instance *out,
                                       inst_lock *out_lock)
{
    nhp_err err = resolve_instance(argc, argv, from, out);
    if (err != NHP_OK) {
        return err;
    }
    pid_t other_pid = 0;
    err = inst_lock_acquire(out->name, out_lock, &other_pid);
    if (err != NHP_OK) {
        if (err == NHP_ELOCKED) {
            err_set("instance %s is locked by another nhpm (pid %ld)", out->name, (long)other_pid);
        }
        inst_free(out);
    }
    return err;
}

/* Acquires `name`'s lock directly, for a command (clone/rm/restore) that
 * only has the name, not an already-loaded inst_instance -- same
 * NHP_ELOCKED-with-PID err_get() message as resolve_instance_locked(). */
static nhp_err lock_instance_by_name(const char *name, inst_lock *out_lock)
{
    pid_t other_pid = 0;
    nhp_err err = inst_lock_acquire(name, out_lock, &other_pid);
    if (err == NHP_ELOCKED) {
        err_set("instance %s is locked by another nhpm (pid %ld)", name, (long)other_pid);
    }
    return err;
}

static void print_patch_conflict(const stack_conflict *c)
{
    (void)fprintf(stderr, "nhpm: %s %s does not apply cleanly%s\n", c->patch_id, c->patch_version,
                  c->is_requested_patch ? "" : " (a later patch in the stack depends on it)");
    for (size_t i = 0; i < c->file_count; i++) {
        (void)fprintf(stderr, "  %-30s hunk %d of %d FAILED\n", c->files[i].path,
                      c->files[i].hunks_failed, c->files[i].hunks_total);
    }
    if (c->quilt_output != NULL && c->quilt_output[0] != '\0') {
        (void)fprintf(stderr, "--- quilt output ---\n%s", c->quilt_output);
    }
}

/* Adds `id` to *inst from the library (docs/features.md section 11.5's
 * "Add from library"), at `version` if given, else the newest
 * downloaded version, then enables it. */
static nhp_err add_from_library_and_enable(inst_instance *inst, const char *id, const char *version,
                                           size_t *out_index, stack_conflict *out_conflict)
{
    lib_patch patch;
    nhp_err err = lib_find(id, &patch);
    if (err != NHP_OK) {
        return err;
    }

    const char *use_version = version;
    if (use_version == NULL && patch.version_count > 0) {
        use_version = patch.versions[patch.version_count - 1].version;
    }
    if (use_version == NULL) {
        err_set("%s has no downloaded versions", id);
        lib_patch_free(&patch);
        return NHP_ENOENT;
    }

    char *diff_path = NULL;
    err = lib_version_diff_path(id, use_version, &diff_path);
    if (err == NHP_OK) {
        const char *sha = "";
        for (size_t i = 0; i < patch.version_count; i++) {
            if (str_eq(patch.versions[i].version, use_version)) {
                sha = patch.versions[i].sha256;
                break;
            }
        }
        err = stack_add(inst, id, use_version, patch.name, diff_path, sha, true, out_index,
                        out_conflict);
    }
    free(diff_path);
    lib_patch_free(&patch);
    return err;
}

static int cmd_enable(int argc, char **argv)
{
    if (argc < 3) {
        (void)fprintf(stderr, "usage: nhpm enable <id>[@version] [--inst <name>]\n");
        return 1;
    }
    char id[256];
    char version_buf[64];
    const char *version_arg = NULL;
    const char *at = strchr(argv[2], '@');
    if (at != NULL) {
        size_t n = (size_t)(at - argv[2]);
        if (n >= sizeof(id)) {
            n = sizeof(id) - 1;
        }
        memcpy(id, argv[2], n);
        id[n] = '\0';
        (void)snprintf(version_buf, sizeof(version_buf), "%s", at + 1);
        version_arg = version_buf;
    } else {
        (void)snprintf(id, sizeof(id), "%s", argv[2]);
    }

    inst_instance inst;
    inst_lock lock;
    nhp_err err = resolve_instance_locked(argc, argv, 3, &inst, &lock);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: %s\n", err_get());
        return (err == NHP_ELOCKED) ? 5 : 1;
    }

    size_t index = SIZE_MAX;
    stack_conflict conflict = {0};
    err = inst_find_patch_index(&inst, id, &index);
    if (err == NHP_ENOENT) {
        err = add_from_library_and_enable(&inst, id, version_arg, &index, &conflict);
    } else if (err == NHP_OK) {
        err = stack_enable(&inst, index, &conflict);
    }

    if (err == NHP_ECONFLICT) {
        print_patch_conflict(&conflict);
        stack_conflict_free(&conflict);
        inst_lock_release(&lock);
        inst_free(&inst);
        return 3;
    }
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not enable %s: %s\n", id, err_get());
        inst_lock_release(&lock);
        inst_free(&inst);
        return 1;
    }
    (void)printf("Enabled %s (strip -p%d).\n", id, inst.patches[index].strip);
    inst_lock_release(&lock);
    inst_free(&inst);
    return 0;
}

static int cmd_disable(int argc, char **argv)
{
    if (argc < 3) {
        (void)fprintf(stderr, "usage: nhpm disable <id> [--inst <name>]\n");
        return 1;
    }
    inst_instance inst;
    inst_lock lock;
    nhp_err err = resolve_instance_locked(argc, argv, 3, &inst, &lock);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: %s\n", err_get());
        return (err == NHP_ELOCKED) ? 5 : 1;
    }

    size_t index = SIZE_MAX;
    err = inst_find_patch_index(&inst, argv[2], &index);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: %s\n", err_get());
        inst_lock_release(&lock);
        inst_free(&inst);
        return 1;
    }

    stack_conflict conflict = {0};
    err = stack_disable(&inst, index, &conflict);
    if (err == NHP_ECONFLICT) {
        print_patch_conflict(&conflict);
        stack_conflict_free(&conflict);
        inst_lock_release(&lock);
        inst_free(&inst);
        return 3;
    }
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not disable %s: %s\n", argv[2], err_get());
        inst_lock_release(&lock);
        inst_free(&inst);
        return 1;
    }
    (void)printf("Disabled %s.\n", argv[2]);
    inst_lock_release(&lock);
    inst_free(&inst);
    return 0;
}

static int cmd_move(int argc, char **argv)
{
    if (argc < 4) {
        (void)fprintf(stderr, "usage: nhpm move <id> up|down|<position> [--inst <name>]\n");
        return 1;
    }
    inst_instance inst;
    inst_lock lock;
    nhp_err err = resolve_instance_locked(argc, argv, 4, &inst, &lock);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: %s\n", err_get());
        return (err == NHP_ELOCKED) ? 5 : 1;
    }

    size_t index = SIZE_MAX;
    err = inst_find_patch_index(&inst, argv[2], &index);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: %s\n", err_get());
        inst_lock_release(&lock);
        inst_free(&inst);
        return 1;
    }

    size_t target;
    if (str_eq(argv[3], "up")) {
        if (index == 0) {
            (void)fprintf(stderr, "nhpm: %s is already at the top\n", argv[2]);
            inst_lock_release(&lock);
            inst_free(&inst);
            return 1;
        }
        target = index - 1;
    } else if (str_eq(argv[3], "down")) {
        if (index + 1 >= inst.patch_count) {
            (void)fprintf(stderr, "nhpm: %s is already at the bottom\n", argv[2]);
            inst_lock_release(&lock);
            inst_free(&inst);
            return 1;
        }
        target = index + 1;
    } else {
        char *endptr = NULL;
        long pos = strtol(argv[3], &endptr, 10);
        if (endptr == argv[3] || *endptr != '\0' || pos < 1 || (size_t)pos > inst.patch_count) {
            (void)fprintf(stderr, "nhpm: bad position '%s'\n", argv[3]);
            inst_lock_release(&lock);
            inst_free(&inst);
            return 1;
        }
        target = (size_t)pos - 1;
    }

    stack_conflict conflict = {0};
    err = stack_move(&inst, index, target, &conflict);
    if (err == NHP_ECONFLICT) {
        print_patch_conflict(&conflict);
        stack_conflict_free(&conflict);
        inst_lock_release(&lock);
        inst_free(&inst);
        return 3;
    }
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not move %s: %s\n", argv[2], err_get());
        inst_lock_release(&lock);
        inst_free(&inst);
        return 1;
    }
    (void)printf("Moved %s.\n", argv[2]);
    inst_lock_release(&lock);
    inst_free(&inst);
    return 0;
}

/* docs/features.md 11.8's "u": undoes the most recent enable/disable/
 * move/add/remove by restoring its automatic snapshot. */
static int cmd_undo(int argc, char **argv)
{
    inst_instance inst;
    inst_lock lock;
    nhp_err err = resolve_instance_locked(argc, argv, 2, &inst, &lock);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: %s\n", err_get());
        return (err == NHP_ELOCKED) ? 5 : 1;
    }

    stack_conflict conflict = {0};
    err = stack_undo(&inst, &conflict);
    if (err == NHP_ECONFLICT) {
        print_patch_conflict(&conflict);
        stack_conflict_free(&conflict);
        inst_lock_release(&lock);
        inst_free(&inst);
        return 3;
    }
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not undo: %s\n", err_get());
        inst_lock_release(&lock);
        inst_free(&inst);
        return 1;
    }
    (void)printf("Undid the last change to %s.\n", inst.name);
    inst_lock_release(&lock);
    inst_free(&inst);
    return 0;
}

/* docs/features.md 12.4's "E" (Edit patch), "begin" half: pops the stack
 * down to and including <id>, leaving it on top to edit directly. Follow
 * up with 'edit-refresh' once done. */
static int cmd_edit_begin(int argc, char **argv)
{
    if (argc < 3) {
        (void)fprintf(stderr, "usage: nhpm edit-begin <id> [--inst <name>]\n");
        return 1;
    }
    inst_instance inst;
    inst_lock lock;
    nhp_err err = resolve_instance_locked(argc, argv, 3, &inst, &lock);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: %s\n", err_get());
        return (err == NHP_ELOCKED) ? 5 : 1;
    }

    size_t index = SIZE_MAX;
    err = inst_find_patch_index(&inst, argv[2], &index);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: %s\n", err_get());
        inst_lock_release(&lock);
        inst_free(&inst);
        return 1;
    }

    stack_conflict conflict = {0};
    err = stack_edit_begin(&inst, index, &conflict);
    if (err == NHP_ECONFLICT) {
        print_patch_conflict(&conflict);
        stack_conflict_free(&conflict);
        inst_lock_release(&lock);
        inst_free(&inst);
        return 3;
    }
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not begin editing %s: %s\n", argv[2], err_get());
        inst_lock_release(&lock);
        inst_free(&inst);
        return 1;
    }

    char *src_dir = inst_source_full_path(&inst);
    (void)printf("%s is on top -- edit files under %s, then run "
                 "'nhpm edit-refresh %s'.\n",
                 argv[2], (src_dir != NULL) ? src_dir : "the instance's source folder", argv[2]);
    free(src_dir);
    inst_lock_release(&lock);
    inst_free(&inst);
    return 0;
}

/* The "R" (refresh) half: quilt-refreshes <id>, pushes the rest of the
 * stack back on, then saves the result as a new library version if <id>
 * is a local- patch. A downloaded patch is just left modified = true. */
static int cmd_edit_refresh(int argc, char **argv)
{
    if (argc < 3) {
        (void)fprintf(stderr, "usage: nhpm edit-refresh <id> [--version V] [--name NAME]\n"
                              "               [--author A] [--target T] [--inst <name>]\n");
        return 1;
    }
    const char *version = "";
    const char *name = "";
    const char *author = NULL;
    const char *target = "";
    for (int i = 3; i < argc; i++) {
        if (str_eq(argv[i], "--version") && i + 1 < argc) {
            version = argv[++i];
        } else if (str_eq(argv[i], "--name") && i + 1 < argc) {
            name = argv[++i];
        } else if (str_eq(argv[i], "--author") && i + 1 < argc) {
            author = argv[++i];
        } else if (str_eq(argv[i], "--target") && i + 1 < argc) {
            target = argv[++i];
        } else if (str_eq(argv[i], "--inst") && i + 1 < argc) {
            i++;
        } else {
            (void)fprintf(stderr, "nhpm: unknown edit-refresh option '%s'\n", argv[i]);
            return 1;
        }
    }

    cfg_app app;
    cfg_app_defaults(&app);
    (void)cfg_app_load(&app);
    if (author == NULL) {
        author = app.author_name;
    }

    inst_instance inst;
    inst_lock lock;
    nhp_err err = resolve_instance_locked(argc, argv, 3, &inst, &lock);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: %s\n", err_get());
        cfg_app_free(&app);
        return (err == NHP_ELOCKED) ? 5 : 1;
    }

    size_t index = SIZE_MAX;
    err = inst_find_patch_index(&inst, argv[2], &index);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: %s\n", err_get());
        inst_lock_release(&lock);
        inst_free(&inst);
        cfg_app_free(&app);
        return 1;
    }

    stack_conflict conflict = {0};
    char *diff_path = NULL;
    err = stack_edit_refresh(&inst, index, &conflict, &diff_path);
    if (err == NHP_ECONFLICT) {
        print_patch_conflict(&conflict);
        stack_conflict_free(&conflict);
        inst_lock_release(&lock);
        inst_free(&inst);
        cfg_app_free(&app);
        return 3;
    }
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not refresh %s: %s\n", argv[2], err_get());
        inst_lock_release(&lock);
        inst_free(&inst);
        cfg_app_free(&app);
        return 1;
    }
    (void)printf("Refreshed %s.\n", argv[2]);

    bool is_local = strncmp(argv[2], "local-", 6) == 0;
    if (is_local) {
        char *new_version = NULL;
        nhp_err lib_err =
            lib_add_version(argv[2], name, author, target, version, diff_path, &new_version);
        if (lib_err == NHP_OK) {
            (void)printf("Saved as %s version %s in the library.\n", argv[2], new_version);
        } else {
            (void)fprintf(stderr,
                          "nhpm: refreshed on disk, but could not save a new library "
                          "version: %s\n",
                          err_get());
        }
        free(new_version);
    } else {
        /* docs/features.md 12.4's "L": nhpm doesn't own this id's
         * version history, so the refreshed result is forked into its
         * own local- entry instead of being written back into it. */
        lib_patch original;
        nhp_err find_err = lib_find(argv[2], &original);
        if (find_err != NHP_OK) {
            (void)fprintf(stderr,
                          "nhpm: refreshed on disk, but could not look up %s to fork it: %s\n",
                          argv[2], err_get());
        } else {
            char *new_id = NULL;
            char *new_version = NULL;
            nhp_err fork_err = lib_fork_as_local(argv[2], inst.patches[index].version,
                                                 original.name, original.author, original.target,
                                                 diff_path, &new_id, &new_version);
            if (fork_err == NHP_OK) {
                (void)printf("Saved as %s version %s in the library.\n", new_id, new_version);
            } else {
                (void)fprintf(stderr,
                              "nhpm: refreshed on disk, but could not save it as a local "
                              "variant: %s\n",
                              err_get());
            }
            free(new_id);
            free(new_version);
            lib_patch_free(&original);
        }
    }

    free(diff_path);
    inst_lock_release(&lock);
    inst_free(&inst);
    cfg_app_free(&app);
    return 0;
}

/* docs/features.md section 12.4's "N" (New patch): a brand-new, empty
 * patch, ready to edit -- 'o'/track-file and 'R'/edit-refresh follow. */
static int cmd_new_patch(int argc, char **argv)
{
    if (argc < 3) {
        (void)fprintf(stderr, "usage: nhpm new-patch <name> [--inst <name>]\n");
        return 1;
    }
    const char *name = argv[2];

    inst_instance inst;
    inst_lock lock;
    nhp_err err = resolve_instance_locked(argc, argv, 3, &inst, &lock);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: %s\n", err_get());
        return (err == NHP_ELOCKED) ? 5 : 1;
    }

    char *id = lib_make_local_id(name);
    if (id == NULL) {
        (void)fprintf(stderr, "nhpm: out of memory\n");
        inst_lock_release(&lock);
        inst_free(&inst);
        return 1;
    }

    size_t index = SIZE_MAX;
    err = stack_new_patch(&inst, id, name, &index);
    free(id);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not create %s: %s\n", name, err_get());
        inst_lock_release(&lock);
        inst_free(&inst);
        return 1;
    }

    char *src_dir = inst_source_full_path(&inst);
    (void)printf("%s is on top -- edit files under %s (use 'nhpm track <path>' first for any "
                 "brand-new file), then run 'nhpm edit-refresh %s'.\n",
                 inst.patches[index].id,
                 (src_dir != NULL) ? src_dir : "the instance's source folder",
                 inst.patches[index].id);
    free(src_dir);
    inst_lock_release(&lock);
    inst_free(&inst);
    return 0;
}

/* docs/features.md 12.4 step 2's "files opened with o are tracked
 * automatically": tells quilt to track a file that isn't already part
 * of the patch currently on top, so edit-refresh picks it up. */
static int cmd_track(int argc, char **argv)
{
    if (argc < 3) {
        (void)fprintf(stderr, "usage: nhpm track <path> [--inst <name>]\n");
        return 1;
    }
    const char *path = argv[2];

    inst_instance inst;
    inst_lock lock;
    nhp_err err = resolve_instance_locked(argc, argv, 3, &inst, &lock);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: %s\n", err_get());
        return (err == NHP_ELOCKED) ? 5 : 1;
    }

    err = stack_track_file(&inst, path);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not track %s: %s\n", path, err_get());
        inst_lock_release(&lock);
        inst_free(&inst);
        return 1;
    }
    (void)printf("Tracking %s.\n", path);
    inst_lock_release(&lock);
    inst_free(&inst);
    return 0;
}

/* docs/features.md 10.4: structural check (inst_check()) plus the deeper
 * stack_check_consistency(). `--adopt`/`--restore` resolve drift; without
 * either, check stays read-only and lock-free ("open read-only"). */
static int cmd_check(int argc, char **argv)
{
    bool adopt = false;
    bool restore = false;
    for (int i = 2; i < argc; i++) {
        if (str_eq(argv[i], "--adopt")) {
            adopt = true;
        } else if (str_eq(argv[i], "--restore")) {
            restore = true;
        } else if (str_eq(argv[i], "--inst") && i + 1 < argc) {
            i++;
        }
    }
    if (adopt && restore) {
        (void)fprintf(stderr, "nhpm: --adopt and --restore are mutually exclusive\n");
        return 1;
    }

    inst_instance inst;
    inst_lock lock;
    nhp_err err = (adopt || restore) ? resolve_instance_locked(argc, argv, 2, &inst, &lock)
                                     : resolve_instance(argc, argv, 2, &inst);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: %s\n", err_get());
        return (err == NHP_ELOCKED) ? 5 : 1;
    }

    inst_check_result result;
    err = inst_check(&inst, &result);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: %s\n", err_get());
        if (adopt || restore) {
            inst_lock_release(&lock);
        }
        inst_free(&inst);
        return 1;
    }
    (void)printf("%s: %s\n", inst.name, result.ok ? "ok" : "problem");
    (void)printf("  source folder exists: %s\n", result.source_path_exists ? "yes" : "no");

    bool stack_ok = true;
    if (result.source_path_exists) {
        stack_drift drift;
        err = stack_check_consistency(&inst, &drift);
        if (err != NHP_OK) {
            (void)fprintf(stderr, "nhpm: could not check the patch stack: %s\n", err_get());
            if (adopt || restore) {
                inst_lock_release(&lock);
            }
            inst_free(&inst);
            return 1;
        }
        stack_ok = drift.ok;
        (void)printf("  patch stack: %s\n", drift.ok ? "matches instance.conf" : "drifted");
        if (!drift.ok && drift.detail[0] != '\0') {
            (void)printf("%s", drift.detail);
        }
        stack_drift_free(&drift);
    }

    int exit_code = (result.ok && stack_ok) ? 0 : 1;

    if (adopt && !stack_ok) {
        err = stack_adopt(&inst);
        if (err == NHP_OK) {
            (void)printf("Adopted the folder's state into instance.conf.\n");
            exit_code = 0;
        } else {
            (void)fprintf(stderr, "nhpm: could not adopt: %s\n", err_get());
            exit_code = 1;
        }
    } else if (restore && !stack_ok) {
        stack_conflict conflict = {0};
        err = stack_restore(&inst, &conflict);
        if (err == NHP_OK) {
            (void)printf("Restored the folder to match instance.conf.\n");
            exit_code = 0;
        } else if (err == NHP_ECONFLICT) {
            print_patch_conflict(&conflict);
            stack_conflict_free(&conflict);
            exit_code = 3;
        } else {
            (void)fprintf(stderr, "nhpm: could not restore: %s\n", err_get());
            exit_code = 1;
        }
    } else if ((adopt || restore) && stack_ok) {
        (void)printf("Nothing to %s -- the folder already matches instance.conf.\n",
                     adopt ? "adopt" : "restore");
    }

    if (adopt || restore) {
        inst_lock_release(&lock);
    }
    inst_free(&inst);
    return exit_code;
}

static int cmd_status(int argc, char **argv)
{
    bool check = false;
    for (int i = 0; i < argc; i++) {
        if (str_eq(argv[i], "--check")) {
            check = true;
        }
    }

    inst_instance inst;
    nhp_err err = resolve_instance(argc, argv, 2, &inst);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: %s\n", err_get());
        return 1;
    }

    size_t enabled_count = 0;
    for (size_t i = 0; i < inst.patch_count; i++) {
        if (inst.patches[i].enabled) {
            enabled_count++;
        }
    }
    (void)printf("Instance:   %s\n", inst.name);
    (void)printf("Source:     NetHack %s\n", inst.source);
    (void)printf("Recipe:     %s\n", inst.recipe);
    (void)printf("Patches:    %zu enabled, %zu total\n", enabled_count, inst.patch_count);
    (void)printf("Last built: %s%s\n", (inst.last_built[0] != '\0') ? inst.last_built : "never",
                 build_is_stale(&inst) ? " (out of date)" : "");

    if (check) {
        stack_check_result *results = NULL;
        size_t result_count = 0;
        nhp_err check_err = stack_check_disabled(&inst, &results, &result_count);
        if (check_err != NHP_OK) {
            (void)fprintf(stderr, "nhpm: could not check disabled patches: %s\n", err_get());
        } else if (result_count == 0) {
            (void)printf("\nNo disabled patches to check.\n");
        } else {
            (void)printf("\nDisabled patches (docs/features.md 11.7):\n");
            for (size_t i = 0; i < result_count; i++) {
                const inst_patch *p = &inst.patches[results[i].index];
                if (results[i].hunks_failed == 0) {
                    (void)printf("  %-20s %-8s applies cleanly (-p%d)\n", p->id, p->version,
                                 results[i].strip);
                } else {
                    (void)printf("  %-20s %-8s %d of %d hunks would fail (-p%d)\n", p->id,
                                 p->version, results[i].hunks_failed, results[i].hunks_total,
                                 results[i].strip);
                }
            }
        }
        stack_check_result_free(results);

        stack_shared_file *shared = NULL;
        size_t shared_count = 0;
        nhp_err shared_err = stack_find_shared_files(&inst, &shared, &shared_count);
        if (shared_err != NHP_OK) {
            (void)fprintf(stderr, "nhpm: could not check for shared files: %s\n", err_get());
        } else if (shared_count > 0) {
            (void)printf("\nFiles touched by more than one enabled patch (docs/features.md "
                         "11.9):\n");
            for (size_t i = 0; i < shared_count; i++) {
                (void)printf("  %-30s", shared[i].path);
                for (size_t j = 0; j < shared[i].patch_count; j++) {
                    (void)printf("%s%s", (j > 0) ? ", " : " ", shared[i].patch_ids[j]);
                }
                (void)printf("\n");
            }
        }
        stack_shared_files_free(shared, shared_count);
    }

    inst_free(&inst);
    return 0;
}

static bool print_build_line(void *user_data, bool is_stderr, const char *line)
{
    (void)user_data;
    (void)fprintf(is_stderr ? stderr : stdout, "%s\n", line);
    return true; /* never cancel */
}

static int cmd_build(int argc, char **argv)
{
    inst_instance inst;
    inst_lock lock;
    nhp_err err = resolve_instance_locked(argc, argv, 2, &inst, &lock);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: %s\n", err_get());
        return (err == NHP_ELOCKED) ? 5 : 1;
    }

    nhp_err build_err = build_run(&inst, print_build_line, NULL);
    inst_lock_release(&lock);
    if (build_err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: build failed: %s\n", err_get());
        inst_free(&inst);
        return 4;
    }
    (void)printf("Built %s.\n", inst.name);
    inst_free(&inst);
    return 0;
}

static int cmd_play(int argc, char **argv)
{
    inst_instance inst;
    inst_lock lock;
    nhp_err err = resolve_instance_locked(argc, argv, 2, &inst, &lock);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: %s\n", err_get());
        return (err == NHP_ELOCKED) ? 5 : 1;
    }

    err = build_play(&inst);
    inst_lock_release(&lock);
    inst_free(&inst);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not play: %s\n", err_get());
        return 1;
    }
    return 0;
}

/* "2026-11-15T20:14:03Z" -> "2026-11-15", for filenames and the
 * "new version each time" library version of a combined patch
 * (docs/features.md 12.2). The caller frees the result. */
static char *today_date(void)
{
    char *iso = time_now_iso8601();
    if (iso != NULL && strlen(iso) >= 10) {
        iso[10] = '\0';
    }
    return iso;
}

/* `~/.local/share/nhpm/exports`, created if needed. NULL on OOM or if
 * it couldn't be created. */
static char *exports_dir(void)
{
    char *data_dir = cfg_xdg_data_dir();
    char *dir = (data_dir != NULL) ? path_join(data_dir, "exports") : NULL;
    free(data_dir);
    if (dir != NULL) {
        (void)fs_mkdir_p(dir, 0700);
    }
    return dir;
}

/* Consumes argv[start..] as a space-separated file list, up to the next
 * "--" option or end of argv. *out_next is the first index not consumed.
 * Returns a pointer into argv itself (not a copy). */
static const char **collect_files(int argc, char **argv, int start, size_t *out_count,
                                  int *out_next)
{
    int end = start;
    while (end < argc && strncmp(argv[end], "--", 2) != 0) {
        end++;
    }
    *out_count = (size_t)(end - start);
    *out_next = end;
    return (const char **)(void *)&argv[start];
}

static int cmd_combined(int argc, char **argv)
{
    const char *out_path = NULL;
    const char *flatten_name = NULL;
    bool to_library = false;

    for (int i = 2; i < argc; i++) {
        if (str_eq(argv[i], "--out") && i + 1 < argc) {
            out_path = argv[++i];
        } else if (str_eq(argv[i], "--to-library")) {
            to_library = true;
        } else if (str_eq(argv[i], "--flatten") && i + 1 < argc) {
            flatten_name = argv[++i];
        } else if (str_eq(argv[i], "--inst") && i + 1 < argc) {
            i++; /* consumed by resolve_instance() below */
        } else {
            (void)fprintf(stderr,
                          "usage: nhpm combined [--out FILE] [--to-library] [--flatten NEW] "
                          "[--inst <name>]\n");
            return 1;
        }
    }

    inst_instance inst;
    nhp_err err = resolve_instance(argc, argv, 2, &inst);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: %s\n", err_get());
        return 1;
    }

    cfg_app app;
    cfg_app_defaults(&app);
    (void)cfg_app_load(&app);

    mkp_combined_result result;
    err = mkp_combined(&inst, NULL, 0, app.ignore, &result);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not build the combined patch: %s\n", err_get());
        cfg_app_free(&app);
        inst_free(&inst);
        return 1;
    }

    char *default_path = NULL;
    if (out_path == NULL) {
        char *dir = exports_dir();
        char *date = today_date();
        buf b;
        buf_init(&b);
        buf_append_fmt(&b, "%s-combined-%s.diff", inst.name, (date != NULL) ? date : "");
        char *filename = buf_take(&b);
        default_path = (dir != NULL) ? path_join(dir, filename) : NULL;
        free(dir);
        free(date);
        free(filename);
    }
    const char *write_path = (out_path != NULL) ? out_path : default_path;
    int exit_code = 0;
    if (write_path == NULL) {
        (void)fprintf(stderr, "nhpm: could not work out where to write the combined patch\n");
        exit_code = 1;
    } else {
        err = fs_write_atomic(write_path, result.diff, strlen(result.diff));
        if (err == NHP_OK) {
            (void)printf("Wrote %s (%zu files).\n", write_path, result.file_count);
        } else {
            (void)fprintf(stderr, "nhpm: could not write %s: %s\n", write_path, err_get());
            exit_code = 1;
        }
    }

    if (to_library) {
        char *date = today_date();
        buf nb;
        buf_init(&nb);
        buf_append_fmt(&nb, "%s-combined", inst.name);
        char *name = buf_take(&nb);
        char *tmp_path = NULL;
        nhp_err lib_err =
            fs_write_temp_file("nhpm-combined-lib", result.diff, strlen(result.diff), &tmp_path);
        char *id = NULL;
        if (lib_err == NHP_OK) {
            lib_err = lib_import_local(tmp_path, name, "", inst.source, date, &id);
        }
        if (lib_err == NHP_OK) {
            (void)printf("Added to the library as %s %s.\n", id, (date != NULL) ? date : "1.0");
        } else {
            (void)fprintf(stderr, "nhpm: could not add to the library: %s\n", err_get());
            exit_code = 1;
        }
        free(id);
        if (tmp_path != NULL) {
            (void)unlink(tmp_path);
        }
        free(tmp_path);
        free(name);
        free(date);
    }

    if (flatten_name != NULL) {
        inst_instance new_inst;
        nhp_err flat_err = inst_new(flatten_name, inst.source, inst.recipe, &new_inst);
        char *tmp_path = NULL;
        char *id = NULL;
        if (flat_err == NHP_OK) {
            flat_err = fs_write_temp_file("nhpm-combined-flatten", result.diff, strlen(result.diff),
                                          &tmp_path);
        }
        if (flat_err == NHP_OK) {
            flat_err = lib_import_local(tmp_path, flatten_name, "", inst.source, "1.0", &id);
        }
        if (flat_err == NHP_OK) {
            size_t idx = 0;
            stack_conflict conflict = {0};
            char sha[65];
            sha256_hex(result.diff, strlen(result.diff), sha);
            flat_err =
                stack_add(&new_inst, id, "1.0", flatten_name, tmp_path, sha, true, &idx, &conflict);
            if (flat_err == NHP_ECONFLICT) {
                stack_conflict_free(&conflict);
            }
        }
        if (flat_err == NHP_OK) {
            (void)printf("Created %s with the combined patch applied.\n", flatten_name);
            inst_free(&new_inst);
        } else {
            (void)fprintf(stderr, "nhpm: could not flatten into %s: %s\n", flatten_name, err_get());
            exit_code = 1;
        }
        free(id);
        if (tmp_path != NULL) {
            (void)unlink(tmp_path);
        }
        free(tmp_path);
    }

    free(default_path);
    mkp_combined_result_free(&result);
    cfg_app_free(&app);
    inst_free(&inst);
    return exit_code;
}

static const char *status_letter(mkp_status status)
{
    switch (status) {
        case MKP_MODIFIED:
            return "M";
        case MKP_ADDED:
            return "A";
        case MKP_DELETED:
            return "D";
        case MKP_BINARY:
        default:
            return "B";
    }
}

static int cmd_changes(int argc, char **argv)
{
    bool show_hidden = false;
    for (int i = 2; i < argc; i++) {
        if (str_eq(argv[i], "--hidden")) {
            show_hidden = true;
        } else if (str_eq(argv[i], "--inst") && i + 1 < argc) {
            i++;
        }
    }

    inst_instance inst;
    nhp_err err = resolve_instance(argc, argv, 2, &inst);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: %s\n", err_get());
        return 1;
    }

    cfg_app app;
    cfg_app_defaults(&app);
    (void)cfg_app_load(&app);

    mkp_file *files = NULL;
    size_t count = 0;
    err = mkp_changes(&inst, app.ignore, &files, &count);
    cfg_app_free(&app);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not list changes: %s\n", err_get());
        inst_free(&inst);
        return 1;
    }

    size_t hidden_count = 0;
    for (size_t i = 0; i < count; i++) {
        if (files[i].hidden) {
            hidden_count++;
            if (!show_hidden) {
                continue;
            }
        }
        (void)printf("[%c] %s %-40s%s%s\n", files[i].default_selected ? 'x' : ' ',
                     status_letter(files[i].status), files[i].path,
                     (files[i].status == MKP_BINARY) ? "  (binary, can't be included)" : "",
                     files[i].hidden ? "  (hidden: build output)" : "");
    }
    if (hidden_count > 0) {
        (void)printf("%zu file%s changed (%zu hidden as build output -- %s)\n", count,
                     (count == 1) ? "" : "s", hidden_count,
                     show_hidden ? "shown above" : "pass --hidden to show");
    } else {
        (void)printf("%zu file%s changed\n", count, (count == 1) ? "" : "s");
    }

    mkp_file_list_free(files, count);
    inst_free(&inst);
    return 0;
}

static int cmd_capture(int argc, char **argv)
{
    if (argc < 3) {
        (void)fprintf(stderr, "usage: nhpm capture <name> [--version V] [--author A] [--target T]\n"
                              "               [--desc TEXT] [--long-desc TEXT] [--files F...] "
                              "[--inst <name>]\n");
        return 1;
    }
    const char *name = argv[2];
    const char *version = "";
    const char *author = NULL;
    const char *target = "";
    const char *short_desc = "";
    const char *long_desc = "";
    const char **files = NULL;
    size_t file_count = 0;

    for (int i = 3; i < argc; i++) {
        if (str_eq(argv[i], "--version") && i + 1 < argc) {
            version = argv[++i];
        } else if (str_eq(argv[i], "--author") && i + 1 < argc) {
            author = argv[++i];
        } else if (str_eq(argv[i], "--target") && i + 1 < argc) {
            target = argv[++i];
        } else if (str_eq(argv[i], "--desc") && i + 1 < argc) {
            short_desc = argv[++i];
        } else if (str_eq(argv[i], "--long-desc") && i + 1 < argc) {
            long_desc = argv[++i];
        } else if (str_eq(argv[i], "--inst") && i + 1 < argc) {
            i++;
        } else if (str_eq(argv[i], "--files")) {
            int next = i + 1;
            files = collect_files(argc, argv, next, &file_count, &next);
            i = next - 1;
        } else {
            (void)fprintf(stderr, "nhpm: unknown capture option '%s'\n", argv[i]);
            return 1;
        }
    }

    inst_instance inst;
    inst_lock lock;
    nhp_err err = resolve_instance_locked(argc, argv, 3, &inst, &lock);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: %s\n", err_get());
        return (err == NHP_ELOCKED) ? 5 : 1;
    }

    cfg_app app;
    cfg_app_defaults(&app);
    (void)cfg_app_load(&app);
    if (author == NULL) {
        author = app.author_name;
    }

    const char **selected = files;
    size_t selected_count = file_count;
    mkp_file *computed = NULL;
    size_t computed_count = 0;
    char **owned_selected = NULL;

    if (selected_count == 0) {
        err = mkp_changes(&inst, app.ignore, &computed, &computed_count);
        if (err != NHP_OK) {
            (void)fprintf(stderr, "nhpm: could not determine changes: %s\n", err_get());
            cfg_app_free(&app);
            inst_lock_release(&lock);
            inst_free(&inst);
            return 1;
        }
        owned_selected =
            (computed_count > 0) ? malloc(computed_count * sizeof(*owned_selected)) : NULL;
        for (size_t i = 0; i < computed_count; i++) {
            if (computed[i].default_selected) {
                owned_selected[selected_count++] = computed[i].path;
            }
        }
        selected = (const char **)(void *)owned_selected;
    }

    mkp_patch_meta meta = {
        .name = name,
        .version = version,
        .author = author,
        .target = target,
        .short_desc = short_desc,
        .long_desc = long_desc,
    };
    char *out_id = NULL;
    err = mkp_capture(&inst, &meta, (const char *const *)selected, selected_count, &out_id);
    cfg_app_free(&app);
    free(owned_selected);
    mkp_file_list_free(computed, computed_count);

    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not capture: %s\n", err_get());
        inst_lock_release(&lock);
        inst_free(&inst);
        return (err == NHP_ECONFLICT) ? 3 : 1;
    }
    (void)printf("Captured %s as %s %s.\n", name, out_id, version[0] != '\0' ? version : "1.0");
    free(out_id);
    inst_lock_release(&lock);
    inst_free(&inst);
    return 0;
}

static int cmd_mkpatch(int argc, char **argv)
{
    if (argc < 4) {
        (void)fprintf(
            stderr, "usage: nhpm mkpatch <folder> <name> [--from base|folder:P|tarball:P|git:REF]\n"
                    "               [--version V] [--author A] [--target T] [--desc TEXT]\n"
                    "               [--long-desc TEXT] [--files F...]\n");
        return 1;
    }
    const char *folder = argv[2];
    const char *name = argv[3];
    const char *from = "base";
    const char *version = "";
    const char *author = NULL;
    const char *target = "";
    const char *short_desc = "";
    const char *long_desc = "";
    const char **files = NULL;
    size_t file_count = 0;

    for (int i = 4; i < argc; i++) {
        if (str_eq(argv[i], "--from") && i + 1 < argc) {
            from = argv[++i];
        } else if (str_eq(argv[i], "--version") && i + 1 < argc) {
            version = argv[++i];
        } else if (str_eq(argv[i], "--author") && i + 1 < argc) {
            author = argv[++i];
        } else if (str_eq(argv[i], "--target") && i + 1 < argc) {
            target = argv[++i];
        } else if (str_eq(argv[i], "--desc") && i + 1 < argc) {
            short_desc = argv[++i];
        } else if (str_eq(argv[i], "--long-desc") && i + 1 < argc) {
            long_desc = argv[++i];
        } else if (str_eq(argv[i], "--files")) {
            int next = i + 1;
            files = collect_files(argc, argv, next, &file_count, &next);
            i = next - 1;
        } else {
            (void)fprintf(stderr, "nhpm: unknown mkpatch option '%s'\n", argv[i]);
            return 1;
        }
    }

    mkp_base_spec base;
    if (str_eq(from, "base")) {
        base.kind = MKP_BASE_AUTO;
        base.path_or_ref = "";
    } else if (strncmp(from, "folder:", 7) == 0) {
        base.kind = MKP_BASE_FOLDER;
        base.path_or_ref = from + 7;
    } else if (strncmp(from, "tarball:", 8) == 0) {
        base.kind = MKP_BASE_TARBALL;
        base.path_or_ref = from + 8;
    } else if (strncmp(from, "git:", 4) == 0) {
        base.kind = MKP_BASE_GIT_REF;
        base.path_or_ref = from + 4;
    } else {
        (void)fprintf(stderr,
                      "nhpm: bad --from '%s' (want base, folder:<path>, tarball:<path>, or "
                      "git:<ref>)\n",
                      from);
        return 1;
    }

    cfg_app app;
    cfg_app_defaults(&app);
    (void)cfg_app_load(&app);
    if (author == NULL) {
        author = app.author_name;
    }

    mkp_patch_meta meta = {
        .name = name,
        .version = version,
        .author = author,
        .target = target,
        .short_desc = short_desc,
        .long_desc = long_desc,
    };
    char *out_id = NULL;
    nhp_err err = mkp_from_folder(folder, &base, &meta, app.ignore, (const char *const *)files,
                                  file_count, &out_id);
    cfg_app_free(&app);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not make a patch from %s: %s\n", folder, err_get());
        return 1;
    }
    (void)printf("Created %s %s in the library.\n", out_id, version[0] != '\0' ? version : "1.0");
    free(out_id);
    return 0;
}

static int cmd_export_patch(int argc, char **argv)
{
    if (argc < 3) {
        (void)fprintf(stderr, "usage: nhpm export-patch <id>[@version] [FILE]\n");
        return 1;
    }
    char id[256];
    char version_buf[64];
    const char *version = NULL;
    const char *at = strchr(argv[2], '@');
    if (at != NULL) {
        size_t n = (size_t)(at - argv[2]);
        if (n >= sizeof(id)) {
            n = sizeof(id) - 1;
        }
        memcpy(id, argv[2], n);
        id[n] = '\0';
        (void)snprintf(version_buf, sizeof(version_buf), "%s", at + 1);
        version = version_buf;
    } else {
        (void)snprintf(id, sizeof(id), "%s", argv[2]);
    }

    char *out_path = NULL;
    if (argc > 3) {
        out_path = str_dup(argv[3]);
    } else {
        lib_patch patch;
        nhp_err find_err = lib_find(id, &patch);
        if (find_err != NHP_OK) {
            (void)fprintf(stderr, "nhpm: %s\n", err_get());
            return 1;
        }
        const char *use_version = version;
        if (use_version == NULL && patch.version_count > 0) {
            use_version = patch.versions[patch.version_count - 1].version;
        }
        char *dir = exports_dir();
        buf b;
        buf_init(&b);
        buf_append_fmt(&b, "%s-%s.diff", id, (use_version != NULL) ? use_version : "1.0");
        char *filename = buf_take(&b);
        out_path = (dir != NULL) ? path_join(dir, filename) : NULL;
        free(dir);
        free(filename);
        lib_patch_free(&patch);
    }
    if (out_path == NULL) {
        (void)fprintf(stderr, "nhpm: could not work out where to write %s\n", id);
        return 1;
    }

    nhp_err err = mkp_export_patch(id, version, out_path);
    if (err != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not export %s: %s\n", id, err_get());
        free(out_path);
        return 1;
    }
    (void)printf("Wrote %s\n", out_path);
    free(out_path);
    return 0;
}

static int run_cli(int argc, char **argv)
{
    const char *cmd = argv[1];

    if (str_eq(cmd, "refresh")) {
        return cmd_refresh();
    }
    if (str_eq(cmd, "search")) {
        return cmd_search((argc > 2) ? argv[2] : "");
    }
    if (str_eq(cmd, "info")) {
        if (argc < 3) {
            (void)fprintf(stderr, "usage: nhpm info <id>\n");
            return 1;
        }
        return cmd_info(argv[2]);
    }
    if (str_eq(cmd, "download")) {
        if (argc < 3) {
            (void)fprintf(stderr, "usage: nhpm download <id>\n");
            return 1;
        }
        return cmd_download(argv[2]);
    }
    if (str_eq(cmd, "library")) {
        return cmd_library((argc > 2) ? argv[2] : "");
    }
    if (str_eq(cmd, "import")) {
        return cmd_import(argc, argv);
    }
    if (str_eq(cmd, "rm")) {
        if (argc < 3) {
            (void)fprintf(stderr, "usage: nhpm rm <id> [version] [--yes]\n");
            return 1;
        }
        bool assume_yes = false;
        const char *version = NULL;
        for (int i = 3; i < argc; i++) {
            if (str_eq(argv[i], "--yes")) {
                assume_yes = true;
            } else if (version == NULL) {
                version = argv[i];
            }
        }
        return cmd_rm(argv[2], version, assume_yes);
    }
    if (str_eq(cmd, "source")) {
        return cmd_source(argc, argv);
    }
    if (str_eq(cmd, "inst")) {
        return cmd_inst(argc, argv);
    }
    if (str_eq(cmd, "enable")) {
        return cmd_enable(argc, argv);
    }
    if (str_eq(cmd, "disable")) {
        return cmd_disable(argc, argv);
    }
    if (str_eq(cmd, "move")) {
        return cmd_move(argc, argv);
    }
    if (str_eq(cmd, "undo")) {
        return cmd_undo(argc, argv);
    }
    if (str_eq(cmd, "edit-begin")) {
        return cmd_edit_begin(argc, argv);
    }
    if (str_eq(cmd, "edit-refresh")) {
        return cmd_edit_refresh(argc, argv);
    }
    if (str_eq(cmd, "new-patch")) {
        return cmd_new_patch(argc, argv);
    }
    if (str_eq(cmd, "track")) {
        return cmd_track(argc, argv);
    }
    if (str_eq(cmd, "check")) {
        return cmd_check(argc, argv);
    }
    if (str_eq(cmd, "status")) {
        return cmd_status(argc, argv);
    }
    if (str_eq(cmd, "build")) {
        return cmd_build(argc, argv);
    }
    if (str_eq(cmd, "play")) {
        return cmd_play(argc, argv);
    }
    if (str_eq(cmd, "combined")) {
        return cmd_combined(argc, argv);
    }
    if (str_eq(cmd, "changes")) {
        return cmd_changes(argc, argv);
    }
    if (str_eq(cmd, "capture")) {
        return cmd_capture(argc, argv);
    }
    if (str_eq(cmd, "mkpatch")) {
        return cmd_mkpatch(argc, argv);
    }
    if (str_eq(cmd, "export-patch")) {
        return cmd_export_patch(argc, argv);
    }

    (void)fprintf(stderr, "nhpm: unknown or not-yet-implemented command '%s'\n", cmd);
    print_usage(stderr);
    return 1;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (str_eq(argv[i], "--version") || str_eq(argv[i], "-V")) {
            (void)printf("nhpm %s\n", NHPM_VERSION);
            return 0;
        }
        if (str_eq(argv[i], "--help") || str_eq(argv[i], "-h")) {
            print_usage(stdout);
            return 0;
        }
    }

    /* Pulled out before deciding CLI vs UI dispatch below, so "nhpm --debug"
     * alone still launches the interface instead of being treated as an
     * unknown CLI command. */
    bool debug = false;
    for (int i = 1; i < argc; i++) {
        if (str_eq(argv[i], "--debug")) {
            debug = true;
            for (int j = i; j + 1 < argc; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            break;
        }
    }

    if (init_logging() != NHP_OK) {
        (void)fprintf(stderr, "nhpm: could not open the log file: %s\n", err_get());
        /* Not fatal: carry on without a log. */
    }
    log_info("main", "nhpm %s starting", NHPM_VERSION);

    int result;
    if (argc > 1) {
        result = run_cli(argc, argv);
    } else {
        nhp_err err = ui_run(debug);
        if (err != NHP_OK) {
            (void)fprintf(stderr, "nhpm: could not start the interface: %s\n", err_get());
            result = 1;
        } else {
            result = 0;
        }
    }

    log_close();
    return result;
}
