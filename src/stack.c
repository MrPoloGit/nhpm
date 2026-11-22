/* stack.c - enable/disable/reorder with rollback, sync with instance.conf.
 * SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "stack.h"

#include "quilt.h"

#include <ctype.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* A patch plus its filename under <src>/patches/ -- instance.conf has no
 * filename field (docs/features.md 10.2), so each operation resolves it
 * once via find_patch_filename() and carries it alongside the patch. */
typedef struct patch_slot {
    inst_patch patch; /* owned */
    char *filename;   /* owned; bare name, e.g. "498-color-alchemy.diff" */
} patch_slot;

static void free_slots(patch_slot *slots, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        inst_patch_free_contents(&slots[i].patch);
        free(slots[i].filename);
    }
    free(slots);
}

static char *patches_file_path(const char *src_dir, const char *filename)
{
    char *patches_dir = path_join(src_dir, "patches");
    char *full = (patches_dir != NULL) ? path_join(patches_dir, filename) : NULL;
    free(patches_dir);
    return full;
}

/* Scans <src_dir>/patches/ for a file named exactly `id` or starting with
 * "<id>-" (the slug added by stack_add()). NULL if there isn't one --
 * instance.conf and the source folder have gone out of sync. */
static char *find_patch_filename(const char *src_dir, const char *id)
{
    char *patches_dir = path_join(src_dir, "patches");
    if (patches_dir == NULL) {
        return NULL;
    }
    DIR *dp = opendir(patches_dir);
    free(patches_dir);
    if (dp == NULL) {
        return NULL;
    }

    size_t id_len = strlen(id);
    char *found = NULL;
    const struct dirent *entry;
    while (found == NULL && (entry = readdir(dp)) != NULL) {
        size_t name_len = strlen(entry->d_name);
        /* "quilt refresh --backup" (the default in some quiltrc's, e.g.
         * Homebrew's) leaves a "<name>~" backup of the patch file right
         * next to it; skip it so it's never mistaken for the real one. */
        if (name_len > 0 && entry->d_name[name_len - 1] == '~') {
            continue;
        }
        bool is_exact = str_eq(entry->d_name, id);
        bool is_prefixed = strncmp(entry->d_name, id, id_len) == 0 && entry->d_name[id_len] == '-';
        if (is_exact || is_prefixed) {
            found = str_dup(entry->d_name);
        }
    }
    (void)closedir(dp);
    return found;
}

static nhp_err build_slots(const char *src_dir, const inst_patch *patches, size_t count,
                           patch_slot **out_slots)
{
    *out_slots = NULL;
    if (count == 0) {
        return NHP_OK;
    }
    patch_slot *slots = calloc(count, sizeof(*slots));
    if (slots == NULL) {
        return NHP_ENOMEM;
    }
    for (size_t i = 0; i < count; i++) {
        nhp_err err = inst_patch_copy(&patches[i], &slots[i].patch);
        if (err != NHP_OK) {
            free_slots(slots, i);
            return err;
        }
        slots[i].filename = find_patch_filename(src_dir, patches[i].id);
        if (slots[i].filename == NULL) {
            inst_patch_free_contents(&slots[i].patch);
            free_slots(slots, i);
            err_set("patch file for '%s' not found under %s/patches -- the instance's patch "
                    "list and its source folder have gone out of sync",
                    patches[i].id, src_dir);
            return NHP_ENOENT;
        }
    }
    *out_slots = slots;
    return NHP_OK;
}

/* "<filename> -p<strip>", or commented out (docs/features.md 11.3) if
 * disabled. strip < 0 (not yet determined) writes a placeholder "-p1" --
 * harmless, since a disabled line is never acted on until enabled. */
static char *series_line_for(const patch_slot *slot)
{
    int strip = (slot->patch.strip >= 0) ? slot->patch.strip : 1;
    buf b;
    buf_init(&b);
    if (!slot->patch.enabled) {
        buf_append_str(&b, "# nhpm-disabled: ");
    }
    buf_append_fmt(&b, "%s -p%d", slot->filename, strip);
    return buf_take(&b);
}

static nhp_err write_series_file(const char *src_dir, const patch_slot *slots, size_t count)
{
    buf text;
    buf_init(&text);
    for (size_t i = 0; i < count; i++) {
        char *line = series_line_for(&slots[i]);
        if (line == NULL) {
            buf_free(&text);
            return NHP_ENOMEM;
        }
        buf_append_str(&text, line);
        buf_append_str(&text, "\n");
        free(line);
    }
    char *data = buf_take(&text);
    char *series_path = path_join(src_dir, "series");
    if (series_path == NULL) {
        free(data);
        return NHP_ENOMEM;
    }
    nhp_err err = fs_write_atomic(series_path, data, strlen(data));
    free(series_path);
    free(data);
    return err;
}

static nhp_err read_series_file(const char *src_dir, char **out_text)
{
    char *series_path = path_join(src_dir, "series");
    if (series_path == NULL) {
        return NHP_ENOMEM;
    }
    size_t len = 0;
    nhp_err err = fs_read_file(series_path, out_text, &len);
    free(series_path);
    if (err == NHP_ENOENT) {
        *out_text = str_dup("");
        err = (*out_text != NULL) ? NHP_OK : NHP_ENOMEM;
    }
    return err;
}

/* Pushes every enabled patch among slots[0..count) in order, stopping at
 * the first failure. *out_fail_index is SIZE_MAX if they all succeeded;
 * otherwise *out_fail_result is the failing push's result (caller frees it). */
static nhp_err push_in_order(const char *src_dir, const patch_slot *slots, size_t count,
                             size_t *out_fail_index, proc_result *out_fail_result)
{
    *out_fail_index = SIZE_MAX;
    for (size_t i = 0; i < count; i++) {
        if (!slots[i].patch.enabled) {
            continue;
        }
        proc_result result = {0};
        nhp_err err = quilt_push(src_dir, slots[i].filename, false, &result);
        if (err != NHP_OK) {
            proc_result_free(&result);
            return err;
        }
        if (result.signaled || result.exit_code != 0) {
            *out_fail_index = i;
            *out_fail_result = result;
            return NHP_OK;
        }
        proc_result_free(&result);
    }
    return NHP_OK;
}

