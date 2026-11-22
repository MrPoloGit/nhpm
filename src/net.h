/* net.h - libcurl wrapper: GET/POST to memory or file, progress, cancel.
 * SPDX-License-Identifier: NGPL */
#ifndef NHPM_NET_H
#define NHPM_NET_H

#include "util.h"

#include <stddef.h>

typedef struct net_resp {
    char *data; /* NUL-terminated response body */
    size_t len; /* body length, not counting the added NUL */
    long http_code;
    char *final_url; /* the URL actually fetched, after redirects */
} net_resp;

/* Called during a transfer with bytes done/total so far (total may be 0 if
 * unknown); return true to cancel (the caller gets NHP_ECANCELED back). */
typedef bool (*net_progress_fn)(void *user_data, double done_bytes, double total_bytes);

/* GETs `url` into memory. *out is always filled in (free with
 * net_resp_free()), even on error. Redirects are followed, TLS is always
 * verified, and the response body is capped at a few megabytes. */
nhp_err net_get(const char *url, net_resp *out, net_progress_fn progress, void *user_data);

/* POSTs `form` (an already-encoded "key=value&key2=value2" body) to `url`. */
nhp_err net_post(const char *url, const char *form, net_resp *out, net_progress_fn progress,
                 void *user_data);

/* Downloads `url` straight to `path` (temp file + fsync + rename, so a
 * failed or canceled download never leaves a partial file at `path`),
 * refusing anything over `max_bytes`. */
nhp_err net_download(const char *url, const char *path, size_t max_bytes, net_progress_fn progress,
                     void *user_data);

void net_resp_free(net_resp *r);

#endif /* NHPM_NET_H */
