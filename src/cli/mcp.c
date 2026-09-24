#define _POSIX_C_SOURCE 200809L
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "mcp.h"
#include "cli_http.h"

#include <cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>

/* ── JSON-RPC helpers ───────────────────────────────────────────────── */

static void send_json(const char *line_out)
{
    printf("%s\n", line_out);
    fflush(stdout);
}

/* Serialize one JSON-RPC envelope to stdout, then free it. Takes ownership. */
static void send_env(cJSON *env)
{
    char *out;
    if (!env)
        return;
    out = cJSON_PrintUnformatted(env);
    if (out)
    {
        send_json(out);
        free(out);
    }
    cJSON_Delete(env);
}

/* id is the request's "id" item (number, string, or null); it is echoed back
 * with its original JSON type preserved. NULL id → JSON null (parse errors). */
static cJSON *make_resp(const cJSON *id, cJSON *result)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "jsonrpc", "2.0");
    if (id)
        cJSON_AddItemToObject(r, "id", cJSON_Duplicate(id, 1));
    else
        cJSON_AddItemToObject(r, "id", cJSON_CreateNull());
    if (result)
        cJSON_AddItemToObject(r, "result", result);
    return r;
}

static cJSON *make_error(const cJSON *id, int code, const char *message)
{
    cJSON *r = cJSON_CreateObject();
    cJSON *err = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "jsonrpc", "2.0");
    if (id)
        cJSON_AddItemToObject(r, "id", cJSON_Duplicate(id, 1));
    else
        cJSON_AddItemToObject(r, "id", cJSON_CreateNull());
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    cJSON_AddItemToObject(r, "error", err);
    return r;
}

/* ── HTTP fetch helpers ─────────────────────────────────────────────── */

static cJSON *fetch_get(cli_ctx_t *ctx, const char *path, int *http_status)
{
    cli_http_resp_t resp;
    cJSON *root = NULL;
    char errbuf[256];
    const cJSON *err_field;

    memset(&resp, 0, sizeof(resp));
    if (cli_http_request(ctx->host, ctx->port, "GET", path, NULL,
                         ctx->timeout, &resp, errbuf, sizeof(errbuf)) < 0)
    {
        fprintf(stderr, "mcp: http error: %s\n", errbuf);
        return NULL;
    }

    if (http_status)
        *http_status = resp.status;

    if (resp.status < 200 || resp.status >= 300)
    {
        fprintf(stderr, "mcp: http %d: %s\n", resp.status,
            resp.body ? resp.body : "(empty)");
        cli_http_resp_free(&resp);
        return NULL;
    }

    root = cJSON_Parse(resp.body);
    cli_http_resp_free(&resp);

    if (!root)
    {
        fprintf(stderr, "mcp: json parse error\n");
        return NULL;
    }

    err_field = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsString(err_field) && err_field->valuestring)
    {
        fprintf(stderr, "mcp: server error: %s\n", err_field->valuestring);
        cJSON_Delete(root);
        return NULL;
    }

    return root;
}

static char *mcp_resolve_device(cli_ctx_t *ctx, const char *id_or_name)
{
    cJSON *root = NULL;
    cJSON *item;
    char *uuid = NULL;
    char enc[512];
    char path[600];

    /* Only try a direct by-id fetch when the token is uuid-shaped: the
     * /devices/{id} endpoint is keyed by uuid, so a name would just 404
     * (and an unescaped one with spaces would 400). Encode defensively. */
    if (cli_is_uuid(id_or_name))
    {
        cli_url_encode(id_or_name, enc, sizeof(enc));
        snprintf(path, sizeof(path), "/api/v1/devices/%s", enc);
        root = fetch_get(ctx, path, NULL);
        if (root)
        {
            const cJSON *idf = cJSON_GetObjectItemCaseSensitive(root, "id");
            if (cJSON_IsString(idf))
                uuid = strdup(idf->valuestring);
            cJSON_Delete(root);
            if (uuid)
                return uuid;
        }
    }

    /* Fall back to name lookup. */
    {
        cJSON *devs = fetch_get(ctx, "/api/v1/devices", NULL);
        if (devs && cJSON_IsArray(devs))
        {
            cJSON_ArrayForEach(item, devs)
            {
                const cJSON *nf = cJSON_GetObjectItemCaseSensitive(item, "name");
                const cJSON *idf = cJSON_GetObjectItemCaseSensitive(item, "id");
                if (cJSON_IsString(nf) && cJSON_IsString(idf) &&
                    strcasecmp(nf->valuestring, id_or_name) == 0)
                {
                    uuid = strdup(idf->valuestring);
                    break;
                }
            }
        }
        if (devs)
            cJSON_Delete(devs);
    }

    return uuid;
}