/* Restores `original_series_text`, pops everything, and pushes it back --
 * the "undo" half of every all-or-nothing operation here (docs/features.md
 * 11.1). Returns NHP_EIO if the restorative push itself fails. */
static nhp_err rollback(const char *src_dir, const char *original_series_text)
{
    char *series_path = path_join(src_dir, "series");
    if (series_path == NULL) {
        return NHP_ENOMEM;
    }
    nhp_err err = fs_write_atomic(series_path, original_series_text, strlen(original_series_text));
    free(series_path);
    if (err != NHP_OK) {
        return err;
    }

    proc_result pop_result = {0};
    (void)quilt_pop_all(src_dir, &pop_result);
    proc_result_free(&pop_result);

    proc_result push_result = {0};
    err = quilt_push_all(src_dir, &push_result);
    /* "No patches in series" (e.g. restoring to a brand-new instance's
     * first, still-all-disabled series) is a trivially successful
     * no-op, not a conflict. */
    bool nothing_to_push = !push_result.signaled && push_result.err_data != NULL &&
                           strstr(push_result.err_data, "No patches in series") != NULL;
    bool ok =
        err == NHP_OK && !push_result.signaled && (push_result.exit_code == 0 || nothing_to_push);
    if (!ok) {
        err_set("rollback after a conflict failed -- the source folder may be in a partially "
                "changed state; quilt said: %s%s",
                (push_result.out_data != NULL) ? push_result.out_data : "",
                (push_result.err_data != NULL) ? push_result.err_data : "");
        proc_result_free(&push_result);
        return NHP_EIO;
    }
    proc_result_free(&push_result);
    return NHP_OK;
}

static int determine_strip_level(const char *src_dir, const char *abs_patch_path)
{
    static const int candidates[] = {1, 0, 2};
    int best_strip = 1;
    int best_failed = -1;
    for (size_t i = 0; i < 3; i++) {
        quilt_dry_run d;
        nhp_err err = quilt_dry_run_patch(src_dir, abs_patch_path, candidates[i], &d);
        if (err == NHP_OK && !d.no_file_found &&
            (best_failed == -1 || d.failed_hunks < best_failed)) {
            best_failed = d.failed_hunks;
            best_strip = candidates[i];
        }
        quilt_dry_run_free(&d);
    }
    return best_strip;
}

void stack_conflict_free(stack_conflict *c)
{
    free(c->patch_id);
    free(c->patch_version);
    /* file_count > 0 is only ever reached after fill_conflict() has already
     * populated files[]; its calloc-failure path zeroes file_count first,
     * so files is never NULL when this loop actually runs. */
    for (size_t i = 0; i < c->file_count; i++) {
        free(c->files[i].path); // NOLINT(clang-analyzer-core.NullDereference)
    }
    free(c->files);
    free(c->quilt_output);
    memset(c, 0, sizeof(*c));
}

static nhp_err fill_conflict(stack_conflict *out, const char *id, const char *version,
                             bool is_requested, const quilt_dry_run *dry,
                             const proc_result *push_result)
{
    memset(out, 0, sizeof(*out));
    out->patch_id = str_dup(id);
    out->patch_version = str_dup(version);
    out->is_requested_patch = is_requested;

    buf b;
    buf_init(&b);
    if (push_result->out_data != NULL) {
        buf_append_str(&b, push_result->out_data);
    }
    if (push_result->err_data != NULL) {
        buf_append_str(&b, push_result->err_data);
    }
    out->quilt_output = buf_take(&b);

    if (dry->file_count > 0) {
        out->files = calloc(dry->file_count, sizeof(*out->files));
        if (out->files == NULL) {
            stack_conflict_free(out);
            return NHP_ENOMEM;
        }
        for (size_t i = 0; i < dry->file_count; i++) {
            out->files[i].path = str_dup(dry->files[i].path);
            out->files[i].hunks_total = dry->files[i].hunks_total;
            out->files[i].hunks_failed = dry->files[i].hunks_failed;
        }
        out->file_count = dry->file_count;
    }

    if (out->patch_id == NULL || out->patch_version == NULL || out->quilt_output == NULL) {
        stack_conflict_free(out);
        return NHP_ENOMEM;
    }
    return NHP_OK;
}

/* Builds a conflict report for `failed_slot` by dry-running it again at its
 * recorded strip level. Must be called before any rollback, while the
 * source folder is still in the state the failing push left it in. */
static nhp_err build_conflict(stack_conflict *out, const patch_slot *failed_slot, bool is_requested,
                              const char *src_dir, const proc_result *push_result)
{
    char *abs_path = patches_file_path(src_dir, failed_slot->filename);
    if (abs_path == NULL) {
        return NHP_ENOMEM;
    }
    int strip = (failed_slot->patch.strip >= 0) ? failed_slot->patch.strip : 1;
    quilt_dry_run dry;
    nhp_err err = quilt_dry_run_patch(src_dir, abs_path, strip, &dry);
    free(abs_path);
    if (err != NHP_OK) {
        return err;
    }
    err = fill_conflict(out, failed_slot->patch.id, failed_slot->patch.version, is_requested, &dry,
                        push_result);
    quilt_dry_run_free(&dry);
    return err;
}

/* Pops everything, writes slots' series, and pushes it all back in order.
 * On success the folder matches `slots`. On conflict, rolls back to
 * `original_series` and returns NHP_ECONFLICT with *out_conflict filled in. */
static nhp_err apply_slots(const char *src_dir, const patch_slot *slots, size_t count,
                           size_t requested_index, const char *original_series,
                           stack_conflict *out_conflict)
{
    proc_result pr = {0};
    nhp_err err = quilt_pop_all(src_dir, &pr);
    proc_result_free(&pr);
    if (err != NHP_OK) {
        return err;
    }

    err = write_series_file(src_dir, slots, count);
    if (err != NHP_OK) {
        return err;
    }

    size_t fail_index = SIZE_MAX;
    proc_result fail_result = {0};
    err = push_in_order(src_dir, slots, count, &fail_index, &fail_result);
    if (err != NHP_OK) {
        return err;
    }
    if (fail_index == SIZE_MAX) {
        return NHP_OK;
    }

    nhp_err build_err = build_conflict(out_conflict, &slots[fail_index],
                                       fail_index == requested_index, src_dir, &fail_result);
    proc_result_free(&fail_result);

    nhp_err rollback_err = rollback(src_dir, original_series);
    if (rollback_err != NHP_OK) {
        if (build_err == NHP_OK) {
            stack_conflict_free(out_conflict);
        }
        return rollback_err;
    }
    return (build_err == NHP_OK) ? NHP_ECONFLICT : build_err;
}

