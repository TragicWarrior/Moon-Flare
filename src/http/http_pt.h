#ifndef MF_HTTP_PT_H
#define MF_HTTP_PT_H

#include "protothread.h"

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/select.h>

#define MF_MAX_HTTP_CLIENTS 16
#define MF_HTTP_HDR_CAP     8192
#define MF_HTTP_BODY_CAP    65536
#define MF_HTTP_IN_CAP      (MF_HTTP_HDR_CAP + MF_HTTP_BODY_CAP)
#define MF_HTTP_OUT_CAP     65536
#define MF_HTTP_IDLE_S      30.0
#define MF_HTTP_SERVER_ID   "moonflared/0.1.0"

#define MF_IO_WANT_READ  1u
#define MF_IO_WANT_WRITE 2u

enum mf_http_state {
    HTTP_RECV_HEADERS = 0,
    HTTP_RECV_BODY,
    HTTP_SEND,
    HTTP_CLOSE
};

struct mf_http;

typedef struct {
    pt_func_t      pt_func;
    struct mf_http *srv;
} mf_http_accept_env_t;

typedef struct {
    pt_func_t      pt_func;
    struct mf_http *srv;
    int            idx;
} mf_http_conn_env_t;

typedef struct mf_http_conn {
    bool     in_use;
    int      fd;
    unsigned select_mask;
    int      state;

    char     in[MF_HTTP_IN_CAP];
    size_t   in_len;
    size_t   last_len;          /* phr last_len */
    size_t   header_len;
    size_t   content_length;

    char     out[MF_HTTP_OUT_CAP];
    size_t   out_len;
    size_t   out_off;

    int      minor;             /* HTTP/1.x */
    bool     close_after;
    bool     req_close;

    char     method[16];
    char     path[128];
    char     peer[64];

    double   last_progress;

    pt_thread_t        thr;
    mf_http_conn_env_t env;
} mf_http_conn_t;

typedef struct mf_http {
    protothread_t          pts;
    char                  *chan_tick;
    volatile sig_atomic_t *quit;
    int                    listen_fd;
    double                 idle_s;
    double                 start_mono;
    bool                   debug;

    mf_http_conn_t         conns[MF_MAX_HTTP_CLIENTS];
    mf_http_accept_env_t   accept_env;
    pt_thread_t            accept_thr;
} mf_http_t;

/* Daemon (or test) provides logging. prio is syslog LOG_*. */
void mf_log(int prio, const char *fmt, ...);
double mf_mono_now(void);

void mf_http_init(mf_http_t *h, protothread_t pts, char *chan_tick,
                  volatile sig_atomic_t *quit, int listen_fd,
                  double idle_s, bool debug);
void mf_http_start(mf_http_t *h);
void mf_http_prepare_fds(mf_http_t *h, fd_set *rset, fd_set *wset, int *maxfd);
void mf_http_close_all(mf_http_t *h);

#endif
