#ifndef MF_CLI_ADDR_H
#define MF_CLI_ADDR_H

/* Feature-test macros only count before the first system header.  A
 * source file that includes one ahead of this header has already had
 * _POSIX_C_SOURCE chosen for it (by its own define or by libc), and
 * defining it again here would only draw a redefinition warning. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <stddef.h>
#include <stdbool.h>

#define CLI_HOST_MAX 128

typedef struct {
    char host[CLI_HOST_MAX];
    int  port;
    double timeout;
    int  raw;
} cli_ctx_t;

/* Split "HOST:PORT" into host_str (out, caller-owns if non-NULL) and port_out.
 * Returns 0 on success, -1 on malformed input.
 * If no ':' found, host_str is set to the whole string and port_out = 5250. */
int cli_split_host_port(const char *input, char *host_str, size_t host_cap,
                        int *port_out);

/* Resolve daemon address: --connect > $MOONFLARE_CONNECT > moonflare.json > default.
 * Sets *ctx with resolved host/port/timeout. Returns 0 on success. */
int cli_resolve_addr(const char *connect_flag, const char *config_override,
                     double timeout, int raw, cli_ctx_t *ctx);

#endif /* MF_CLI_ADDR_H */