/* Phase A of enabling a patch (docs/features.md 11.4): pushes the baseline
 * prefix, dry-runs slots[index] to pick a strip level, and marks it enabled
 * in memory. apply_slots() (phase B) redoes the real push afterwards. */
static nhp_err determine_and_set_strip(const char *src_dir, patch_slot *slots, size_t count,
                                       size_t index, const char *original_series,
                                       stack_conflict *out_conflict)
{
    proc_result pr = {0};
    nhp_err err = quilt_pop_all(src_dir, &pr);
    proc_result_free(&pr);
    if (err != NHP_OK) {
        return err;
    }

    err = write_series_file(src_dir, slots, count);
    if (err != NHP_OK) {
        return err;
    }

    size_t fail_index = SIZE_MAX;
    proc_result fail_result = {0};
    err = push_in_order(src_dir, slots, index, &fail_index, &fail_result);
    if (err != NHP_OK) {
        return err;
    }

    if (fail_index != SIZE_MAX) {
        nhp_err build_err =
            build_conflict(out_conflict, &slots[fail_index], false, src_dir, &fail_result);
        proc_result_free(&fail_result);
        nhp_err rollback_err = rollback(src_dir, original_series);
        if (rollback_err != NHP_OK) {
            if (build_err == NHP_OK) {
                stack_conflict_free(out_conflict);
            }
            return rollback_err;
        }
        return (build_err == NHP_OK) ? NHP_ECONFLICT : build_err;
    }

    char *abs_patch_path = patches_file_path(src_dir, slots[index].filename);
    if (abs_patch_path == NULL) {
        return NHP_ENOMEM;
    }
    slots[index].patch.strip = determine_strip_level(src_dir, abs_patch_path);
    free(abs_patch_path);
    slots[index].patch.enabled = true;
    return NHP_OK;
}

/* On NHP_OK, moves `slots`' patch contents into inst->patches and saves
 * instance.conf. On any other err, frees `slots` and leaves *inst
 * untouched. Either way, `slots` is consumed. */
static nhp_err finish_slots(inst_instance *inst, patch_slot *slots, size_t count, nhp_err err)
{
    if (err != NHP_OK) {
        free_slots(slots, count);
        return err;
    }

    inst_patch *new_patches = NULL;
    if (count > 0) {
        new_patches = calloc(count, sizeof(*new_patches));
        if (new_patches == NULL) {
            free_slots(slots, count);
            return NHP_ENOMEM;
        }
    }
    for (size_t i = 0; i < count; i++) {
        new_patches[i] = slots[i].patch; /* ownership moves */
        free(slots[i].filename);
    }
    free(slots);

    for (size_t i = 0; i < inst->patch_count; i++) {
        inst_patch_free_contents(&inst->patches[i]);
    }
    free(inst->patches);
    inst->patches = new_patches;
    inst->patch_count = count;

    char *stamp = time_now_iso8601();
    if (stamp == NULL) {
        return NHP_ENOMEM;
    }
    free(inst->stack_modified);
    inst->stack_modified = stamp;

    return inst_save(inst);
}

nhp_err stack_enable(inst_instance *inst, size_t index, stack_conflict *out_conflict)
{
    if (index >= inst->patch_count) {
        return NHP_EINVAL;
    }
    if (inst->patches[index].enabled) {
        return NHP_OK;
    }

    char *src_dir = inst_source_full_path(inst);
    if (src_dir == NULL) {
        return NHP_ENOMEM;
    }

    patch_slot *slots = NULL;
    nhp_err err = build_slots(src_dir, inst->patches, inst->patch_count, &slots);
    if (err != NHP_OK) {
        free(src_dir);
        return err;
    }

    char *original_series = NULL;
    err = read_series_file(src_dir, &original_series);
    if (err != NHP_OK) {
        free_slots(slots, inst->patch_count);
        free(src_dir);
        return err;
    }

    err = determine_and_set_strip(src_dir, slots, inst->patch_count, index, original_series,
                                  out_conflict);
    if (err == NHP_OK) {
        err = apply_slots(src_dir, slots, inst->patch_count, index, original_series, out_conflict);
    }

    err = finish_slots(inst, slots, inst->patch_count, err);
    free(original_series);
    free(src_dir);
    return err;
}

nhp_err stack_disable(inst_instance *inst, size_t index, stack_conflict *out_conflict)
{
    if (index >= inst->patch_count) {
        return NHP_EINVAL;
    }
    if (!inst->patches[index].enabled) {
        return NHP_OK;
    }

    char *src_dir = inst_source_full_path(inst);
    if (src_dir == NULL) {
        return NHP_ENOMEM;
    }

    patch_slot *slots = NULL;
    nhp_err err = build_slots(src_dir, inst->patches, inst->patch_count, &slots);
    if (err != NHP_OK) {
        free(src_dir);
        return err;
    }

    char *original_series = NULL;
    err = read_series_file(src_dir, &original_series);
    if (err != NHP_OK) {
        free_slots(slots, inst->patch_count);
        free(src_dir);
        return err;
    }

    slots[index].patch.enabled = false;

    err = apply_slots(src_dir, slots, inst->patch_count, index, original_series, out_conflict);
    err = finish_slots(inst, slots, inst->patch_count, err);
    free(original_series);
    free(src_dir);
    return err;
}

