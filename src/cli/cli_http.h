#ifndef MF_CLI_HTTP_H
#define MF_CLI_HTTP_H

#define _POSIX_C_SOURCE 200809L
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <stddef.h>

/* Forward declare cJSON for cli_get_json() signature. */
typedef struct cJSON cJSON;

typedef struct {
    int    status;
    char  *body;
    size_t body_len;
} cli_http_resp_t;

int cli_http_request(const char *host, int port, const char *method,
                     const char *path, const char *json,
                     double timeout_s, cli_http_resp_t *resp,
                     char *errbuf, size_t errcap);

void cli_http_resp_free(cli_http_resp_t *r);

/* Percent-encode `in` into `out` for safe use as one URL path segment
 * (RFC 3986 unreserved chars pass through; everything else -> %XX).
 * Always NUL-terminates; truncates if `out` is too small.
 * Returns bytes written (excluding NUL), or -1 on bad args. */
int cli_url_encode(const char *in, char *out, size_t cap);

/* 1 if `s` is non-empty and made up only of hex digits and '-' (uuid-shaped),
 * else 0. Used to decide whether a token can be a device id vs. a name. */
int cli_is_uuid(const char *s);

#endif /* MF_CLI_HTTP_H */
