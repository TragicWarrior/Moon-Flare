#define _POSIX_C_SOURCE 200809L
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "cli_addr.h"
#include "cli_http.h"
#include "cli_render.h"
#include "mcp.h"

#include <cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <errno.h>
#include "config/config.h"

/* ── Config merge helpers ───────────────────────────────────────────── */

/* Merge offline device config (from moonflared.json) into the device list
 * so configured-but-offline devices still appear in --list. */
static cJSON *merge_offline_config(cJSON *devices, const char *config_override)
{
    cJSON *merged;
    cJSON *item;
    mf_daemon_config_t daemon_cfg;
    int i;

    /* If we can't load the daemon config, signal "no merge" (NULL) so the
     * caller renders (and frees) the original list. Returning `devices` here
     * would let the caller free the same object twice (double-free). */
    if (mf_config_load(config_override, &daemon_cfg) != 0)
        return NULL;

    /* Already have all devices from the daemon; no merge needed
     * unless there are configured-but-offline devices. */
    merged = cJSON_Duplicate(devices, 1);
    if (!merged)
        return NULL;

    for (i = 0; i < daemon_cfg.n_devices; i++)
    {
        cJSON *existing = NULL;
        const char *uuid = daemon_cfg.devices[i].uuid;
        int found = 0;

        cJSON_ArrayForEach(item, merged)
        {
            const cJSON *uid = cJSON_GetObjectItemCaseSensitive(item, "id");
            if (cJSON_IsString(uid) && strcmp(uid->valuestring, uuid) == 0)
            {
                existing = item;
                found = 1;
                break;
            }
        }

        if (!found)
        {
            /* Add the offline configured device. */
            cJSON *dev = cJSON_CreateObject();
            char online_str[8];
            snprintf(online_str, sizeof(online_str), "%s", "offline");
            cJSON_AddStringToObject(dev, "id", uuid);
            cJSON_AddStringToObject(dev, "name", daemon_cfg.devices[i].name);
            cJSON_AddStringToObject(dev, "kind", daemon_cfg.devices[i].kind);
            cJSON_AddStringToObject(dev, "driver", daemon_cfg.devices[i].driver);
            cJSON_AddBoolToObject(dev, "online", 0);
            cJSON_AddItemToArray(merged, dev);
        }
        else
        {
            /* Mark enabled column if we had config. */
            if (daemon_cfg.devices[i].enabled)
            {
                cJSON_AddTrueToObject(existing, "enabled");
            }
        }
    }

    return merged;
}

/* ── Resolve device by name ─────────────────────────────────────────── */

static char *resolve_name_to_uuid(cli_ctx_t *ctx, const char *name)
{
    cli_http_resp_t resp;
    char *uuid = NULL;
    cJSON *root;
    cJSON *item;

    memset(&resp, 0, sizeof(resp));
    if (cli_http_request(ctx->host, ctx->port, "GET", "/api/v1/devices", NULL,
                         ctx->timeout, &resp, NULL, 0) < 0)
        return NULL;

    if (resp.status < 200 || resp.status >= 300)
    {
        cli_http_resp_free(&resp);
        return NULL;
    }

    root = cJSON_Parse(resp.body);
    cli_http_resp_free(&resp);

    if (!root || !cJSON_IsArray(root))
        return NULL;

    cJSON_ArrayForEach(item, root)
    {
        const cJSON *name_field = cJSON_GetObjectItemCaseSensitive(item, "name");
        const cJSON *id_field = cJSON_GetObjectItemCaseSensitive(item, "id");
        if (cJSON_IsString(name_field) && cJSON_IsString(id_field))
        {
            if (strcasecmp(name_field->valuestring, name) == 0)
            {
                uuid = strdup(id_field->valuestring);
                break;
            }
        }
    }

    cJSON_Delete(root);
    return uuid;
}

/* ── Subcommand handlers ────────────────────────────────────────────── */

