/* instance.c - instances: create, load, save, clone, rename, delete,
 * snapshots, export/import, lock.
 * SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "instance.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "proc.h"
#include "sources.h"
#include "version.h"

/* ---- paths and names ---- */

static char *instances_dir(void)
{
    char *data_dir = cfg_xdg_data_dir();
    if (data_dir == NULL) {
        return NULL;
    }
    char *dir = path_join(data_dir, "instances");
    free(data_dir);
    return dir;
}

char *inst_dir(const char *name)
{
    char *dir = instances_dir();
    if (dir == NULL) {
        return NULL;
    }
    char *result = path_join(dir, name);
    free(dir);
    return result;
}

/* docs/security.md: "instance names are limited to [A-Za-z0-9._-]". */
static bool is_valid_name(const char *name)
{
    if (name == NULL || name[0] == '\0' || str_eq(name, ".") || str_eq(name, "..")) {
        return false;
    }
    for (const char *p = name; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        if (!isalnum(c) && c != '.' && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

/* For filenames (snapshots, orphaned-saves): colons aren't shell-friendly,
 * so this uses dashes instead, matching docs/features.md's own example
 * ("2026-11-15T20-14-03-before-building.conf"). */
static char *now_stamp_for_filename(void)
{
    time_t now = time(NULL);
    struct tm tm_utc;
    (void)gmtime_r(&now, &tm_utc);
    char stamp[32];
    (void)strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H-%M-%S", &tm_utc);
    return str_dup(stamp);
}

/* Moves (not copies) a directory, falling back to copy+remove if `src` and
 * `dst` are on different filesystems. */
static nhp_err move_dir(const char *src, const char *dst)
{
    if (rename(src, dst) == 0) {
        return NHP_OK;
    }
    if (errno != EXDEV) {
        err_set("could not move %s to %s: %s", src, dst, strerror(errno));
        return NHP_EIO;
    }
    nhp_err err = fs_copy_tree(src, dst);
    if (err == NHP_OK) {
        err = fs_remove_tree(src);
    }
    return err;
}

/* True if every entry in `tarball_path` is safe to extract (no absolute
 * paths, no ".." -- docs/security.md). Duplicated from sources.c's
 * identical check rather than shared, to keep the two modules independent. */
static nhp_err check_tar_entries_safe(const char *tarball_path)
{
    const char *argv[] = {"tar", "tf", tarball_path, NULL};
    proc_result result = {0};
    nhp_err err = proc_run(argv, NULL, NULL, &result);
    if (err != NHP_OK) {
        proc_result_free(&result);
        return err;
    }
    if (result.signaled || result.exit_code != 0) {
        err_set("could not list the contents of %s", tarball_path);
        proc_result_free(&result);
        return NHP_EIO;
    }

    nhp_err check_err = NHP_OK;
    const char *p = result.out_data;
    while (*p != '\0' && check_err == NHP_OK) {
        const char *line_start = p;
        while (*p != '\0' && *p != '\n') {
            p++;
        }
        size_t line_len = (size_t)(p - line_start);
        if (*p == '\n') {
            p++;
        }
        if (line_len == 0) {
            continue;
        }
        char *entry = str_ndup(line_start, line_len);
        if (entry == NULL) {
            check_err = NHP_ENOMEM;
            break;
        }
        if (!path_is_safe(entry)) {
            err_set("the archive contains an unsafe path: %s", entry);
            check_err = NHP_EPARSE;
        }
        free(entry);
    }
    proc_result_free(&result);
    return check_err;
}

static char *sanitize_label(const char *label)
{
    buf b;
    buf_init(&b);
    bool pending_dash = false;
    bool any_char = false;
    for (const char *p = label; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c)) {
            if (pending_dash && any_char) {
                buf_append(&b, "-", 1);
            }
            buf_append(&b, (const char *)&c, 1);
            pending_dash = false;
            any_char = true;
        } else {
            pending_dash = true;
        }
    }
    return buf_take(&b);
}

/* ---- freeing ---- */

void inst_patch_free_contents(inst_patch *p)
{
    free(p->id);
    free(p->version);
    free(p->sha256);
    memset(p, 0, sizeof(*p));
}

nhp_err inst_patch_copy(const inst_patch *src, inst_patch *dst)
{
    dst->id = str_dup(src->id);
    dst->version = str_dup(src->version);
    dst->enabled = src->enabled;
    dst->strip = src->strip;
    dst->sha256 = str_dup(src->sha256);
    dst->modified = src->modified;
    if (dst->id == NULL || dst->version == NULL || dst->sha256 == NULL) {
        inst_patch_free_contents(dst);
        return NHP_ENOMEM;
    }
    return NHP_OK;
}

void inst_free(inst_instance *inst)
{
    free(inst->name);
    free(inst->source);
    free(inst->source_path);
    free(inst->created);
    free(inst->modified);
    free(inst->stack_modified);
    free(inst->last_built);
    free(inst->last_played);
    free(inst->notes);
    free(inst->recipe);
    free(inst->make_vars);
    free(inst->custom_commands);
    free(inst->install_dir);
    free(inst->game_name);
    for (size_t i = 0; i < inst->patch_count; i++) {
        inst_patch_free_contents(&inst->patches[i]);
    }
    free(inst->patches);
    memset(inst, 0, sizeof(*inst));
}

void inst_list_free(inst_instance *instances, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        inst_free(&instances[i]);
    }
    free(instances);
}

/* ---- instance.conf ---- */

static bool inst_core_fields_ok(const inst_instance *inst)
{
    return inst->name != NULL && inst->source != NULL && inst->source_path != NULL &&
           inst->created != NULL && inst->modified != NULL && inst->stack_modified != NULL &&
           inst->last_built != NULL && inst->last_played != NULL && inst->notes != NULL &&
           inst->recipe != NULL && inst->make_vars != NULL && inst->custom_commands != NULL &&
           inst->install_dir != NULL && inst->game_name != NULL;
}

