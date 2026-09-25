#ifndef MF_CLI_RENDER_H
#define MF_CLI_RENDER_H

#define _POSIX_C_SOURCE 200809L
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <stddef.h>

/* Forward declare cJSON. */
typedef struct cJSON cJSON;

/* Render --list modules table to stdout. Returns 0 on success. */
int cli_render_list_modules(const cJSON *modules, int raw);

/* Render --list drivers table to stdout. Returns 0 on success. */
int cli_render_list_drivers(const cJSON *drivers, int raw);

/* Render --query full module readout to stdout. Returns 0 on success. */
int cli_render_query_module(const cJSON *mod, int raw);

/* Render --status summary to stdout. Returns 0 on success. */
int cli_render_status(const cJSON *status, int raw);

/* Render module history to stdout. Returns 0 on success. */
int cli_render_history(const cJSON *history, int raw);

/* Print usage text to stderr. */
void cli_print_usage(const char *progname);

#endif /* MF_CLI_RENDER_H */