static int cmd_list_devices(cli_ctx_t *ctx, const char *config_override)
{
    cli_http_resp_t resp;
    cJSON *root;
    int rc = 1;

    memset(&resp, 0, sizeof(resp));
    if (cli_http_request(ctx->host, ctx->port, "GET", "/api/v1/devices", NULL,
                         ctx->timeout, &resp, NULL, 0) < 0)
    {
        fprintf(stderr, "error: connection failed\n");
        return 1;
    }

    if (resp.status < 200 || resp.status >= 300)
    {
        fprintf(stderr, "error: HTTP %d\n", resp.status);
        cli_http_resp_free(&resp);
        return 1;
    }

    root = cJSON_Parse(resp.body);
    cli_http_resp_free(&resp);

    if (!root)
    {
        fprintf(stderr, "error: failed to parse device list\n");
        return 1;
    }

    /* Merge offline config if available. */
    if (!ctx->raw)
    {
        cJSON *merged = merge_offline_config(root, config_override);
        if (merged)
        {
            rc = cli_render_list_devices(merged, 0);
            cJSON_Delete(merged);
        }
        else
        {
            rc = cli_render_list_devices(root, 0);
        }
    }
    else
    {
        rc = cli_render_list_devices(root, 1);
    }

    cJSON_Delete(root);
    return rc < 0 ? 1 : 0;
}

static int cmd_list_drivers(cli_ctx_t *ctx)
{
    cli_http_resp_t resp;
    cJSON *root;
    int rc;

    memset(&resp, 0, sizeof(resp));
    if (cli_http_request(ctx->host, ctx->port, "GET", "/api/v1/drivers", NULL,
                         ctx->timeout, &resp, NULL, 0) < 0)
    {
        fprintf(stderr, "error: connection failed\n");
        return 1;
    }

    if (resp.status < 200 || resp.status >= 300)
    {
        fprintf(stderr, "error: HTTP %d\n", resp.status);
        cli_http_resp_free(&resp);
        return 1;
    }

    root = cJSON_Parse(resp.body);
    cli_http_resp_free(&resp);

    if (!root)
    {
        fprintf(stderr, "error: failed to parse drivers list\n");
        return 1;
    }

    {
        const cJSON *drivers = cJSON_GetObjectItemCaseSensitive(root, "drivers");
        if (cJSON_IsArray(drivers))
        {
            rc = cli_render_list_drivers(drivers, ctx->raw ? 1 : 0);
        }
        else if (cJSON_IsObject(root))
        {
            rc = cli_render_list_drivers(root, ctx->raw ? 1 : 0);
        }
        else
        {
            rc = cli_render_list_drivers(root, ctx->raw ? 1 : 0);
        }
    }
    cJSON_Delete(root);
    return rc < 0 ? 1 : 0;
}

static int cmd_query(cli_ctx_t *ctx, const char *id_or_name)
{
    char *uuid = NULL;
    char path[600];
    cli_http_resp_t resp;
    int rc = 1;

    /* Resolve: if it looks like a uuid, use it directly; else name lookup. */
    if (!cli_is_uuid(id_or_name))
    {
        uuid = resolve_name_to_uuid(ctx, id_or_name);
        if (!uuid)
        {
            fprintf(stderr, "error: device not found: %s\n", id_or_name);
            return 1;
        }
    }
    if (!uuid)
        uuid = strdup(id_or_name);

    {
        char enc[512];
        cli_url_encode(uuid, enc, sizeof(enc));
        snprintf(path, sizeof(path), "/api/v1/devices/%s", enc);
    }
    free(uuid);

    memset(&resp, 0, sizeof(resp));
    if (cli_http_request(ctx->host, ctx->port, "GET", path, NULL,
                         ctx->timeout, &resp, NULL, 0) < 0)
    {
        fprintf(stderr, "error: connection failed\n");
        return 1;
    }

    if (resp.status == 404)
    {
        fprintf(stderr, "error: device not found: %s\n", id_or_name);
        cli_http_resp_free(&resp);
        return 1;
    }

    if (resp.status < 200 || resp.status >= 300)
    {
        fprintf(stderr, "error: HTTP %d\n", resp.status);
        cli_http_resp_free(&resp);
        return 1;
    }

    {
        cJSON *root = cJSON_Parse(resp.body);
        cli_http_resp_free(&resp);

        if (!root)
        {
            fprintf(stderr, "error: failed to parse device\n");
            return 1;
        }

        rc = cli_render_query_device(root, ctx->raw ? 1 : 0);
        cJSON_Delete(root);
    }

    return rc < 0 ? 1 : 0;
}