static nhp_err parse_instance_conf(const char *text, inst_instance *out)
{
    memset(out, 0, sizeof(*out));

    cfg_doc doc;
    cfg_doc_init(&doc);
    nhp_err err = cfg_doc_parse(text, &doc);
    if (err != NHP_OK) {
        cfg_doc_free(&doc);
        return err;
    }

    const cfg_section *instance_sec = cfg_doc_section(&doc, "instance", 0);
    const cfg_section *build_sec = cfg_doc_section(&doc, "build", 0);

    out->name = str_dup(cfg_section_get(instance_sec, "name", ""));
    out->kind = str_eq(cfg_section_get(instance_sec, "kind", "managed"), "linked") ? INST_LINKED
                                                                                   : INST_MANAGED;
    out->source = str_dup(cfg_section_get(instance_sec, "source", ""));
    out->source_path = str_dup(cfg_section_get(instance_sec, "source_path", ""));
    out->created = str_dup(cfg_section_get(instance_sec, "created", ""));
    out->modified = str_dup(cfg_section_get(instance_sec, "modified", ""));
    out->stack_modified = str_dup(cfg_section_get(instance_sec, "stack_modified", ""));
    out->last_built = str_dup(cfg_section_get(instance_sec, "last_built", ""));
    out->last_played = str_dup(cfg_section_get(instance_sec, "last_played", ""));
    out->notes = str_dup(cfg_section_get(instance_sec, "notes", ""));

    out->recipe = str_dup(cfg_section_get(build_sec, "recipe", "custom"));
    out->make_vars = str_dup(cfg_section_get(build_sec, "make_vars", ""));
    out->custom_commands = str_dup(cfg_section_get(build_sec, "custom_commands", ""));
    out->install_dir = str_dup(cfg_section_get(build_sec, "install_dir", "install"));
    out->game_name = str_dup(cfg_section_get(build_sec, "game_name", "nethack"));

    size_t patch_count = cfg_doc_section_count(&doc, "patch");
    if (patch_count > 0) {
        out->patches = calloc(patch_count, sizeof(*out->patches));
        if (out->patches == NULL) {
            cfg_doc_free(&doc);
            inst_free(out);
            return NHP_ENOMEM;
        }
    }

    nhp_err patch_err = NHP_OK;
    for (size_t i = 0; i < patch_count && patch_err == NHP_OK; i++) {
        const cfg_section *psec = cfg_doc_section(&doc, "patch", i);
        inst_patch *p = &out->patches[i];
        p->id = str_dup(cfg_section_get(psec, "id", ""));
        p->version = str_dup(cfg_section_get(psec, "version", ""));
        p->enabled = cfg_section_get_bool(psec, "enabled", true);
        p->strip = (int)cfg_section_get_long(psec, "strip", -1);
        p->sha256 = str_dup(cfg_section_get(psec, "sha256", ""));
        p->modified = cfg_section_get_bool(psec, "modified", false);
        if (p->id == NULL || p->version == NULL || p->sha256 == NULL) {
            patch_err = NHP_ENOMEM;
        } else {
            out->patch_count++;
        }
    }
    cfg_doc_free(&doc);

    if (patch_err != NHP_OK || !inst_core_fields_ok(out)) {
        inst_free(out);
        return NHP_ENOMEM;
    }
    return NHP_OK;
}

static nhp_err write_instance_section(cfg_doc *doc, const inst_instance *inst)
{
    cfg_section *sec = cfg_doc_add_section(doc, "instance");
    if (sec == NULL) {
        return NHP_ENOMEM;
    }
    nhp_err err = cfg_section_set(sec, "name", inst->name);
    if (err == NHP_OK) {
        err = cfg_section_set(sec, "kind", (inst->kind == INST_LINKED) ? "linked" : "managed");
    }
    if (err == NHP_OK) {
        err = cfg_section_set(sec, "source", inst->source);
    }
    if (err == NHP_OK) {
        err = cfg_section_set(sec, "source_path", inst->source_path);
    }
    if (err == NHP_OK) {
        err = cfg_section_set(sec, "created", inst->created);
    }
    if (err == NHP_OK) {
        err = cfg_section_set(sec, "modified", inst->modified);
    }
    if (err == NHP_OK) {
        err = cfg_section_set(sec, "stack_modified", inst->stack_modified);
    }
    if (err == NHP_OK) {
        err = cfg_section_set(sec, "last_built", inst->last_built);
    }
    if (err == NHP_OK) {
        err = cfg_section_set(sec, "last_played", inst->last_played);
    }
    if (err == NHP_OK) {
        err = cfg_section_set(sec, "notes", inst->notes);
    }
    return err;
}

static nhp_err write_build_section(cfg_doc *doc, const inst_instance *inst)
{
    cfg_section *sec = cfg_doc_add_section(doc, "build");
    if (sec == NULL) {
        return NHP_ENOMEM;
    }
    nhp_err err = cfg_section_set(sec, "recipe", inst->recipe);
    if (err == NHP_OK) {
        err = cfg_section_set(sec, "make_vars", inst->make_vars);
    }
    if (err == NHP_OK) {
        err = cfg_section_set(sec, "custom_commands", inst->custom_commands);
    }
    if (err == NHP_OK) {
        err = cfg_section_set(sec, "install_dir", inst->install_dir);
    }
    if (err == NHP_OK) {
        err = cfg_section_set(sec, "game_name", inst->game_name);
    }
    return err;
}