/* ── Tool handlers ──────────────────────────────────────────────────── */

static cJSON *handle_list_devices(cli_ctx_t *ctx)
{
    cJSON *devs = fetch_get(ctx, "/api/v1/devices", NULL);
    char *text;
    cJSON *result, *content, *item;

    if (!devs)
    {
        text = strdup("error: failed to fetch devices");
        result = cJSON_CreateObject();
        content = cJSON_CreateArray();
        item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "type", "text");
        cJSON_AddStringToObject(item, "text", text ? text : "error: failed");
        cJSON_AddItemToArray(content, item);
        cJSON_AddItemToObject(result, "content", content);
        cJSON_AddTrueToObject(result, "isError");
        free(text);
        return result;
    }

    text = cJSON_PrintUnformatted(devs);
    result = cJSON_CreateObject();
    content = cJSON_CreateArray();
    item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text ? text : "");
    cJSON_AddItemToArray(content, item);
    cJSON_AddItemToObject(result, "content", content);
    cJSON_AddFalseToObject(result, "isError");

    cJSON_Delete(devs);
    free(text);
    return result;
}

static cJSON *handle_query_device(cli_ctx_t *ctx, const cJSON *args)
{
    const cJSON *id_field = cJSON_GetObjectItemCaseSensitive(args, "id");
    char *uuid = NULL;
    char path[600];
    cJSON *dev;
    char *text;
    cJSON *result, *content, *item;

    if (!cJSON_IsString(id_field) || !id_field->valuestring)
    {
        text = strdup("error: missing required parameter: id");
        result = cJSON_CreateObject();
        content = cJSON_CreateArray();
        item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "type", "text");
        cJSON_AddStringToObject(item, "text", text ? text : "error: failed");
        cJSON_AddItemToArray(content, item);
        cJSON_AddItemToObject(result, "content", content);
        cJSON_AddTrueToObject(result, "isError");
        free(text);
        return result;
    }

    uuid = mcp_resolve_device(ctx, id_field->valuestring);
    if (!uuid)
    {
        text = malloc(256);
        snprintf(text, 256, "error: device not found: %s", id_field->valuestring);
        result = cJSON_CreateObject();
        content = cJSON_CreateArray();
        item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "type", "text");
        cJSON_AddStringToObject(item, "text", text ? text : "error: failed");
        cJSON_AddItemToArray(content, item);
        cJSON_AddItemToObject(result, "content", content);
        cJSON_AddTrueToObject(result, "isError");
        free(text);
        return result;
    }

    {
        char enc[512];
        cli_url_encode(uuid, enc, sizeof(enc));
        snprintf(path, sizeof(path), "/api/v1/devices/%s", enc);
    }
    free(uuid);

    dev = fetch_get(ctx, path, NULL);
    if (!dev)
    {
        text = malloc(256);
        snprintf(text, 256, "error: device not found: %s", id_field->valuestring);
        result = cJSON_CreateObject();
        content = cJSON_CreateArray();
        item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "type", "text");
        cJSON_AddStringToObject(item, "text", text ? text : "error: failed");
        cJSON_AddItemToArray(content, item);
        cJSON_AddItemToObject(result, "content", content);
        cJSON_AddTrueToObject(result, "isError");
        free(text);
        return result;
    }

    text = cJSON_PrintUnformatted(dev);
    result = cJSON_CreateObject();
    content = cJSON_CreateArray();
    item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text ? text : "");
    cJSON_AddItemToArray(content, item);
    cJSON_AddItemToObject(result, "content", content);
    cJSON_AddFalseToObject(result, "isError");

    cJSON_Delete(dev);
    free(text);
    return result;
}