nhp_err stack_move(inst_instance *inst, size_t from, size_t to, stack_conflict *out_conflict)
{
    if (from >= inst->patch_count || to >= inst->patch_count) {
        return NHP_EINVAL;
    }
    if (from == to) {
        return NHP_OK;
    }

    char *src_dir = inst_source_full_path(inst);
    if (src_dir == NULL) {
        return NHP_ENOMEM;
    }

    patch_slot *slots = NULL;
    nhp_err err = build_slots(src_dir, inst->patches, inst->patch_count, &slots);
    if (err != NHP_OK) {
        free(src_dir);
        return err;
    }

    char *original_series = NULL;
    err = read_series_file(src_dir, &original_series);
    if (err != NHP_OK) {
        free_slots(slots, inst->patch_count);
        free(src_dir);
        return err;
    }

    patch_slot moved = slots[from];
    if (from < to) {
        for (size_t i = from; i < to; i++) {
            slots[i] = slots[i + 1];
        }
    } else {
        for (size_t i = from; i > to; i--) {
            slots[i] = slots[i - 1];
        }
    }
    slots[to] = moved;

    err = apply_slots(src_dir, slots, inst->patch_count, to, original_series, out_conflict);
    err = finish_slots(inst, slots, inst->patch_count, err);
    free(original_series);
    free(src_dir);
    return err;
}

static char *slugify(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return str_dup("patch");
    }
    const size_t max_len = 40;
    char *slug = malloc(max_len + 1);
    if (slug == NULL) {
        return NULL;
    }
    size_t out_i = 0;
    bool last_was_dash = false;
    for (size_t i = 0; name[i] != '\0' && out_i < max_len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (isalnum(c)) {
            slug[out_i++] = (char)tolower(c);
            last_was_dash = false;
        } else if (!last_was_dash && out_i > 0) {
            slug[out_i++] = '-';
            last_was_dash = true;
        }
    }
    while (out_i > 0 && slug[out_i - 1] == '-') {
        out_i--;
    }
    slug[out_i] = '\0';
    if (out_i == 0) {
        free(slug);
        return str_dup("patch");
    }
    return slug;
}

/* Copies `library_patch_path` into <src_dir>/patches/<id>-<slug>.diff;
 * `name` is only used to build the slug. */
static nhp_err copy_patch_file_into_instance(const char *src_dir, const char *id, const char *name,
                                             const char *library_patch_path)
{
    char *slug = slugify(name);
    if (slug == NULL) {
        return NHP_ENOMEM;
    }
    buf fname_buf;
    buf_init(&fname_buf);
    buf_append_fmt(&fname_buf, "%s-%s.diff", id, slug);
    free(slug);
    char *filename = buf_take(&fname_buf);

    char *patches_dir = path_join(src_dir, "patches");
    nhp_err err = (patches_dir != NULL) ? fs_mkdir_p(patches_dir, 0755) : NHP_ENOMEM;
    char *dest_path = (err == NHP_OK) ? path_join(patches_dir, filename) : NULL;
    free(patches_dir);
    if (err == NHP_OK && dest_path == NULL) {
        err = NHP_ENOMEM;
    }
    if (err == NHP_OK) {
        char *data = NULL;
        size_t len = 0;
        err = fs_read_file(library_patch_path, &data, &len);
        if (err == NHP_OK) {
            err = fs_write_atomic(dest_path, data, len);
            free(data);
        }
    }
    free(dest_path);
    free(filename);
    return err;
}

/* Appends a new, disabled inst_patch entry to inst->patches and returns
 * its index. */
static nhp_err append_patch_entry(inst_instance *inst, const char *id, const char *version,
                                  const char *sha256, size_t *out_index)
{
    inst_patch *grown = realloc(inst->patches, (inst->patch_count + 1) * sizeof(*grown));
    if (grown == NULL) {
        return NHP_ENOMEM;
    }
    inst->patches = grown;
    inst_patch *new_entry = &inst->patches[inst->patch_count];
    memset(new_entry, 0, sizeof(*new_entry));
    new_entry->id = str_dup(id);
    new_entry->version = str_dup(version);
    new_entry->enabled = false;
    new_entry->strip = -1;
    new_entry->sha256 = str_dup(sha256);
    new_entry->modified = false;
    if (new_entry->id == NULL || new_entry->version == NULL || new_entry->sha256 == NULL) {
        inst_patch_free_contents(new_entry);
        return NHP_ENOMEM;
    }
    inst->patch_count++;
    *out_index = inst->patch_count - 1;
    return NHP_OK;
}

nhp_err stack_add(inst_instance *inst, const char *id, const char *version, const char *name,
                  const char *library_patch_path, const char *sha256, bool enable,
                  size_t *out_new_index, stack_conflict *out_conflict)
{
    char *src_dir = inst_source_full_path(inst);
    if (src_dir == NULL) {
        return NHP_ENOMEM;
    }

    nhp_err err = copy_patch_file_into_instance(src_dir, id, name, library_patch_path);
    if (err != NHP_OK) {
        free(src_dir);
        return err;
    }

    size_t new_index = 0;
    err = append_patch_entry(inst, id, version, sha256, &new_index);
    if (err != NHP_OK) {
        free(src_dir);
        return err;
    }

    patch_slot *slots = NULL;
    err = build_slots(src_dir, inst->patches, inst->patch_count, &slots);
    if (err == NHP_OK) {
        err = write_series_file(src_dir, slots, inst->patch_count);
    }
    if (err == NHP_OK) {
        char *stamp = time_now_iso8601();
        err = (stamp != NULL) ? NHP_OK : NHP_ENOMEM;
        if (err == NHP_OK) {
            free(inst->stack_modified);
            inst->stack_modified = stamp;
        }
    }
    if (err == NHP_OK) {
        err = inst_save(inst);
    }
    if (err != NHP_OK) {
        free_slots(slots, inst->patch_count);
        free(src_dir);
        return err;
    }

    if (out_new_index != NULL) {
        *out_new_index = new_index;
    }

    if (!enable) {
        free_slots(slots, inst->patch_count);
        free(src_dir);
        return NHP_OK;
    }

    char *original_series = NULL;
    err = read_series_file(src_dir, &original_series);
    if (err != NHP_OK) {
        free_slots(slots, inst->patch_count);
        free(src_dir);
        return err;
    }

    err = determine_and_set_strip(src_dir, slots, inst->patch_count, new_index, original_series,
                                  out_conflict);
    if (err == NHP_OK) {
        err = apply_slots(src_dir, slots, inst->patch_count, new_index, original_series,
                          out_conflict);
    }
    err = finish_slots(inst, slots, inst->patch_count, err);
    free(original_series);
    free(src_dir);
    return err;
}