static nhp_err write_patch_section(cfg_doc *doc, const inst_patch *p)
{
    cfg_section *sec = cfg_doc_add_section(doc, "patch");
    if (sec == NULL) {
        return NHP_ENOMEM;
    }
    char strip_buf[16];
    snprintf(strip_buf, sizeof(strip_buf), "%d", p->strip);

    nhp_err err = cfg_section_set(sec, "id", p->id);
    if (err == NHP_OK) {
        err = cfg_section_set(sec, "version", p->version);
    }
    if (err == NHP_OK) {
        err = cfg_section_set(sec, "enabled", p->enabled ? "yes" : "no");
    }
    if (err == NHP_OK) {
        err = cfg_section_set(sec, "strip", strip_buf);
    }
    if (err == NHP_OK) {
        err = cfg_section_set(sec, "sha256", p->sha256);
    }
    if (err == NHP_OK) {
        err = cfg_section_set(sec, "modified", p->modified ? "yes" : "no");
    }
    return err;
}

static char *format_instance_conf(const inst_instance *inst)
{
    cfg_doc doc;
    cfg_doc_init(&doc);

    nhp_err err = cfg_set(&doc, NULL, "format", "1");
    if (err == NHP_OK) {
        err = write_instance_section(&doc, inst);
    }
    if (err == NHP_OK) {
        err = write_build_section(&doc, inst);
    }
    for (size_t i = 0; i < inst->patch_count && err == NHP_OK; i++) {
        err = write_patch_section(&doc, &inst->patches[i]);
    }

    char *text = (err == NHP_OK) ? cfg_doc_format(&doc) : NULL;
    cfg_doc_free(&doc);
    return text;
}

nhp_err inst_load(const char *name, inst_instance *out)
{
    memset(out, 0, sizeof(*out));

    char *dir = inst_dir(name);
    if (dir == NULL) {
        return NHP_ENOMEM;
    }
    char *conf_path = path_join(dir, "instance.conf");
    free(dir);
    if (conf_path == NULL) {
        return NHP_ENOMEM;
    }

    char *text = NULL;
    size_t len = 0;
    nhp_err err = fs_read_file(conf_path, &text, &len);
    free(conf_path);
    if (err != NHP_OK) {
        if (err == NHP_ENOENT) {
            err_set("instance %s does not exist", name);
        }
        return err;
    }

    err = parse_instance_conf(text, out);
    free(text);
    return err;
}

nhp_err inst_save(inst_instance *inst)
{
    char *dir = inst_dir(inst->name);
    if (dir == NULL) {
        return NHP_ENOMEM;
    }
    char *conf_path = path_join(dir, "instance.conf");
    char *bak_path = path_join(dir, "instance.conf.bak");
    free(dir);
    if (conf_path == NULL || bak_path == NULL) {
        free(conf_path);
        free(bak_path);
        return NHP_ENOMEM;
    }

    /* step 1 (docs/features.md section 10.3): copy the current file to
     * .bak, if there is one yet. */
    if (fs_exists(conf_path)) {
        char *old_data = NULL;
        size_t old_len = 0;
        if (fs_read_file(conf_path, &old_data, &old_len) == NHP_OK) {
            (void)fs_write_atomic(bak_path, old_data, old_len);
            free(old_data);
        }
    }
    free(bak_path);

    char *new_modified = time_now_iso8601();
    if (new_modified == NULL) {
        free(conf_path);
        return NHP_ENOMEM;
    }
    free(inst->modified);
    inst->modified = new_modified;

    char *text = format_instance_conf(inst);
    nhp_err err = (text != NULL) ? fs_write_atomic(conf_path, text, strlen(text)) : NHP_ENOMEM;
    free(text);
    free(conf_path);
    return err;
}

nhp_err inst_list(inst_instance **out_instances, size_t *out_count)
{
    *out_instances = NULL;
    *out_count = 0;

    char *dir = instances_dir();
    if (dir == NULL) {
        return NHP_ENOMEM;
    }

    DIR *dp = opendir(dir);
    if (dp == NULL) {
        if (errno == ENOENT) {
            free(dir);
            return NHP_OK;
        }
        err_set("could not open %s: %s", dir, strerror(errno));
        free(dir);
        return NHP_EIO;
    }

    inst_instance *list = NULL;
    size_t count = 0;
    size_t cap = 0;
    const struct dirent *entry;
    nhp_err err = NHP_OK;

    while (err == NHP_OK && (entry = readdir(dp)) != NULL) {
        if (str_eq(entry->d_name, ".") || str_eq(entry->d_name, "..")) {
            continue;
        }
        char *entry_dir = path_join(dir, entry->d_name);
        if (entry_dir == NULL) {
            err = NHP_ENOMEM;
            break;
        }
        struct stat st;
        bool is_dir = stat(entry_dir, &st) == 0 && S_ISDIR(st.st_mode);
        free(entry_dir);
        if (!is_dir) {
            continue;
        }

        if (count == cap) {
            size_t new_cap = (cap == 0) ? 8 : cap * 2;
            inst_instance *grown = realloc(list, new_cap * sizeof(*grown));
            if (grown == NULL) {
                err = NHP_ENOMEM;
                break;
            }
            list = grown;
            cap = new_cap;
        }

        memset(&list[count], 0, sizeof(list[count]));
        err = inst_load(entry->d_name, &list[count]);
        if (err != NHP_OK) {
            inst_free(&list[count]);
            break;
        }
        count++;
    }
    (void)closedir(dp);
    free(dir);

    if (err != NHP_OK) {
        inst_list_free(list, count);
        return err;
    }
    *out_instances = list;
    *out_count = count;
    return NHP_OK;
}

/* ---- creation ---- */

