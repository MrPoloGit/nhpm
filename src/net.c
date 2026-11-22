/* net.c - libcurl wrapper: GET/POST to memory or file, progress, cancel.
 * SPDX-License-Identifier: NGPL */
#include "feature.h"
#include "net.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <curl/curl.h>

/* A page or search result is at most a few hundred KB in practice; this is
 * a generous safety cap, not a figure from docs/features.md. */
#define NET_MAX_MEMORY_BYTES ((size_t)10 * 1024 * 1024)
#define NET_TIMEOUT_SECONDS  20L

static bool g_curl_inited = false;

static void ensure_curl_init(void)
{
    if (!g_curl_inited) {
        (void)curl_global_init(CURL_GLOBAL_DEFAULT);
        (void)atexit(curl_global_cleanup);
        g_curl_inited = true;
    }
}

typedef struct progress_ctx {
    net_progress_fn fn;
    void *user_data;
} progress_ctx;

/* must match libcurl's CURLOPT_XFERINFOFUNCTION signature exactly */
// cppcheck-suppress constParameterCallback
static int xferinfo_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal,
                       curl_off_t ulnow)
{
    const progress_ctx *ctx = clientp;
    if (ctx->fn == NULL) {
        return 0;
    }
    double done = (double)(dlnow + ulnow);
    double total = (double)(dltotal + ultotal);
    return ctx->fn(ctx->user_data, done, total) ? 1 : 0;
}

static CURL *make_curl(const char *url)
{
    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        return NULL;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, NET_TIMEOUT_SECONDS);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "nhpm/" NHPM_VERSION);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    return curl;
}

static void setup_progress(CURL *curl, progress_ctx *ctx, net_progress_fn progress, void *user_data)
{
    ctx->fn = progress;
    ctx->user_data = user_data;
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, (progress != NULL) ? 0L : 1L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, ctx);
}

/* ---- GET/POST to memory ---- */

typedef struct mem_write_ctx {
    buf *out;
    bool exceeded;
} mem_write_ctx;

/* must match libcurl's CURLOPT_WRITEFUNCTION signature exactly */
// cppcheck-suppress constParameterCallback
static size_t mem_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    mem_write_ctx *ctx = userdata;
    size_t total = size * nmemb;
    if (ctx->out->len + total > NET_MAX_MEMORY_BYTES) {
        ctx->exceeded = true;
        return 0; /* a short write makes curl abort the transfer */
    }
    return buf_append(ctx->out, ptr, total) ? total : 0;
}

static nhp_err fetch_to_memory(CURL *curl, const char *url, net_resp *out, net_progress_fn progress,
                               void *user_data)
{
    memset(out, 0, sizeof(*out));

    buf body;
    buf_init(&body);
    mem_write_ctx wctx = {.out = &body, .exceeded = false};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, mem_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &wctx);

    progress_ctx pctx;
    setup_progress(curl, &pctx, progress, user_data);

    CURLcode res = curl_easy_perform(curl);

    nhp_err err = NHP_OK;
    if (wctx.exceeded) {
        err_set("the response from %s was larger than the %zu-byte limit", url,
                NET_MAX_MEMORY_BYTES);
        err = NHP_EIO;
    } else if (res == CURLE_ABORTED_BY_CALLBACK) {
        err_set("the request to %s was canceled", url);
        err = NHP_ECANCELED;
    } else if (res != CURLE_OK) {
        err_set("could not fetch %s: %s", url, curl_easy_strerror(res));
        err = NHP_ENET;
    }

    if (err != NHP_OK) {
        buf_free(&body);
        return err;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    char *effective_url = NULL;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url);

    size_t body_len = body.len;
    out->data = buf_take(&body);
    out->len = body_len;
    out->http_code = http_code;
    out->final_url = str_dup((effective_url != NULL) ? effective_url : url);

    log_debug("net", "%s -> %ld (%zu bytes)", url, http_code, out->len);
    return NHP_OK;
}

nhp_err net_get(const char *url, net_resp *out, net_progress_fn progress, void *user_data)
{
    ensure_curl_init();

    CURL *curl = make_curl(url);
    if (curl == NULL) {
        memset(out, 0, sizeof(*out));
        err_set("could not initialize an HTTP request for %s", url);
        return NHP_ENET;
    }

    nhp_err err = fetch_to_memory(curl, url, out, progress, user_data);
    curl_easy_cleanup(curl);
    return err;
}

nhp_err net_post(const char *url, const char *form, net_resp *out, net_progress_fn progress,
                 void *user_data)
{
    ensure_curl_init();

    CURL *curl = make_curl(url);
    if (curl == NULL) {
        memset(out, 0, sizeof(*out));
        err_set("could not initialize an HTTP request for %s", url);
        return NHP_ENET;
    }
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, form);

    nhp_err err = fetch_to_memory(curl, url, out, progress, user_data);
    curl_easy_cleanup(curl);
    return err;
}