nhp_err stack_remove(inst_instance *inst, size_t index, stack_conflict *out_conflict)
{
    if (index >= inst->patch_count) {
        return NHP_EINVAL;
    }

    if (inst->patches[index].enabled) {
        nhp_err err = stack_disable(inst, index, out_conflict);
        if (err != NHP_OK) {
            return err;
        }
    }

    char *src_dir = inst_source_full_path(inst);
    if (src_dir == NULL) {
        return NHP_ENOMEM;
    }

    char *filename = find_patch_filename(src_dir, inst->patches[index].id);
    if (filename != NULL) {
        char *path = patches_file_path(src_dir, filename);
        if (path != NULL) {
            (void)unlink(path); /* best-effort; the array entry is removed regardless */
        }
        free(path);
        free(filename);
    }

    inst_patch_free_contents(&inst->patches[index]);
    for (size_t i = index; i + 1 < inst->patch_count; i++) {
        inst->patches[i] = inst->patches[i + 1];
    }
    inst->patch_count--;

    patch_slot *slots = NULL;
    nhp_err err = build_slots(src_dir, inst->patches, inst->patch_count, &slots);
    if (err == NHP_OK) {
        err = write_series_file(src_dir, slots, inst->patch_count);
        free_slots(slots, inst->patch_count);
    }
    if (err == NHP_OK) {
        char *stamp = time_now_iso8601();
        err = (stamp != NULL) ? NHP_OK : NHP_ENOMEM;
        if (err == NHP_OK) {
            free(inst->stack_modified);
            inst->stack_modified = stamp;
        }
    }
    if (err == NHP_OK) {
        err = inst_save(inst);
    }
    free(src_dir);
    return err;
}

/* Dry-runs slots[index] first for an accurate hunk breakdown -- the forced
 * push below leaves some hunks applied and others as .rej files, which
 * would throw off a dry run done afterwards -- then runs "quilt push -f". */
static nhp_err force_push_and_fill_conflict(const char *src_dir, const patch_slot *slots,
                                            size_t index, stack_conflict *out_conflict)
{
    char *abs_patch_path = patches_file_path(src_dir, slots[index].filename);
    if (abs_patch_path == NULL) {
        return NHP_ENOMEM;
    }
    int strip = (slots[index].patch.strip >= 0) ? slots[index].patch.strip : 1;
    quilt_dry_run dry;
    nhp_err err = quilt_dry_run_patch(src_dir, abs_patch_path, strip, &dry);
    free(abs_patch_path);
    if (err != NHP_OK) {
        return err;
    }

    proc_result force_result = {0};
    err = quilt_push(src_dir, slots[index].filename, true, &force_result);
    if (err != NHP_OK) {
        quilt_dry_run_free(&dry);
        proc_result_free(&force_result);
        return err;
    }

    /* "quilt push -f" reports a nonzero exit even on its own kind of
     * success (the patch landed on the stack with .rej files next to
     * what didn't apply) -- that's the expected outcome here. */
    err = fill_conflict(out_conflict, slots[index].patch.id, slots[index].patch.version, true, &dry,
                        &force_result);
    quilt_dry_run_free(&dry);
    proc_result_free(&force_result);
    return err;
}

nhp_err stack_force_begin(inst_instance *inst, size_t index, stack_conflict *out_conflict)
{
    if (index >= inst->patch_count) {
        return NHP_EINVAL;
    }

    char *src_dir = inst_source_full_path(inst);
    if (src_dir == NULL) {
        return NHP_ENOMEM;
    }

    char *snapshot_name = NULL;
    nhp_err err = inst_snapshot_create(inst->name, "before-force", &snapshot_name);
    free(snapshot_name);
    if (err != NHP_OK) {
        free(src_dir);
        return err;
    }

    patch_slot *slots = NULL;
    err = build_slots(src_dir, inst->patches, inst->patch_count, &slots);
    if (err != NHP_OK) {
        free(src_dir);
        return err;
    }

    char *original_series = NULL;
    err = read_series_file(src_dir, &original_series);
    if (err != NHP_OK) {
        free_slots(slots, inst->patch_count);
        free(src_dir);
        return err;
    }

    /* Reuses phase A of a normal enable to determine a strip level and mark
     * it enabled: a still-commented series line isn't something "quilt
     * push", forced or not, can find, so Force must uncomment it first. */
    err = determine_and_set_strip(src_dir, slots, inst->patch_count, index, original_series,
                                  out_conflict);
    if (err == NHP_OK) {
        err = write_series_file(src_dir, slots, inst->patch_count);
    }
    if (err == NHP_OK) {
        err = force_push_and_fill_conflict(src_dir, slots, index, out_conflict);
    }

    if (err == NHP_OK) {
        /* Reflect the now-enabled, now-known-strip state in *inst too
         * (not saved yet -- stack_force_finish() does that, once the
         * caller is done editing). */
        inst->patches[index].enabled = true;
        inst->patches[index].strip = slots[index].patch.strip;
    }

    free_slots(slots, inst->patch_count);
    free(original_series);
    free(src_dir);
    return err;
}

nhp_err stack_force_finish(inst_instance *inst, size_t index)
{
    if (index >= inst->patch_count) {
        return NHP_EINVAL;
    }

    char *src_dir = inst_source_full_path(inst);
    if (src_dir == NULL) {
        return NHP_ENOMEM;
    }

    char *filename = find_patch_filename(src_dir, inst->patches[index].id);
    if (filename == NULL) {
        free(src_dir);
        return NHP_ENOENT;
    }

    proc_result result = {0};
    nhp_err err = quilt_refresh(src_dir, filename, &result);
    if (err == NHP_OK && (result.signaled || result.exit_code != 0)) {
        err_set("quilt refresh %s failed: %s", filename, result.err_data);
        err = NHP_EIO;
    }
    proc_result_free(&result);
    free(filename);
    free(src_dir);
    if (err != NHP_OK) {
        return err;
    }

    inst->patches[index].enabled = true;
    inst->patches[index].modified = true;

    char *stamp = time_now_iso8601();
    if (stamp == NULL) {
        return NHP_ENOMEM;
    }
    free(inst->stack_modified);
    inst->stack_modified = stamp;

    return inst_save(inst);
}