static nhp_err create_instance_dirs(const char *dir)
{
    nhp_err err = fs_mkdir_p(dir, 0700);
    if (err != NHP_OK) {
        return err;
    }
    char *install_dir = path_join(dir, "install");
    char *snapshots_dir = path_join(dir, "snapshots");
    char *backup_dir = path_join(dir, "backup");
    if (install_dir == NULL || snapshots_dir == NULL || backup_dir == NULL) {
        free(install_dir);
        free(snapshots_dir);
        free(backup_dir);
        return NHP_ENOMEM;
    }
    err = fs_mkdir_p(install_dir, 0700);
    if (err == NHP_OK) {
        err = fs_mkdir_p(snapshots_dir, 0700);
    }
    if (err == NHP_OK) {
        err = fs_mkdir_p(backup_dir, 0700);
    }
    free(install_dir);
    free(snapshots_dir);
    free(backup_dir);
    return err;
}

/* Fills in the fields every new instance (managed or linked) shares.
 * `out` must already have name/kind/source/source_path set. */
static nhp_err fill_new_instance_defaults(inst_instance *out, const char *recipe)
{
    char *created = time_now_iso8601();
    out->created = created;
    out->modified = str_dup(created);
    out->stack_modified = str_dup("");
    out->last_built = str_dup("");
    out->last_played = str_dup("");
    out->notes = str_dup("");
    out->recipe = str_dup((recipe != NULL && recipe[0] != '\0') ? recipe : "custom");
    out->make_vars = str_dup("");
    out->custom_commands = str_dup("");
    out->install_dir = str_dup("install");
    out->game_name = str_dup("nethack");
    out->patches = NULL;
    out->patch_count = 0;
    return inst_core_fields_ok(out) ? NHP_OK : NHP_ENOMEM;
}

nhp_err inst_new(const char *name, const char *source_version, const char *recipe,
                 inst_instance *out)
{
    memset(out, 0, sizeof(*out));
    if (!is_valid_name(name)) {
        err_set("'%s' isn't a valid instance name (use letters, digits, '.', '_', '-')", name);
        return NHP_EINVAL;
    }

    char *dir = inst_dir(name);
    if (dir == NULL) {
        return NHP_ENOMEM;
    }
    if (fs_exists(dir)) {
        err_set("instance %s already exists", name);
        free(dir);
        return NHP_EEXIST;
    }

    char *tree = bsrc_tree_path(source_version);
    if (tree == NULL) {
        free(dir);
        return NHP_ENOMEM;
    }
    if (!fs_exists(tree)) {
        err_set("no base source for %s; import one first", source_version);
        free(tree);
        free(dir);
        return NHP_ENOENT;
    }

    nhp_err err = create_instance_dirs(dir);

    char *src_dir = NULL;
    if (err == NHP_OK) {
        src_dir = path_join(dir, "src");
        err = (src_dir != NULL) ? fs_copy_tree(tree, src_dir) : NHP_ENOMEM;
    }
    free(tree);

    if (err == NHP_OK) {
        /* the base source is read-only; the instance's working copy must
         * not be. */
        const char *argv[] = {"chmod", "-R", "u+w", src_dir, NULL};
        proc_result result = {0};
        (void)proc_run(argv, NULL, NULL, &result);
        proc_result_free(&result);
    }
    free(src_dir);

    if (err != NHP_OK) {
        (void)fs_remove_tree(dir);
        free(dir);
        return err;
    }
    free(dir);

    out->name = str_dup(name);
    out->kind = INST_MANAGED;
    out->source = str_dup(source_version);
    out->source_path = str_dup("src");
    err = fill_new_instance_defaults(out, recipe);
    if (err != NHP_OK) {
        inst_free(out);
        return err;
    }
    return inst_save(out);
}

nhp_err inst_link(const char *name, const char *folder, const char *recipe, inst_instance *out)
{
    memset(out, 0, sizeof(*out));
    if (!is_valid_name(name)) {
        err_set("'%s' isn't a valid instance name (use letters, digits, '.', '_', '-')", name);
        return NHP_EINVAL;
    }
    if (!fs_exists(folder)) {
        err_set("%s does not exist", folder);
        return NHP_ENOENT;
    }

    char *dir = inst_dir(name);
    if (dir == NULL) {
        return NHP_ENOMEM;
    }
    if (fs_exists(dir)) {
        err_set("instance %s already exists", name);
        free(dir);
        return NHP_EEXIST;
    }

    nhp_err err = create_instance_dirs(dir);
    free(dir);
    if (err != NHP_OK) {
        return err;
    }

    ver_version v;
    char *source_str =
        (ver_read_patchlevel(folder, &v) == NHP_OK) ? ver_format(&v) : str_dup("unknown");
    ver_free(&v);

    out->name = str_dup(name);
    out->kind = INST_LINKED;
    out->source = source_str;
    out->source_path = str_dup(folder);
    err = fill_new_instance_defaults(out, recipe);
    if (err != NHP_OK) {
        inst_free(out);
        return err;
    }
    return inst_save(out);
}

/* ---- clone / rename / delete ---- */

