#ifndef MF_REST_H
#define MF_REST_H

#include <stddef.h>

/* In-memory REST dispatch (PR-5 / QW-4). No sockets. http_pt calls this. */

#define MF_REST_BODY_CAP 65536

typedef struct mf_rest_request {
    const char *method;     /* "GET", "POST", "PUT", "DELETE" */
    const char *path;       /* "/api/v1/..." ; query stripped */
    const char *body;       /* may be NULL */
    size_t      body_len;
    const char *if_match;   /* If-Match header, nullable */
    const char *peer;       /* client IP, nullable */
} mf_rest_request_t;

typedef struct mf_rest_response {
    int    status;
    char   body[MF_REST_BODY_CAP];
    size_t body_len;
    char   location[128];   /* 201 Location, else empty */
    char   etag[32];        /* config_gen, else empty */
} mf_rest_response_t;

void mf_rest_init(void);

/* Live daemon config pointer (moonflared). NULL → built-in defaults. */
void mf_rest_set_live_config(void *daemon_cfg, const char *path);
const char *mf_rest_listen_spec(void);

/* Fill resp. Always writes JSON into resp->body (NUL-terminated).
 * Returns 0 on dispatch (including 4xx); -1 if method/path buffer is unusable. */
int mf_rest_dispatch(const mf_rest_request_t *req, mf_rest_response_t *resp);

#endif
