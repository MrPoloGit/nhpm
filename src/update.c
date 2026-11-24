/* update.c - checks GitHub for a newer nhpm release (nhpm-plan.md M8).
 * SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "update.h"

#include "net.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* nhpm isn't released publicly yet (no tag exists), so a 404 here is the
 * expected answer, not an error -- see update_check() below. */
#define UPDATE_REPO_OWNER "MrPoloGit"
#define UPDATE_REPO_NAME  "nhpm"
#define UPDATE_LATEST_URL                                                                          \
    "https://github.com/" UPDATE_REPO_OWNER "/" UPDATE_REPO_NAME "/releases/latest"
#define UPDATE_TAG_MARKER "/releases/tag/"

static const char *skip_v_prefix(const char *s)
{
    return (s[0] == 'v' || s[0] == 'V') ? s + 1 : s;
}

/* Up to 4 dot-separated numeric components ("0.1" -> {0, 1, 0, 0}); a
 * non-numeric or missing component reads as 0, same as a shorter version
 * being padded with zeroes. */
static void parse_version_parts(const char *s, long out[4])
{
    const char *p = skip_v_prefix(s);
    for (int i = 0; i < 4; i++) {
        char *end = NULL;
        long n = strtol(p, &end, 10);
        out[i] = (end == p) ? 0 : n;
        p = (*end == '.') ? end + 1 : end;
    }
}

int update_version_compare(const char *a, const char *b)
{
    long pa[4];
    long pb[4];
    parse_version_parts(a, pa);
    parse_version_parts(b, pb);
    for (int i = 0; i < 4; i++) {
        if (pa[i] != pb[i]) {
            return (pa[i] < pb[i]) ? -1 : 1;
        }
    }
    return 0;
}

void update_check_result_free(update_check_result *r)
{
    if (r == NULL) {
        return;
    }
    free(r->latest_version);
    r->latest_version = NULL;
}

static char *update_cache_path(void)
{
    char *dir = cfg_xdg_cache_dir();
    if (dir == NULL) {
        return NULL;
    }
    char *path = path_join(dir, "update-check");
    free(dir);
    return path;
}

/* The cache file holds exactly one line: the latest known tag, or empty
 * if the last check found no releases. Returns false (out_version
 * untouched) if there's no usable, fresh-enough cache. */
static bool read_cache(const char *path, long max_age_days, char **out_version)
{
    time_t mtime;
    if (fs_mtime(path, &mtime) != NHP_OK) {
        return false;
    }
    time_t now = time(NULL);
    long max_age_seconds = max_age_days * 24L * 3600L;
    if (now < mtime || now - mtime >= max_age_seconds) {
        return false;
    }

    char *data = NULL;
    size_t len = 0;
    if (fs_read_file(path, &data, &len) != NHP_OK) {
        return false;
    }
    while (len > 0 && (data[len - 1] == '\n' || data[len - 1] == '\r')) {
        data[--len] = '\0';
    }
    *out_version = data;
    return true;
}

static void fill_from_version(update_check_result *out, char *version)
{
    out->checked = true;
    if (version[0] != '\0') {
        out->latest_version = version;
        out->update_available = update_version_compare(NHPM_VERSION, version) < 0;
    } else {
        free(version);
    }
}

void update_check_cached(long max_age_days, update_check_result *out)
{
    memset(out, 0, sizeof(*out));
    char *path = update_cache_path();
    if (path == NULL) {
        return;
    }
    char *cached = NULL;
    if (read_cache(path, max_age_days, &cached)) {
        fill_from_version(out, cached);
    }
    free(path);
}

/* Strips a trailing '/' GitHub's redirect doesn't actually send, but a
 * future change to the URL shape might -- cheap to guard against. */
static char *extract_tag(const char *final_url)
{
    const char *marker = strstr(final_url, UPDATE_TAG_MARKER);
    if (marker == NULL) {
        return str_dup("");
    }
    char *tag = str_dup(marker + strlen(UPDATE_TAG_MARKER));
    if (tag == NULL) {
        return NULL;
    }
    size_t len = strlen(tag);
    if (len > 0 && tag[len - 1] == '/') {
        tag[len - 1] = '\0';
    }
    return tag;
}

nhp_err update_check(const cfg_app *app, bool force, update_check_result *out)
{
    memset(out, 0, sizeof(*out));

    if (!app->check_updates && !force) {
        return NHP_OK;
    }

    if (!force) {
        update_check_cached(app->update_check_max_age_days, out);
        if (out->checked) {
            return NHP_OK;
        }
    }

    net_resp resp = {0};
    nhp_err err = net_get(UPDATE_LATEST_URL, &resp, NULL, NULL);
    if (err != NHP_OK) {
        net_resp_free(&resp);
        return err;
    }

    char *latest;
    if (resp.http_code == 404) {
        /* No releases published yet -- not an error. */
        latest = str_dup("");
    } else if (resp.http_code >= 200 && resp.http_code < 400 && resp.final_url != NULL) {
        latest = extract_tag(resp.final_url);
    } else {
        latest = str_dup("");
    }
    net_resp_free(&resp);
    if (latest == NULL) {
        return NHP_ENOMEM;
    }

    char *cache_path = update_cache_path();
    if (cache_path != NULL) {
        char *cache_dir = cfg_xdg_cache_dir();
        if (cache_dir != NULL) {
            (void)fs_mkdir_p(cache_dir, 0700);
            free(cache_dir);
        }
        (void)fs_write_atomic(cache_path, latest, strlen(latest));
        free(cache_path);
    }

    fill_from_version(out, latest);
    return NHP_OK;
}