nhp_err inst_clone(const char *src_name, const char *new_name, bool copy_saves)
{
    if (!is_valid_name(new_name)) {
        err_set("'%s' isn't a valid instance name (use letters, digits, '.', '_', '-')", new_name);
        return NHP_EINVAL;
    }

    char *src_dir = inst_dir(src_name);
    char *dst_dir = inst_dir(new_name);
    if (src_dir == NULL || dst_dir == NULL) {
        free(src_dir);
        free(dst_dir);
        return NHP_ENOMEM;
    }
    if (!fs_exists(src_dir)) {
        err_set("instance %s does not exist", src_name);
        free(src_dir);
        free(dst_dir);
        return NHP_ENOENT;
    }
    if (fs_exists(dst_dir)) {
        err_set("instance %s already exists", new_name);
        free(src_dir);
        free(dst_dir);
        return NHP_EEXIST;
    }

    nhp_err err = fs_copy_tree(src_dir, dst_dir);
    if (err == NHP_OK && !copy_saves) {
        char *install_dir = path_join(dst_dir, "install");
        if (install_dir != NULL) {
            (void)fs_remove_tree(install_dir);
            (void)fs_mkdir_p(install_dir, 0700);
            free(install_dir);
        }
    }
    free(src_dir);
    free(dst_dir);
    if (err != NHP_OK) {
        return err;
    }

    inst_instance cloned;
    err = inst_load(new_name, &cloned);
    if (err != NHP_OK) {
        return err;
    }
    free(cloned.name);
    cloned.name = str_dup(new_name);
    char *stamp = time_now_iso8601();
    free(cloned.created);
    cloned.created = stamp;
    free(cloned.modified);
    cloned.modified = str_dup(stamp);
    if (cloned.name == NULL || cloned.created == NULL || cloned.modified == NULL) {
        inst_free(&cloned);
        return NHP_ENOMEM;
    }
    err = inst_save(&cloned);
    inst_free(&cloned);
    return err;
}

nhp_err inst_rename(const char *old_name, const char *new_name)
{
    if (!is_valid_name(new_name)) {
        err_set("'%s' isn't a valid instance name (use letters, digits, '.', '_', '-')", new_name);
        return NHP_EINVAL;
    }

    char *old_dir = inst_dir(old_name);
    char *new_dir = inst_dir(new_name);
    if (old_dir == NULL || new_dir == NULL) {
        free(old_dir);
        free(new_dir);
        return NHP_ENOMEM;
    }
    if (!fs_exists(old_dir)) {
        err_set("instance %s does not exist", old_name);
        free(old_dir);
        free(new_dir);
        return NHP_ENOENT;
    }
    if (fs_exists(new_dir)) {
        err_set("instance %s already exists", new_name);
        free(old_dir);
        free(new_dir);
        return NHP_EEXIST;
    }

    inst_lock lock;
    pid_t other_pid = 0;
    nhp_err err = inst_lock_acquire(old_name, &lock, &other_pid);
    if (err != NHP_OK) {
        free(old_dir);
        free(new_dir);
        return err; /* NHP_ELOCKED if another nhpm has it open */
    }

    if (rename(old_dir, new_dir) != 0) {
        err_set("could not rename %s to %s: %s", old_dir, new_dir, strerror(errno));
        err = NHP_EIO;
    }
    inst_lock_release(&lock);
    free(old_dir);
    free(new_dir);
    if (err != NHP_OK) {
        return err;
    }

    inst_instance inst;
    err = inst_load(new_name, &inst);
    if (err != NHP_OK) {
        return err;
    }
    free(inst.name);
    inst.name = str_dup(new_name);
    if (inst.name == NULL) {
        inst_free(&inst);
        return NHP_ENOMEM;
    }
    err = inst_save(&inst);
    inst_free(&inst);
    return err;
}

nhp_err inst_delete(const char *name, bool keep_saves)
{
    char *dir = inst_dir(name);
    if (dir == NULL) {
        return NHP_ENOMEM;
    }
    if (!fs_exists(dir)) {
        err_set("instance %s does not exist", name);
        free(dir);
        return NHP_ENOENT;
    }

    nhp_err err = NHP_OK;
    if (keep_saves) {
        char *install_dir = path_join(dir, "install");
        if (install_dir != NULL && fs_exists(install_dir)) {
            char *data_dir = cfg_xdg_data_dir();
            char *orphaned_root = (data_dir != NULL) ? path_join(data_dir, "orphaned-saves") : NULL;
            free(data_dir);
            char *stamp = now_stamp_for_filename();

            char *dest_name = NULL;
            if (stamp != NULL) {
                buf b;
                buf_init(&b);
                if (buf_append_fmt(&b, "%s-%s", name, stamp)) {
                    dest_name = buf_take(&b);
                } else {
                    buf_free(&b);
                }
            }
            free(stamp);

            char *dest_dir = (orphaned_root != NULL && dest_name != NULL)
                                 ? path_join(orphaned_root, dest_name)
                                 : NULL;
            free(dest_name);

            if (orphaned_root == NULL || dest_dir == NULL) {
                err = NHP_ENOMEM;
            } else {
                err = fs_mkdir_p(orphaned_root, 0700);
                if (err == NHP_OK) {
                    err = move_dir(install_dir, dest_dir);
                }
            }
            free(orphaned_root);
            free(dest_dir);
        }
        free(install_dir);
    }

    if (err == NHP_OK) {
        err = fs_remove_tree(dir);
    }
    free(dir);
    return err;
}

/* ---- snapshots ---- */

void inst_snapshot_free(inst_snapshot *s)
{
    free(s->filename);
    free(s->label);
    free(s->created);
    memset(s, 0, sizeof(*s));
}

void inst_snapshot_list_free(inst_snapshot *snapshots, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        inst_snapshot_free(&snapshots[i]);
    }
    free(snapshots);
}