/* ---- 10.4: checking and resolving drift between instance.conf and the
 * folder (quilt run by hand, a patch edited directly, or a crash
 * mid-operation) ---- */

void stack_drift_free(stack_drift *d)
{
    free(d->detail);
    memset(d, 0, sizeof(*d));
}

nhp_err stack_check_consistency(const inst_instance *inst, stack_drift *out)
{
    memset(out, 0, sizeof(*out));
    char *src_dir = inst_source_full_path(inst);
    if (src_dir == NULL) {
        return NHP_ENOMEM;
    }

    patch_slot *slots = NULL;
    nhp_err err = build_slots(src_dir, inst->patches, inst->patch_count, &slots);
    if (err != NHP_OK) {
        /* A patch's file is simply gone -- itself the drift to report,
         * not a reason to fail this check outright. */
        out->detail = str_dup(err_get());
        free(src_dir);
        return (out->detail != NULL) ? NHP_OK : NHP_ENOMEM;
    }

    buf expected_buf;
    buf_init(&expected_buf);
    nhp_err build_err = NHP_OK;
    for (size_t i = 0; i < inst->patch_count && build_err == NHP_OK; i++) {
        char *line = series_line_for(&slots[i]);
        if (line == NULL) {
            build_err = NHP_ENOMEM;
            break;
        }
        (void)buf_append_str(&expected_buf, line);
        (void)buf_append_str(&expected_buf, "\n");
        free(line);
    }
    char *expected_series = buf_take(&expected_buf);
    if (build_err != NHP_OK || expected_series == NULL) {
        free(expected_series);
        free_slots(slots, inst->patch_count);
        free(src_dir);
        return NHP_ENOMEM;
    }

    char *actual_series = NULL;
    err = read_series_file(src_dir, &actual_series);
    if (err != NHP_OK) {
        free(expected_series);
        free_slots(slots, inst->patch_count);
        free(src_dir);
        return err;
    }
    out->series_matches = str_eq(expected_series, actual_series);
    free(expected_series);
    free(actual_series);

    char **applied = NULL;
    size_t applied_count = 0;
    err = quilt_applied(src_dir, &applied, &applied_count);
    if (err != NHP_OK) {
        free_slots(slots, inst->patch_count);
        free(src_dir);
        return err;
    }
    size_t expected_enabled_count = 0;
    for (size_t i = 0; i < inst->patch_count; i++) {
        if (slots[i].patch.enabled) {
            expected_enabled_count++;
        }
    }
    bool quilt_ok = (applied_count == expected_enabled_count);
    size_t j = 0;
    for (size_t i = 0; quilt_ok && i < inst->patch_count; i++) {
        if (!slots[i].patch.enabled) {
            continue;
        }
        if (!str_eq(applied[j], slots[i].filename)) {
            quilt_ok = false;
        }
        j++;
    }
    out->quilt_applied_matches = quilt_ok;
    quilt_name_list_free(applied, applied_count);

    buf detail;
    buf_init(&detail);
    if (!out->series_matches) {
        (void)buf_append_str(&detail, "the series file doesn't match instance.conf's patch list\n");
    }
    if (!out->quilt_applied_matches) {
        (void)buf_append_str(&detail,
                             "quilt's applied patches don't match the enabled patch list\n");
    }

    bool hashes_ok = true;
    for (size_t i = 0; i < inst->patch_count; i++) {
        if (slots[i].patch.sha256[0] == '\0') {
            continue; /* never recorded -- not something to flag as "changed" */
        }
        char *abs_path = patches_file_path(src_dir, slots[i].filename);
        char *data = NULL;
        size_t len = 0;
        if (abs_path != NULL && fs_read_file(abs_path, &data, &len) == NHP_OK) {
            char hex[65];
            sha256_hex(data, len, hex);
            if (!str_eq(hex, slots[i].patch.sha256)) {
                hashes_ok = false;
                (void)buf_append_fmt(&detail, "%s's patch file has changed since it was added\n",
                                     slots[i].patch.id);
            }
        }
        free(data);
        free(abs_path);
    }
    out->hashes_match = hashes_ok;
    out->ok = out->series_matches && out->quilt_applied_matches && out->hashes_match;
    out->detail = buf_take(&detail);

    free_slots(slots, inst->patch_count);
    free(src_dir);
    return (out->detail != NULL) ? NHP_OK : NHP_ENOMEM;
}

nhp_err stack_restore(const inst_instance *inst, stack_conflict *out_conflict)
{
    char *src_dir = inst_source_full_path(inst);
    if (src_dir == NULL) {
        return NHP_ENOMEM;
    }

    patch_slot *slots = NULL;
    nhp_err err = build_slots(src_dir, inst->patches, inst->patch_count, &slots);
    if (err != NHP_OK) {
        free(src_dir);
        return err;
    }

    char *original_series = NULL;
    err = read_series_file(src_dir, &original_series);
    if (err != NHP_OK) {
        free_slots(slots, inst->patch_count);
        free(src_dir);
        return err;
    }

    /* No single "requested" patch here (SIZE_MAX never matches any real
     * index), so a conflict is always reported as "a later patch
     * depends on it" -- the honest truth for a whole-stack restore. */
    err = apply_slots(src_dir, slots, inst->patch_count, SIZE_MAX, original_series, out_conflict);

    free_slots(slots, inst->patch_count);
    free(original_series);
    free(src_dir);
    return err;
}