static cJSON *handle_device_history(cli_ctx_t *ctx, const cJSON *args)
{
    const cJSON *id_field = cJSON_GetObjectItemCaseSensitive(args, "id");
    char *uuid = NULL;
    char path[600];
    cJSON *hist;
    char *text;
    cJSON *result, *content, *item;

    if (!cJSON_IsString(id_field) || !id_field->valuestring)
    {
        text = strdup("error: missing required parameter: id");
        result = cJSON_CreateObject();
        content = cJSON_CreateArray();
        item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "type", "text");
        cJSON_AddStringToObject(item, "text", text ? text : "error: failed");
        cJSON_AddItemToArray(content, item);
        cJSON_AddItemToObject(result, "content", content);
        cJSON_AddTrueToObject(result, "isError");
        free(text);
        return result;
    }

    uuid = mcp_resolve_device(ctx, id_field->valuestring);
    if (!uuid)
    {
        text = malloc(256);
        snprintf(text, 256, "error: device not found: %s", id_field->valuestring);
        result = cJSON_CreateObject();
        content = cJSON_CreateArray();
        item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "type", "text");
        cJSON_AddStringToObject(item, "text", text ? text : "error: failed");
        cJSON_AddItemToArray(content, item);
        cJSON_AddItemToObject(result, "content", content);
        cJSON_AddTrueToObject(result, "isError");
        free(text);
        return result;
    }

    {
        char enc[512];
        cli_url_encode(uuid, enc, sizeof(enc));
        snprintf(path, sizeof(path), "/api/v1/devices/%s/history", enc);
    }
    free(uuid);

    hist = fetch_get(ctx, path, NULL);
    if (!hist)
    {
        text = malloc(256);
        snprintf(text, 256, "error: failed to fetch history for: %s",
            id_field->valuestring);
        result = cJSON_CreateObject();
        content = cJSON_CreateArray();
        item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "type", "text");
        cJSON_AddStringToObject(item, "text", text ? text : "error: failed");
        cJSON_AddItemToArray(content, item);
        cJSON_AddItemToObject(result, "content", content);
        cJSON_AddTrueToObject(result, "isError");
        free(text);
        return result;
    }

    text = cJSON_PrintUnformatted(hist);
    result = cJSON_CreateObject();
    content = cJSON_CreateArray();
    item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text ? text : "");
    cJSON_AddItemToArray(content, item);
    cJSON_AddItemToObject(result, "content", content);
    cJSON_AddFalseToObject(result, "isError");

    cJSON_Delete(hist);
    free(text);
    return result;
}

static cJSON *handle_status(cli_ctx_t *ctx)
{
    cJSON *status = fetch_get(ctx, "/api/v1/status", NULL);
    char *text;
    cJSON *result, *content, *item;

    if (!status)
    {
        text = strdup("error: failed to fetch status");
        result = cJSON_CreateObject();
        content = cJSON_CreateArray();
        item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "type", "text");
        cJSON_AddStringToObject(item, "text", text ? text : "error: failed");
        cJSON_AddItemToArray(content, item);
        cJSON_AddItemToObject(result, "content", content);
        cJSON_AddTrueToObject(result, "isError");
        free(text);
        return result;
    }

    text = cJSON_PrintUnformatted(status);
    result = cJSON_CreateObject();
    content = cJSON_CreateArray();
    item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text ? text : "");
    cJSON_AddItemToArray(content, item);
    cJSON_AddItemToObject(result, "content", content);
    cJSON_AddFalseToObject(result, "isError");

    cJSON_Delete(status);
    free(text);
    return result;
}

/* ── Tool definitions ───────────────────────────────────────────────── */

static cJSON *make_tool(const char *name, const char *desc, cJSON *schema)
{
    cJSON *t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "name", name);
    cJSON_AddStringToObject(t, "description", desc);
    cJSON_AddItemToObject(t, "inputSchema", schema);
    return t;
}