static int cmd_status(cli_ctx_t *ctx)
{
    cli_http_resp_t resp;
    cJSON *root;
    int rc;

    memset(&resp, 0, sizeof(resp));
    if (cli_http_request(ctx->host, ctx->port, "GET", "/api/v1/status", NULL,
                         ctx->timeout, &resp, NULL, 0) < 0)
    {
        fprintf(stderr, "error: connection failed\n");
        return 1;
    }

    if (resp.status < 200 || resp.status >= 300)
    {
        fprintf(stderr, "error: HTTP %d\n", resp.status);
        cli_http_resp_free(&resp);
        return 1;
    }

    root = cJSON_Parse(resp.body);
    cli_http_resp_free(&resp);

    if (!root)
    {
        fprintf(stderr, "error: failed to parse status\n");
        return 1;
    }

    rc = cli_render_status(root, ctx->raw ? 1 : 0);
    cJSON_Delete(root);
    return rc < 0 ? 1 : 0;
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    const char *connect_flag = NULL;
    const char *config_override = NULL;
    const char *list_what = "devices";
    const char *query_id = NULL;
    double timeout = 5.0;
    int raw = 0;
    int do_list = 0;
    int do_query = 0;
    int do_status = 0;
    int do_mcp = 0;
    int do_help = 0;
    int do_version = 0;
    int i;
    int cmd_set = 0;
    cli_ctx_t ctx;
    int rc;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--connect") == 0)
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "error: --connect requires an argument\n");
                return 2;
            }
            connect_flag = argv[++i];
        }
        else if (strcmp(argv[i], "--config") == 0)
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "error: --config requires an argument\n");
                return 2;
            }
            config_override = argv[++i];
        }
        else if (strcmp(argv[i], "--timeout") == 0)
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "error: --timeout requires an argument\n");
                return 2;
            }
            timeout = atof(argv[++i]);
            if (timeout <= 0)
            {
                fprintf(stderr, "error: --timeout must be positive\n");
                return 2;
            }
        }
        else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--formatted") == 0)
        {
            raw = 0;
        }
        else if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--raw") == 0)
        {
            raw = 1;
        }
        else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--list") == 0)
        {
            do_list = 1;
            cmd_set++;
            if (i + 1 < argc && argv[i + 1][0] != '-')
            {
                list_what = argv[++i];
            }
        }
        else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--query") == 0)
        {
            do_query = 1;
            cmd_set++;
            if (i + 1 >= argc)
            {
                fprintf(stderr, "error: --query requires an argument\n");
                return 2;
            }
            query_id = argv[++i];
        }
        else if (strcmp(argv[i], "--status") == 0)
        {
            do_status = 1;
            cmd_set++;
        }
        else if (strcmp(argv[i], "--mcp") == 0)
        {
            do_mcp = 1;
            cmd_set++;
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
        {
            do_help = 1;
            cmd_set++;
        }
        else if (strcmp(argv[i], "--version") == 0)
        {
            do_version = 1;
            cmd_set++;
        }
        else
        {
            fprintf(stderr, "error: unknown option: %s\n", argv[i]);
            return 2;
        }
    }

    /* --formatted and --raw: last one wins; default is formatted. */
    /* Already handled above in the arg parsing loop. */

    if (do_help)
    {
        cli_print_usage(argv[0]);
        return 0;
    }

    if (do_version)
    {
        printf("moonflare-cli/" MF_VERSION "\n");
        return 0;
    }

    /* Must have exactly one command. */
    if (cmd_set != 1)
    {
        fprintf(stderr, "error: exactly one command required (-l, -q, --status, --mcp)\n");
        return 2;
    }

    /* Resolve daemon address. */
    if (cli_resolve_addr(connect_flag, config_override, timeout, raw, &ctx) < 0)
    {
        return 1;
    }
    ctx.raw = raw;

    if (do_mcp)
    {
        rc = mcp_run(&ctx);
        return rc;
    }

    if (do_list)
    {
        if (strcmp(list_what, "drivers") == 0)
        {
            rc = cmd_list_drivers(&ctx);
        }
        else
        {
            rc = cmd_list_devices(&ctx, config_override);
        }
        return rc;
    }

    if (do_query)
    {
        rc = cmd_query(&ctx, query_id);
        return rc;
    }

    if (do_status)
    {
        rc = cmd_status(&ctx);
        return rc;
    }

    return 2;
}