nhp_err inst_snapshot_create(const char *name, const char *label, char **out_filename)
{
    *out_filename = NULL;

    char *dir = inst_dir(name);
    if (dir == NULL) {
        return NHP_ENOMEM;
    }
    char *conf_path = path_join(dir, "instance.conf");
    char *snapshots_dir = path_join(dir, "snapshots");
    free(dir);
    if (conf_path == NULL || snapshots_dir == NULL) {
        free(conf_path);
        free(snapshots_dir);
        return NHP_ENOMEM;
    }
    if (!fs_exists(conf_path)) {
        err_set("instance %s does not exist", name);
        free(conf_path);
        free(snapshots_dir);
        return NHP_ENOENT;
    }

    nhp_err err = fs_mkdir_p(snapshots_dir, 0700);

    char *data = NULL;
    size_t len = 0;
    if (err == NHP_OK) {
        err = fs_read_file(conf_path, &data, &len);
    }
    free(conf_path);

    char *filename = NULL;
    if (err == NHP_OK) {
        char *stamp = now_stamp_for_filename();
        char *label_slug = (label != NULL && label[0] != '\0') ? sanitize_label(label) : NULL;

        buf b;
        buf_init(&b);
        bool ok = (stamp != NULL) && buf_append_str(&b, stamp);
        if (ok && label_slug != NULL && label_slug[0] != '\0') {
            ok = buf_append_fmt(&b, "-%s", label_slug);
        }
        if (ok) {
            ok = buf_append_str(&b, ".conf");
        }
        free(stamp);
        free(label_slug);

        if (ok) {
            filename = buf_take(&b);
        } else {
            buf_free(&b);
            err = NHP_ENOMEM;
        }
    }

    if (err == NHP_OK) {
        char *snapshot_path = path_join(snapshots_dir, filename);
        err = (snapshot_path != NULL) ? fs_write_atomic(snapshot_path, data, len) : NHP_ENOMEM;
        free(snapshot_path);
    }
    free(snapshots_dir);
    free(data);

    if (err == NHP_OK) {
        *out_filename = filename;
    } else {
        free(filename);
    }
    return err;
}

static char *extract_label_from_filename(const char *filename)
{
    size_t len = strlen(filename);
    static const char suffix[] = ".conf";
    size_t suffix_len = strlen(suffix);
    size_t timestamp_len = strlen("2026-11-15T20-14-03");

    if (len <= suffix_len + timestamp_len || strcmp(filename + len - suffix_len, suffix) != 0) {
        return str_dup("");
    }
    size_t stem_len = len - suffix_len;
    const char *after_timestamp = filename + timestamp_len;
    if (*after_timestamp == '-') {
        after_timestamp++;
    }
    size_t label_len = (size_t)(filename + stem_len - after_timestamp);
    return str_ndup(after_timestamp, label_len);
}

static int compare_snapshot_filenames(const void *a, const void *b)
{
    const inst_snapshot *sa = a;
    const inst_snapshot *sb = b;
    return strcmp(sa->filename, sb->filename);
}

static nhp_err build_snapshot_entry(const char *snapshots_dir, const char *filename,
                                    inst_snapshot *out)
{
    memset(out, 0, sizeof(*out));
    out->filename = str_dup(filename);
    out->label = extract_label_from_filename(filename);

    char *full = path_join(snapshots_dir, filename);
    time_t mtime = 0;
    if (full != NULL && fs_mtime(full, &mtime) == NHP_OK) {
        struct tm tm_utc;
        (void)gmtime_r(&mtime, &tm_utc);
        char stamp[32];
        (void)strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
        out->created = str_dup(stamp);
    } else {
        out->created = str_dup("");
    }
    free(full);

    if (out->filename == NULL || out->label == NULL || out->created == NULL) {
        return NHP_ENOMEM;
    }
    return NHP_OK;
}

nhp_err inst_snapshot_list(const char *name, inst_snapshot **out_snapshots, size_t *out_count)
{
    *out_snapshots = NULL;
    *out_count = 0;

    char *dir = inst_dir(name);
    if (dir == NULL) {
        return NHP_ENOMEM;
    }
    char *snapshots_dir = path_join(dir, "snapshots");
    free(dir);
    if (snapshots_dir == NULL) {
        return NHP_ENOMEM;
    }

    DIR *dp = opendir(snapshots_dir);
    if (dp == NULL) {
        if (errno == ENOENT) {
            free(snapshots_dir);
            return NHP_OK;
        }
        err_set("could not open %s: %s", snapshots_dir, strerror(errno));
        free(snapshots_dir);
        return NHP_EIO;
    }

    inst_snapshot *list = NULL;
    size_t count = 0;
    size_t cap = 0;
    const struct dirent *entry;
    nhp_err err = NHP_OK;

    while (err == NHP_OK && (entry = readdir(dp)) != NULL) {
        size_t name_len = strlen(entry->d_name);
        if (name_len <= 5 || strcmp(entry->d_name + name_len - 5, ".conf") != 0) {
            continue;
        }
        if (count == cap) {
            size_t new_cap = (cap == 0) ? 8 : cap * 2;
            inst_snapshot *grown = realloc(list, new_cap * sizeof(*grown));
            if (grown == NULL) {
                err = NHP_ENOMEM;
                break;
            }
            list = grown;
            cap = new_cap;
        }
        err = build_snapshot_entry(snapshots_dir, entry->d_name, &list[count]);
        if (err == NHP_OK) {
            count++;
        }
    }
    (void)closedir(dp);
    free(snapshots_dir);

    if (err != NHP_OK) {
        inst_snapshot_list_free(list, count);
        return err;
    }

    qsort(list, count, sizeof(*list), compare_snapshot_filenames);
    *out_snapshots = list;
    *out_count = count;
    return NHP_OK;
}

nhp_err inst_snapshot_delete(const char *name, const char *filename)
{
    if (!path_is_safe(filename)) {
        err_set("'%s' is not a valid snapshot filename", filename);
        return NHP_EINVAL;
    }

    char *dir = inst_dir(name);
    if (dir == NULL) {
        return NHP_ENOMEM;
    }
    char *snapshots_dir = path_join(dir, "snapshots");
    free(dir);
    char *full = (snapshots_dir != NULL) ? path_join(snapshots_dir, filename) : NULL;
    free(snapshots_dir);
    if (full == NULL) {
        return NHP_ENOMEM;
    }

    if (!fs_exists(full)) {
        err_set("no such snapshot: %s", filename);
        free(full);
        return NHP_ENOENT;
    }

    nhp_err err = NHP_OK;
    if (unlink(full) != 0) {
        err_set("could not delete %s: %s", full, strerror(errno));
        err = NHP_EIO;
    }
    free(full);
    return err;
}

