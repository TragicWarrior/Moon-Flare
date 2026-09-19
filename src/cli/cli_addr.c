#define _POSIX_C_SOURCE 200809L
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "cli_addr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "config/config.h"

/* ── Address resolution ─────────────────────────────────────────────── */

int cli_split_host_port(const char *input, char *host_str, size_t host_cap,
                        int *port_out)
{
    char copy[256];
    char *colon;

    if (!input || !input[0])
        return -1;

    if (host_cap == 0)
        return -1;

    if ((size_t)strlen(input) >= sizeof(copy))
        return -1;

    strncpy(copy, input, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';
    colon = strrchr(copy, ':');

    if (!colon) {
        /* No colon: whole string is host, default port. */
        snprintf(host_str, host_cap, "%s", copy);
        *port_out = 5250;
        if (host_str[0] == '\0')
            snprintf(host_str, host_cap, "%s", "127.0.0.1");
        return 0;
    }

    *colon = '\0';
    {
        long p = strtol(colon + 1, NULL, 10);
        if (p <= 0 || p > 65535) {
            return -1;
        }
        *port_out = (int)p;
    }

    if (copy[0] == '\0')
        snprintf(host_str, host_cap, "%s", "127.0.0.1");
    else
        snprintf(host_str, host_cap, "%s", copy);

    return 0;
}

int cli_resolve_addr(const char *connect_flag, const char *config_override,
                     double timeout, int raw, cli_ctx_t *ctx)
{
    const char *env_val = NULL;
    int rc;

    if (!ctx)
        return -1;

    memset(ctx, 0, sizeof(*ctx));
    ctx->port = 5250;
    ctx->timeout = timeout > 0 ? timeout : 5.0;
    ctx->raw = raw;

    /* 1. --connect flag. */
    if (connect_flag && connect_flag[0]) {
        rc = cli_split_host_port(connect_flag, ctx->host, sizeof(ctx->host),
                                 &ctx->port);
        if (rc < 0) {
            fprintf(stderr, "error: invalid --connect value: %s\n", connect_flag);
            return -1;
        }
        return 0;
    }

    /* 2. $MOONFLARE_CONNECT env. */
    env_val = getenv("MOONFLARE_CONNECT");
    if (env_val && env_val[0]) {
        rc = cli_split_host_port(env_val, ctx->host, sizeof(ctx->host),
                                 &ctx->port);
        if (rc < 0) {
            fprintf(stderr, "error: invalid $MOONFLARE_CONNECT value: %s\n",
                    env_val);
            return -1;
        }
        return 0;
    }

    /* 3. moonflare.json -> connect. */
    {
        mf_tui_config_t tui_cfg;
        rc = mf_tui_config_load(config_override, &tui_cfg);
        if (rc == 0 && tui_cfg.connect[0]) {
            rc = cli_split_host_port(tui_cfg.connect, ctx->host, sizeof(ctx->host),
                                     &ctx->port);
            if (rc < 0) {
                fprintf(stderr, "error: invalid connect in config: %s\n",
                        tui_cfg.connect);
                return -1;
            }
            return 0;
        }
    }

    /* 4. Default 127.0.0.1:5250. */
    snprintf(ctx->host, sizeof(ctx->host), "%s", "127.0.0.1");
    ctx->port = 5250;

    return 0;
}
