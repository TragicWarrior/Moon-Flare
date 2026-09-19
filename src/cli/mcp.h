#ifndef MF_MCP_H
#define MF_MCP_H

#define _POSIX_C_SOURCE 200809L
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "cli_addr.h"

/* Run the MCP server loop on stdio until stdin closes. Returns exit code. */
int mcp_run(cli_ctx_t *ctx);

#endif /* MF_MCP_H */
