#ifndef MF_MCP_H
#define MF_MCP_H

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

#include "cli_addr.h"

/* Run the MCP server loop on stdio until stdin closes. Returns exit code. */
int mcp_run(cli_ctx_t *ctx);

#endif /* MF_MCP_H */