static cJSON *build_tools_list(void)
{
    cJSON *tools = cJSON_CreateArray();
    cJSON *schema, *props, *id_prop, *req;

    /* list_devices - no params. */
    schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "object");
    props = cJSON_CreateObject();
    cJSON_AddItemToObject(schema, "properties", props);
    cJSON_AddFalseToObject(schema, "additionalProperties");
    cJSON_AddItemToArray(tools, make_tool("list_devices",
        "List all devices known to moonflared (id, name, kind, driver, online status, and "
        "whether the device is active, i.e. counted in system totals).",
        schema));

    /* query_device - id required. */
    schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "object");
    props = cJSON_CreateObject();
    id_prop = cJSON_CreateObject();
    cJSON_AddStringToObject(id_prop, "type", "string");
    cJSON_AddStringToObject(id_prop, "description", "device uuid or exact name");
    cJSON_AddItemToObject(props, "id", id_prop);
    cJSON_AddItemToObject(schema, "properties", props);
    req = cJSON_CreateArray();
    cJSON_AddItemToArray(req, cJSON_CreateString("id"));
    cJSON_AddItemToObject(schema, "required", req);
    cJSON_AddFalseToObject(schema, "additionalProperties");
    cJSON_AddItemToArray(tools, make_tool("query_device",
        "Get the full live reading for one device by id or exact name.",
        schema));

    /* device_history - id required. */
    schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "object");
    props = cJSON_CreateObject();
    id_prop = cJSON_CreateObject();
    cJSON_AddStringToObject(id_prop, "type", "string");
    cJSON_AddStringToObject(id_prop, "description", "device uuid or exact name");
    cJSON_AddItemToObject(props, "id", id_prop);
    cJSON_AddItemToObject(schema, "properties", props);
    req = cJSON_CreateArray();
    cJSON_AddItemToArray(req, cJSON_CreateString("id"));
    cJSON_AddItemToObject(schema, "required", req);
    cJSON_AddFalseToObject(schema, "additionalProperties");
    cJSON_AddItemToArray(tools, make_tool("device_history",
        "Get the recent time-series of one module's graph column (e.g. SOC for "
        "batteries, power for chargers, temperature for weather); the reply "
        "names the column.",
        schema));

    /* status - no params. */
    schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "object");
    props = cJSON_CreateObject();
    cJSON_AddItemToObject(schema, "properties", props);
    cJSON_AddFalseToObject(schema, "additionalProperties");
    cJSON_AddItemToArray(tools, make_tool("status",
        "One-shot summary of every device grouped by kind (batteries, chargers, inverters, "
        "actuators, services; a weather service carries data.weather), plus a \"system\" object with "
        "totals over the active, online devices: charger input watts, capacity-weighted "
        "SOC, stored/capacity Wh, and battery charge/discharge watts. Devices marked "
        "\"active\": false still report readings but are left out of the totals.",
        schema));

    return tools;
}

/* ── Tool dispatch ──────────────────────────────────────────────────── */

static cJSON *dispatch_tool_call(cli_ctx_t *ctx, const cJSON *tool_name,
                                  const cJSON *arguments)
{
    const char *name;
    cJSON *args = (cJSON *)arguments;

    if (!cJSON_IsString(tool_name) || !tool_name->valuestring)
        return NULL;

    name = tool_name->valuestring;

    if (strcmp(name, "list_devices") == 0)
        return handle_list_devices(ctx);

    if (strcmp(name, "query_device") == 0)
    {
        if (!args || !cJSON_IsObject(args))
            args = cJSON_CreateObject();
        return handle_query_device(ctx, args);
    }

    if (strcmp(name, "device_history") == 0)
    {
        if (!args || !cJSON_IsObject(args))
            args = cJSON_CreateObject();
        return handle_device_history(ctx, args);
    }

    if (strcmp(name, "status") == 0)
        return handle_status(ctx);

    return NULL;
}

/* ── Method handlers ────────────────────────────────────────────────── */

static cJSON *handle_initialize(const cJSON *params)
{
    cJSON *result = cJSON_CreateObject();
    const cJSON *pv = cJSON_GetObjectItemCaseSensitive(params, "protocolVersion");

    if (cJSON_IsString(pv) && pv->valuestring)
        cJSON_AddStringToObject(result, "protocolVersion", pv->valuestring);
    else
        cJSON_AddStringToObject(result, "protocolVersion", "2024-11-05");

    {
        cJSON *caps = cJSON_CreateObject();
        /* Advertise the tools capability so clients know to call tools/list. */
        cJSON_AddItemToObject(caps, "tools", cJSON_CreateObject());
        cJSON_AddItemToObject(result, "capabilities", caps);
    }

    {
        cJSON *si = cJSON_CreateObject();
        cJSON_AddStringToObject(si, "name", "moonflare-cli");
        cJSON_AddStringToObject(si, "version", MF_VERSION);
        cJSON_AddItemToObject(result, "serverInfo", si);
    }

    return result;
}