nhp_err inst_snapshot_restore(const char *name, const char *filename)
{
    if (!path_is_safe(filename)) {
        err_set("'%s' is not a valid snapshot filename", filename);
        return NHP_EINVAL;
    }

    char *dir = inst_dir(name);
    if (dir == NULL) {
        return NHP_ENOMEM;
    }
    char *snapshots_dir = path_join(dir, "snapshots");
    char *snapshot_path = (snapshots_dir != NULL) ? path_join(snapshots_dir, filename) : NULL;
    free(snapshots_dir);
    if (snapshot_path == NULL) {
        free(dir);
        return NHP_ENOMEM;
    }
    if (!fs_exists(snapshot_path)) {
        err_set("no such snapshot: %s", filename);
        free(snapshot_path);
        free(dir);
        return NHP_ENOENT;
    }

    char *data = NULL;
    size_t len = 0;
    nhp_err err = fs_read_file(snapshot_path, &data, &len);
    free(snapshot_path);
    if (err != NHP_OK) {
        free(dir);
        return err;
    }

    inst_instance parsed;
    err = parse_instance_conf(data, &parsed);
    if (err != NHP_OK) {
        free(data);
        free(dir);
        return err;
    }
    inst_free(&parsed); /* just validating that the snapshot still parses */

    /* take a fresh snapshot first, so restoring can itself be undone */
    char *backup_filename = NULL;
    err = inst_snapshot_create(name, "before-restore", &backup_filename);
    free(backup_filename);
    if (err != NHP_OK) {
        free(data);
        free(dir);
        return err;
    }

    char *conf_path = path_join(dir, "instance.conf");
    free(dir);
    err = (conf_path != NULL) ? fs_write_atomic(conf_path, data, len) : NHP_ENOMEM;
    free(conf_path);
    free(data);
    return err;
}

/* ---- export / import ---- */

nhp_err inst_export(const char *name, const char *out_path)
{
    char *dir = inst_dir(name);
    if (dir == NULL) {
        return NHP_ENOMEM;
    }
    if (!fs_exists(dir)) {
        err_set("instance %s does not exist", name);
        free(dir);
        return NHP_ENOENT;
    }

    char *conf_path = path_join(dir, "instance.conf");
    char *patches_src = path_join(dir, "src/patches");
    free(dir);
    if (conf_path == NULL || patches_src == NULL) {
        free(conf_path);
        free(patches_src);
        return NHP_ENOMEM;
    }

    char *work_dir = fs_make_temp_dir("nhpm-export");
    if (work_dir == NULL) {
        free(conf_path);
        free(patches_src);
        return NHP_ENOMEM;
    }

    char *conf_data = NULL;
    size_t conf_len = 0;
    nhp_err err = fs_read_file(conf_path, &conf_data, &conf_len);
    free(conf_path);

    if (err == NHP_OK) {
        char *work_conf = path_join(work_dir, "instance.conf");
        err = (work_conf != NULL) ? fs_write_atomic(work_conf, conf_data, conf_len) : NHP_ENOMEM;
        free(work_conf);
    }
    free(conf_data);

    if (err == NHP_OK && fs_exists(patches_src)) {
        char *work_patches = path_join(work_dir, "patches");
        err = (work_patches != NULL) ? fs_copy_tree(patches_src, work_patches) : NHP_ENOMEM;
        free(work_patches);
    }
    free(patches_src);

    if (err == NHP_OK) {
        const char *argv[] = {"tar", "czf", out_path, "-C", work_dir, ".", NULL};
        proc_result result = {0};
        err = proc_run(argv, NULL, NULL, &result);
        if (err == NHP_OK && (result.signaled || result.exit_code != 0)) {
            err_set("could not create %s", out_path);
            err = NHP_EIO;
        }
        proc_result_free(&result);
    }

    (void)fs_remove_tree(work_dir);
    free(work_dir);
    return err;
}

/* Extracts `nhinst_path` into a fresh temp dir and parses the
 * instance.conf inside it. *out_work_dir is always set when a temp dir was
 * created (even on failure), so the caller can clean it up either way. */
static nhp_err extract_and_parse_nhinst(const char *nhinst_path, inst_instance *out_parsed,
                                        char **out_work_dir)
{
    *out_work_dir = NULL;

    nhp_err err = check_tar_entries_safe(nhinst_path);
    if (err != NHP_OK) {
        return err;
    }

    char *work_dir = fs_make_temp_dir("nhpm-import");
    if (work_dir == NULL) {
        return NHP_ENOMEM;
    }
    *out_work_dir = work_dir;

    const char *argv[] = {"tar", "xf", nhinst_path, "-C", work_dir, NULL};
    proc_result result = {0};
    err = proc_run(argv, NULL, NULL, &result);
    if (err == NHP_OK && (result.signaled || result.exit_code != 0)) {
        err_set("could not extract %s", nhinst_path);
        err = NHP_EIO;
    }
    proc_result_free(&result);
    if (err != NHP_OK) {
        return err;
    }

    char *work_conf = path_join(work_dir, "instance.conf");
    char *conf_data = NULL;
    size_t conf_len = 0;
    err = (work_conf != NULL) ? fs_read_file(work_conf, &conf_data, &conf_len) : NHP_ENOMEM;
    free(work_conf);
    if (err == NHP_OK) {
        err = parse_instance_conf(conf_data, out_parsed);
    }
    free(conf_data);
    return err;
}