nhp_err stack_adopt(inst_instance *inst)
{
    char *src_dir = inst_source_full_path(inst);
    if (src_dir == NULL) {
        return NHP_ENOMEM;
    }

    char **applied = NULL;
    size_t applied_count = 0;
    nhp_err err = quilt_applied(src_dir, &applied, &applied_count);
    if (err != NHP_OK) {
        free(src_dir);
        return err;
    }

    size_t kept = 0;
    for (size_t i = 0; i < inst->patch_count; i++) {
        inst_patch *p = &inst->patches[i];
        char *filename = find_patch_filename(src_dir, p->id);
        if (filename == NULL) {
            inst_patch_free_contents(p);
            continue;
        }

        bool is_applied = false;
        for (size_t j = 0; j < applied_count; j++) {
            if (str_eq(applied[j], filename)) {
                is_applied = true;
                break;
            }
        }
        p->enabled = is_applied;

        char *abs_path = patches_file_path(src_dir, filename);
        char *data = NULL;
        size_t len = 0;
        if (abs_path != NULL && fs_read_file(abs_path, &data, &len) == NHP_OK) {
            char hex[65];
            sha256_hex(data, len, hex);
            char *new_sha = str_dup(hex);
            if (new_sha != NULL) {
                free(p->sha256);
                p->sha256 = new_sha;
            }
        }
        free(data);
        free(abs_path);

        if (is_applied && p->strip < 0) {
            char *abs_patch_path = patches_file_path(src_dir, filename);
            if (abs_patch_path != NULL) {
                p->strip = determine_strip_level(src_dir, abs_patch_path);
            }
            free(abs_patch_path);
        }
        free(filename);

        inst->patches[kept++] = *p;
    }
    inst->patch_count = kept;
    quilt_name_list_free(applied, applied_count);

    /* Keep the series file's "# nhpm-disabled:" markers in sync with the
     * enabled flags just adopted, so instance.conf doesn't drift from the
     * series file nhpm itself writes on every other stack change. */
    patch_slot *slots = NULL;
    err = build_slots(src_dir, inst->patches, inst->patch_count, &slots);
    if (err == NHP_OK) {
        err = write_series_file(src_dir, slots, inst->patch_count);
        free_slots(slots, inst->patch_count);
    }
    free(src_dir);
    if (err != NHP_OK) {
        return err;
    }

    char *stamp = time_now_iso8601();
    if (stamp == NULL) {
        return NHP_ENOMEM;
    }
    free(inst->stack_modified);
    inst->stack_modified = stamp;

    return inst_save(inst);
}

/* ---- 12.4: editing a patch already on the stack ---- */

nhp_err stack_edit_begin(inst_instance *inst, size_t index, stack_conflict *out_conflict)
{
    if (index >= inst->patch_count) {
        return NHP_EINVAL;
    }
    if (!inst->patches[index].enabled) {
        err_set("%s is disabled -- enable it first", inst->patches[index].id);
        return NHP_EINVAL;
    }

    char *src_dir = inst_source_full_path(inst);
    if (src_dir == NULL) {
        return NHP_ENOMEM;
    }

    char *snapshot_name = NULL;
    nhp_err err = inst_snapshot_create(inst->name, "before edit", &snapshot_name);
    free(snapshot_name);
    if (err != NHP_OK) {
        free(src_dir);
        return err;
    }

    char *filename = find_patch_filename(src_dir, inst->patches[index].id);
    if (filename == NULL) {
        err_set("could not find %s's patch file under %s/patches", inst->patches[index].id,
                src_dir);
        free(src_dir);
        return NHP_ENOENT;
    }

    proc_result result = {0};
    err = quilt_pop(src_dir, filename, &result);
    bool already_on_top = !result.signaled && result.exit_code != 0 && result.err_data != NULL &&
                          strstr(result.err_data, "No patch removed") != NULL;
    if (err == NHP_OK && !already_on_top && (result.signaled || result.exit_code != 0)) {
        /* Popping backward to an already-known-good point essentially
         * never conflicts in practice, but report it the same way
         * every other stack.c failure does if it somehow does. */
        quilt_dry_run empty;
        memset(&empty, 0, sizeof(empty));
        nhp_err build_err = fill_conflict(out_conflict, inst->patches[index].id,
                                          inst->patches[index].version, true, &empty, &result);
        err = (build_err == NHP_OK) ? NHP_ECONFLICT : build_err;
    }
    proc_result_free(&result);
    free(filename);
    free(src_dir);
    return err;
}

nhp_err stack_edit_refresh(inst_instance *inst, size_t index, stack_conflict *out_conflict,
                           char **out_diff_path)
{
    *out_diff_path = NULL;
    if (index >= inst->patch_count) {
        return NHP_EINVAL;
    }

    char *src_dir = inst_source_full_path(inst);
    if (src_dir == NULL) {
        return NHP_ENOMEM;
    }

    char *filename = find_patch_filename(src_dir, inst->patches[index].id);
    if (filename == NULL) {
        err_set("could not find %s's patch file under %s/patches", inst->patches[index].id,
                src_dir);
        free(src_dir);
        return NHP_ENOENT;
    }

    proc_result refresh_result = {0};
    nhp_err err = quilt_refresh(src_dir, filename, &refresh_result);
    if (err == NHP_OK && (refresh_result.signaled || refresh_result.exit_code != 0)) {
        err_set("quilt refresh %s failed: %s", filename,
                (refresh_result.err_data != NULL) ? refresh_result.err_data : "unknown error");
        err = NHP_EIO;
    }
    proc_result_free(&refresh_result);
    if (err != NHP_OK) {
        free(filename);
        free(src_dir);
        return err;
    }

    proc_result push_result = {0};
    err = quilt_push_all(src_dir, &push_result);
    bool nothing_above = !push_result.signaled && push_result.err_data != NULL &&
                         (strstr(push_result.err_data, "No patches in series") != NULL ||
                          strstr(push_result.err_data, "fully applied") != NULL);
    bool push_ok =
        err == NHP_OK && !push_result.signaled && (push_result.exit_code == 0 || nothing_above);
    if (err == NHP_OK && !push_ok) {
        /* Something above `index` no longer applies after the refresh. The
         * refreshed patch and stack_edit_begin()'s snapshot are left as-is --
         * the caller can retry, edit further, or revert by hand. */
        quilt_dry_run empty;
        memset(&empty, 0, sizeof(empty));
        nhp_err build_err =
            fill_conflict(out_conflict, inst->patches[index].id, inst->patches[index].version,
                          false, &empty, &push_result);
        proc_result_free(&push_result);
        free(filename);
        free(src_dir);
        return (build_err == NHP_OK) ? NHP_ECONFLICT : build_err;
    }
    proc_result_free(&push_result);
    if (err != NHP_OK) {
        free(filename);
        free(src_dir);
        return err;
    }

    /* "quilt refresh" rewrites the series file, dropping its "-pN" flags --
     * resync it back to nhpm's own format, same as stack_adopt(). Nothing
     * pushed/popped changes, so this never conflicts. */
    patch_slot *slots = NULL;
    err = build_slots(src_dir, inst->patches, inst->patch_count, &slots);
    if (err == NHP_OK) {
        err = write_series_file(src_dir, slots, inst->patch_count);
        free_slots(slots, inst->patch_count);
    }
    if (err != NHP_OK) {
        free(filename);
        free(src_dir);
        return err;
    }

    char *abs_path = patches_file_path(src_dir, filename);
    free(filename);
    if (abs_path == NULL) {
        free(src_dir);
        return NHP_ENOMEM;
    }

    char *data = NULL;
    size_t len = 0;
    if (fs_read_file(abs_path, &data, &len) == NHP_OK) {
        char hex[65];
        sha256_hex(data, len, hex);
        char *new_sha = str_dup(hex);
        if (new_sha != NULL) {
            free(inst->patches[index].sha256);
            inst->patches[index].sha256 = new_sha;
        }
    }
    free(data);
    free(src_dir);

    inst->patches[index].modified = true;

    char *stamp = time_now_iso8601();
    if (stamp == NULL) {
        free(abs_path);
        return NHP_ENOMEM;
    }
    free(inst->stack_modified);
    inst->stack_modified = stamp;

    nhp_err save_err = inst_save(inst);
    if (save_err != NHP_OK) {
        free(abs_path);
        return save_err;
    }

    *out_diff_path = abs_path;
    return NHP_OK;
}