static cJSON *handle_ping(void)
{
    return cJSON_CreateObject();
}

/* ── Main loop ──────────────────────────────────────────────────────── */

int mcp_run(cli_ctx_t *ctx)
{
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;

    fprintf(stderr, "mcp: starting on stdio\n");

    while ((len = getline(&line, &cap, stdin)) > 0)
    {
        cJSON *req;
        const cJSON *method, *id_field, *params;
        const char *m;
        int is_notification;

        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0)
            continue;

        req = cJSON_Parse(line);
        if (!req)
        {
            fprintf(stderr, "mcp: parse error from: %.*s\n", (int)len, line);
            send_env(make_error(NULL, -32700, "Parse error"));
            continue;
        }

        method   = cJSON_GetObjectItemCaseSensitive(req, "method");
        id_field = cJSON_GetObjectItemCaseSensitive(req, "id");
        params   = cJSON_GetObjectItemCaseSensitive(req, "params");
        /* A JSON-RPC request with no "id" is a notification: never reply. */
        is_notification = (id_field == NULL);

        if (!cJSON_IsString(method) || !method->valuestring)
        {
            fprintf(stderr, "mcp: invalid request (no method)\n");
            if (!is_notification)
                send_env(make_error(id_field, -32600, "Invalid request"));
            cJSON_Delete(req);
            continue;
        }
        m = method->valuestring;

        if (strcmp(m, "initialize") == 0)
        {
            cJSON *result = handle_initialize(
                params && cJSON_IsObject(params) ? params : NULL);
            if (!is_notification)
                send_env(make_resp(id_field, result));
            else
                cJSON_Delete(result);
        }
        else if (strcmp(m, "notifications/initialized") == 0)
        {
            fprintf(stderr, "mcp: initialized notification\n");
        }
        else if (strcmp(m, "ping") == 0)
        {
            cJSON *result = handle_ping();
            if (!is_notification)
                send_env(make_resp(id_field, result));
            else
                cJSON_Delete(result);
        }
        else if (strcmp(m, "tools/list") == 0)
        {
            cJSON *result = cJSON_CreateObject();
            cJSON_AddItemToObject(result, "tools", build_tools_list());
            if (!is_notification)
                send_env(make_resp(id_field, result));
            else
                cJSON_Delete(result);
        }
        else if (strcmp(m, "tools/call") == 0)
        {
            const cJSON *tool_name, *tool_args;
            cJSON *tool_result;

            if (!params || !cJSON_IsObject(params))
            {
                if (!is_notification)
                    send_env(make_error(id_field, -32602, "Invalid params"));
                cJSON_Delete(req);
                continue;
            }
            tool_name = cJSON_GetObjectItemCaseSensitive(params, "name");
            tool_args = cJSON_GetObjectItemCaseSensitive(params, "arguments");
            if (!cJSON_IsString(tool_name) || !tool_name->valuestring)
            {
                if (!is_notification)
                    send_env(make_error(id_field, -32602, "Invalid params"));
                cJSON_Delete(req);
                continue;
            }

            /* dispatch_tool_call returns a ready {content, isError} object,
             * which is exactly the tools/call result shape. */
            tool_result = dispatch_tool_call(ctx, tool_name, tool_args);
            if (!tool_result)
            {
                char msg[256];
                snprintf(msg, sizeof(msg), "unknown tool: %s",
                         tool_name->valuestring);
                if (!is_notification)
                    send_env(make_error(id_field, -32602, msg));
            }
            else if (!is_notification)
            {
                send_env(make_resp(id_field, tool_result));
            }
            else
            {
                cJSON_Delete(tool_result);
            }
        }
        else
        {
            char msg[256];
            snprintf(msg, sizeof(msg), "Method not found: %s", m);
            if (!is_notification)
                send_env(make_error(id_field, -32601, msg));
        }

        cJSON_Delete(req);
    }

    free(line);
    fprintf(stderr, "mcp: stdin closed, exiting\n");
    return 0;
}
