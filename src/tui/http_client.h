#ifndef MF_TUI_HTTP_CLIENT_H
#define MF_TUI_HTTP_CLIENT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/select.h>

#define MF_CLI_IO_READ  1u
#define MF_CLI_IO_WRITE 2u

typedef enum {
    MF_CONN_UP = 0,
    MF_CONN_CONNECTING,
    MF_CONN_WAIT
} mf_conn_state_t;

typedef struct mf_http_cli {
    char            host[128];
    int             port;
    int             fd;
    mf_conn_state_t state;
    unsigned        want;
    double          last_fresh;
    double          conn_since;
    double          next_try;
    double          backoff;
    int             stale;
    int             inflight;
    int             skipped;
    char            path[160];
    char            out[512];
    size_t          out_len, out_off;
    char            in[65536];
    size_t          in_len;
    int             have_body;
    char            body[65536];
    size_t          body_len;
    uint64_t        last_seq;
    char            status_line[96];
} mf_http_cli_t;

void mf_http_cli_init(mf_http_cli_t *c, const char *host, int port);
void mf_http_cli_close(mf_http_cli_t *c);
void mf_http_cli_start(mf_http_cli_t *c, double now);
void mf_http_cli_prepare_fds(mf_http_cli_t *c, fd_set *r, fd_set *w, int *maxfd);
void mf_http_cli_pump(mf_http_cli_t *c, int readable, int writable, double now);

/* 1 = queued, 0 = skipped (already in flight), -1 = not connected. */
int  mf_http_cli_get(mf_http_cli_t *c, const char *path);
int  mf_http_cli_take_body(mf_http_cli_t *c, char *dst, size_t cap);

#endif