nhp_err stack_new_patch(inst_instance *inst, const char *id, const char *name, size_t *out_index)
{
    char *src_dir = inst_source_full_path(inst);
    if (src_dir == NULL) {
        return NHP_ENOMEM;
    }

    char *slug = slugify(name);
    if (slug == NULL) {
        free(src_dir);
        return NHP_ENOMEM;
    }
    buf fname_buf;
    buf_init(&fname_buf);
    buf_append_fmt(&fname_buf, "%s-%s.diff", id, slug);
    free(slug);
    char *filename = buf_take(&fname_buf);
    if (filename == NULL) {
        free(src_dir);
        return NHP_ENOMEM;
    }

    char *dest_path = patches_file_path(src_dir, filename);
    if (dest_path != NULL && fs_exists(dest_path)) {
        err_set("%s already exists", filename);
        free(dest_path);
        free(filename);
        free(src_dir);
        return NHP_EEXIST;
    }
    free(dest_path);

    char *snapshot_name = NULL;
    nhp_err err = inst_snapshot_create(inst->name, "before new patch", &snapshot_name);
    free(snapshot_name);
    if (err != NHP_OK) {
        free(filename);
        free(src_dir);
        return err;
    }

    proc_result result = {0};
    err = quilt_new(src_dir, filename, &result);
    if (err == NHP_OK && (result.signaled || result.exit_code != 0)) {
        err_set("quilt new %s failed: %s", filename,
                (result.err_data != NULL) ? result.err_data : "unknown error");
        err = NHP_EIO;
    }
    proc_result_free(&result);
    if (err != NHP_OK) {
        free(filename);
        free(src_dir);
        return err;
    }

    /* An empty patch has nothing to refresh yet, so quilt won't write
     * <filename> until the first "quilt refresh" -- write an empty
     * placeholder now so find_patch_filename() has something to find. */
    char *placeholder_path = patches_file_path(src_dir, filename);
    if (placeholder_path == NULL) {
        free(filename);
        free(src_dir);
        return NHP_ENOMEM;
    }
    err = fs_write_atomic(placeholder_path, "", 0);
    free(placeholder_path);
    if (err != NHP_OK) {
        free(filename);
        free(src_dir);
        return err;
    }

    char hex[65];
    sha256_hex("", 0, hex);

    inst_patch *grown = realloc(inst->patches, (inst->patch_count + 1) * sizeof(*grown));
    if (grown == NULL) {
        free(filename);
        free(src_dir);
        return NHP_ENOMEM;
    }
    inst->patches = grown;
    inst_patch *entry = &inst->patches[inst->patch_count];
    memset(entry, 0, sizeof(*entry));
    entry->id = str_dup(id);
    entry->version = str_dup("");
    entry->enabled = true;
    entry->strip = 1;
    entry->sha256 = str_dup(hex);
    entry->modified = false;
    if (entry->id == NULL || entry->version == NULL || entry->sha256 == NULL) {
        inst_patch_free_contents(entry);
        free(filename);
        free(src_dir);
        return NHP_ENOMEM;
    }
    inst->patch_count++;
    *out_index = inst->patch_count - 1;
    free(filename);

    patch_slot *slots = NULL;
    err = build_slots(src_dir, inst->patches, inst->patch_count, &slots);
    if (err == NHP_OK) {
        err = write_series_file(src_dir, slots, inst->patch_count);
        free_slots(slots, inst->patch_count);
    }
    free(src_dir);
    if (err != NHP_OK) {
        return err;
    }

    char *stamp = time_now_iso8601();
    if (stamp == NULL) {
        return NHP_ENOMEM;
    }
    free(inst->stack_modified);
    inst->stack_modified = stamp;

    return inst_save(inst);
}

nhp_err stack_track_file(const inst_instance *inst, const char *path)
{
    char *src_dir = inst_source_full_path(inst);
    if (src_dir == NULL) {
        return NHP_ENOMEM;
    }

    proc_result result = {0};
    nhp_err err = quilt_add(src_dir, path, &result);
    free(src_dir);
    if (err == NHP_OK && (result.signaled || result.exit_code != 0)) {
        err_set("quilt add %s failed: %s", path,
                (result.err_data != NULL) ? result.err_data : "unknown error");
        err = NHP_EIO;
    }
    proc_result_free(&result);
    return err;
}