void net_resp_free(net_resp *r)
{
    if (r == NULL) {
        return;
    }
    free(r->data);
    free(r->final_url);
    r->data = NULL;
    r->final_url = NULL;
}

/* ---- download to file ---- */

typedef struct file_write_ctx {
    FILE *fp;
    size_t written;
    size_t max_bytes;
    bool exceeded;
} file_write_ctx;

/* must match libcurl's CURLOPT_WRITEFUNCTION signature exactly */
// cppcheck-suppress constParameterCallback
static size_t file_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    file_write_ctx *ctx = userdata;
    size_t total = size * nmemb;
    if (ctx->written + total > ctx->max_bytes) {
        ctx->exceeded = true;
        return 0;
    }
    size_t written = fwrite(ptr, 1, total, ctx->fp);
    ctx->written += written;
    return written;
}

/* Opens a temp file next to `path` for writing; *out_tmpl is the newly
 * allocated temp path (the caller renames or unlinks it when done). */
static FILE *open_temp_near(const char *path, char **out_tmpl)
{
    const char *slash = strrchr(path, '/');
    char *dir = (slash != NULL) ? str_ndup(path, (size_t)(slash - path)) : str_dup(".");
    if (dir == NULL) {
        return NULL;
    }
    char *tmpl = path_join(dir, ".nhpm-dl-XXXXXX");
    free(dir);
    if (tmpl == NULL) {
        return NULL;
    }

    int fd = mkstemp(tmpl);
    if (fd < 0) {
        err_set("could not create a temporary file next to %s: %s", path, strerror(errno));
        free(tmpl);
        return NULL;
    }
    FILE *fp = fdopen(fd, "wb");
    if (fp == NULL) {
        err_set("could not open a temporary file for %s: %s", path, strerror(errno));
        (void)close(fd);
        (void)unlink(tmpl);
        free(tmpl);
        return NULL;
    }
    *out_tmpl = tmpl;
    return fp;
}

static nhp_err finish_download(FILE *fp, const char *tmpl, const char *path)
{
    nhp_err err = NHP_OK;
    if (fflush(fp) != 0) {
        err_set("could not flush %s: %s", path, strerror(errno));
        err = NHP_EIO;
    }
    if (err == NHP_OK && fsync(fileno(fp)) != 0) {
        err_set("could not fsync %s: %s", path, strerror(errno));
        err = NHP_EIO;
    }
    (void)fclose(fp);

    if (err == NHP_OK && rename(tmpl, path) != 0) {
        err_set("could not rename %s to %s: %s", tmpl, path, strerror(errno));
        err = NHP_EIO;
    }
    return err;
}

nhp_err net_download(const char *url, const char *path, size_t max_bytes, net_progress_fn progress,
                     void *user_data)
{
    ensure_curl_init();

    char *tmpl = NULL;
    FILE *fp = open_temp_near(path, &tmpl);
    if (fp == NULL) {
        return NHP_EIO;
    }

    CURL *curl = make_curl(url);
    if (curl == NULL) {
        (void)fclose(fp);
        (void)unlink(tmpl);
        free(tmpl);
        err_set("could not initialize an HTTP request for %s", url);
        return NHP_ENET;
    }

    file_write_ctx wctx = {.fp = fp, .written = 0, .max_bytes = max_bytes, .exceeded = false};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, file_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &wctx);

    progress_ctx pctx;
    setup_progress(curl, &pctx, progress, user_data);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    nhp_err err = NHP_OK;
    if (wctx.exceeded) {
        err_set("the download from %s was larger than the allowed %zu bytes", url, max_bytes);
        err = NHP_EIO;
    } else if (res == CURLE_ABORTED_BY_CALLBACK) {
        err_set("the download from %s was canceled", url);
        err = NHP_ECANCELED;
    } else if (res != CURLE_OK) {
        err_set("could not download %s: %s", url, curl_easy_strerror(res));
        err = NHP_ENET;
    } else if (http_code >= 400) {
        err_set("could not download %s: HTTP %ld", url, http_code);
        err = NHP_ENET;
    }

    if (err == NHP_OK) {
        err = finish_download(fp, tmpl, path);
    } else {
        (void)fclose(fp);
    }

    if (err != NHP_OK) {
        (void)unlink(tmpl);
    }
    free(tmpl);

    log_debug("net", "download %s -> %s (HTTP %ld)", url, (err == NHP_OK) ? "ok" : "failed",
              http_code);
    return err;
}