/* Copies the patches/ directory from an extracted .nhinst (if there is
 * one) into the new instance's src/patches. */
static nhp_err copy_imported_patches(const char *work_dir, const char *new_name)
{
    char *work_patches = path_join(work_dir, "patches");
    if (work_patches == NULL) {
        return NHP_ENOMEM;
    }

    nhp_err err = NHP_OK;
    if (fs_exists(work_patches)) {
        char *dest_dir = inst_dir(new_name);
        char *dest_patches = (dest_dir != NULL) ? path_join(dest_dir, "src/patches") : NULL;
        free(dest_dir);
        err = (dest_patches != NULL) ? fs_copy_tree(work_patches, dest_patches) : NHP_ENOMEM;
        free(dest_patches);
    }
    free(work_patches);
    return err;
}

nhp_err inst_import(const char *nhinst_path, const char *new_name, const char *source_version,
                    inst_instance *out)
{
    memset(out, 0, sizeof(*out));

    inst_instance parsed;
    char *work_dir = NULL;
    nhp_err err = extract_and_parse_nhinst(nhinst_path, &parsed, &work_dir);
    if (err != NHP_OK) {
        (void)fs_remove_tree(work_dir);
        free(work_dir);
        return err;
    }

    const char *actual_source =
        (source_version != NULL && source_version[0] != '\0') ? source_version : parsed.source;
    err = inst_new(new_name, actual_source, parsed.recipe, out);
    if (err == NHP_OK) {
        free(out->make_vars);
        out->make_vars = str_dup(parsed.make_vars);
        free(out->install_dir);
        out->install_dir = str_dup(parsed.install_dir);
        free(out->game_name);
        out->game_name = str_dup(parsed.game_name);
        free(out->notes);
        out->notes = str_dup(parsed.notes);

        /* docs/features.md 10.5's library-copy-preferred import needs
         * stack.c (M4) to push patches; for now just carry the data across. */
        out->patches = parsed.patches;
        out->patch_count = parsed.patch_count;
        parsed.patches = NULL;
        parsed.patch_count = 0;

        if (out->make_vars == NULL || out->install_dir == NULL || out->game_name == NULL ||
            out->notes == NULL) {
            err = NHP_ENOMEM;
        }
    }
    inst_free(&parsed);

    if (err == NHP_OK) {
        err = copy_imported_patches(work_dir, new_name);
    }

    (void)fs_remove_tree(work_dir);
    free(work_dir);

    if (err == NHP_OK) {
        err = inst_save(out);
    }
    if (err != NHP_OK) {
        inst_free(out);
    }
    return err;
}

/* ---- source path resolution ---- */

char *inst_source_full_path(const inst_instance *inst)
{
    if (inst->kind == INST_LINKED) {
        return str_dup(inst->source_path);
    }
    char *dir = inst_dir(inst->name);
    char *full = (dir != NULL) ? path_join(dir, inst->source_path) : NULL;
    free(dir);
    return full;
}

/* ---- load-time check ---- */

nhp_err inst_check(const inst_instance *inst, inst_check_result *out)
{
    memset(out, 0, sizeof(*out));

    char *source_full_path = inst_source_full_path(inst);
    if (source_full_path == NULL) {
        return NHP_ENOMEM;
    }

    out->source_path_exists = fs_exists(source_full_path);
    free(source_full_path);
    out->ok = out->source_path_exists;
    return NHP_OK;
}

nhp_err inst_find_patch_index(const inst_instance *inst, const char *id, size_t *out_index)
{
    for (size_t i = 0; i < inst->patch_count; i++) {
        if (str_eq(inst->patches[i].id, id)) {
            *out_index = i;
            return NHP_OK;
        }
    }
    err_set("no patch '%s' in instance '%s'", id, inst->name);
    return NHP_ENOENT;
}

/* ---- locking ---- */

nhp_err inst_lock_acquire(const char *name, inst_lock *out, pid_t *out_other_pid)
{
    *out_other_pid = 0;
    out->fd = -1;

    char *dir = inst_dir(name);
    if (dir == NULL) {
        return NHP_ENOMEM;
    }
    char *lock_path = path_join(dir, "lock");
    free(dir);
    if (lock_path == NULL) {
        return NHP_ENOMEM;
    }

    int fd = open(lock_path, O_RDWR | O_CREAT, 0600);
    if (fd < 0) {
        err_set("could not open %s: %s", lock_path, strerror(errno));
        free(lock_path);
        return NHP_EIO;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno == EWOULDBLOCK) {
            char pid_buf[32] = {0};
            ssize_t n = read(fd, pid_buf, sizeof(pid_buf) - 1);
            if (n > 0) {
                *out_other_pid = (pid_t)strtol(pid_buf, NULL, 10);
            }
            (void)close(fd);
            free(lock_path);
            err_set("instance %s is locked by another nhpm", name);
            return NHP_ELOCKED;
        }
        err_set("could not lock %s: %s", lock_path, strerror(errno));
        (void)close(fd);
        free(lock_path);
        return NHP_EIO;
    }

    char pid_str[32];
    int pid_len = snprintf(pid_str, sizeof(pid_str), "%ld", (long)getpid());
    if (ftruncate(fd, 0) != 0 || lseek(fd, 0, SEEK_SET) < 0 ||
        write(fd, pid_str, (size_t)pid_len) != pid_len) {
        /* best-effort: failing to record our PID doesn't invalidate the
         * lock itself, it just means a contender won't learn who holds
         * it. */
    }

    free(lock_path);
    out->fd = fd;
    return NHP_OK;
}

void inst_lock_release(inst_lock *lock)
{
    if (lock->fd >= 0) {
        (void)flock(lock->fd, LOCK_UN);
        (void)close(lock->fd);
        lock->fd = -1;
    }
}
