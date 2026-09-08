#define _POSIX_C_SOURCE 200809L

#include "device.h"
#include "rest.h"
#include <cJSON.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void mf_log(int prio, const char *fmt, ...)
{
    (void)prio;
    (void)fmt;
}

static int g_fail = 0;

static void check(int cond, const char *desc)
{
    if (cond) {
        printf("  PASS: %s\n", desc);
    } else {
        fprintf(stderr, "  FAIL: %s\n", desc);
        g_fail++;
    }
}

static void test_status(void)
{
    printf("1. GET /api/v1/status\n");

    mf_rest_request_t req;
    memset(&req, 0, sizeof(req));
    req.method = "GET";
    req.path   = "/api/v1/status";
    req.body   = NULL;
    req.body_len = 0;

    mf_rest_response_t resp;
    memset(&resp, 0, sizeof(resp));
    int rc = mf_rest_dispatch(&req, &resp);

    check(rc == 0, "dispatch returned 0");
    check(resp.status == 200, "status 200");

    /* Check server header */
    cJSON *root = cJSON_Parse(resp.body);
    check(root != NULL, "response parses as JSON");
    if (root) {
        cJSON *server = cJSON_GetObjectItem(root, "server");
        check(server != NULL && strcmp(server->valuestring, "moonflared/0.1.0") == 0,
              "server is moonflared/0.1.0");

        cJSON *inverters = cJSON_GetObjectItem(root, "inverters");
        check(inverters != NULL && cJSON_IsArray(inverters) &&
              cJSON_GetArraySize(inverters) == 0,
              "inverters is empty array");

        cJSON *phantoms = cJSON_GetObjectItem(root, "phantoms");
        check(phantoms != NULL && cJSON_IsArray(phantoms) &&
              cJSON_GetArraySize(phantoms) == 0,
              "phantoms is empty array");

        cJSON *batteries = cJSON_GetObjectItem(root, "batteries");
        check(batteries != NULL && cJSON_IsArray(batteries),
              "batteries array present");

        cJSON *chargers = cJSON_GetObjectItem(root, "chargers");
        check(chargers != NULL && cJSON_IsArray(chargers),
              "chargers array present");

        cJSON_Delete(root);
    }
}

static void test_drivers(void)
{
    printf("2. GET /api/v1/drivers\n");

    mf_rest_request_t req;
    memset(&req, 0, sizeof(req));
    req.method = "GET";
    req.path   = "/api/v1/drivers";
    req.body   = NULL;
    req.body_len = 0;

    mf_rest_response_t resp;
    memset(&resp, 0, sizeof(resp));
    int rc = mf_rest_dispatch(&req, &resp);

    check(rc == 0, "dispatch returned 0");
    check(resp.status == 200, "status 200");

    cJSON *root = cJSON_Parse(resp.body);
    check(root != NULL, "response parses as JSON");
    if (root) {
        cJSON *drivers = cJSON_GetObjectItem(root, "drivers");
        check(drivers != NULL && cJSON_IsArray(drivers), "drivers array present");
        if (drivers) {
            int sz = cJSON_GetArraySize(drivers);
            check(sz == 2, "2 drivers");

            /* Find battery/demo */
            bool found_battery = false, found_charger = false;
            for (int i = 0; i < sz; i++) {
                cJSON *d = cJSON_GetArrayItem(drivers, i);
                cJSON *kind = cJSON_GetObjectItem(d, "kind");
                cJSON *driver = cJSON_GetObjectItem(d, "driver");
                if (kind && driver) {
                    if (strcmp(kind->valuestring, "battery") == 0 &&
                        strcmp(driver->valuestring, "demo") == 0)
                        found_battery = true;
                    if (strcmp(kind->valuestring, "charger") == 0 &&
                        strcmp(driver->valuestring, "demo") == 0)
                        found_charger = true;
                }
            }
            check(found_battery, "kind=battery driver=demo present");
            check(found_charger, "kind=charger driver=demo present");
        }
        cJSON_Delete(root);
    }
}

static void test_404(void)
{
    printf("3. GET /api/v1/nope\n");

    mf_rest_request_t req;
    memset(&req, 0, sizeof(req));
    req.method = "GET";
    req.path   = "/api/v1/nope";
    req.body   = NULL;
    req.body_len = 0;

    mf_rest_response_t resp;
    memset(&resp, 0, sizeof(resp));
    int rc = mf_rest_dispatch(&req, &resp);

    check(rc == 0, "dispatch returned 0");
    check(resp.status == 404, "status 404");

    cJSON *root = cJSON_Parse(resp.body);
    check(root != NULL, "response parses as JSON");
    if (root) {
        cJSON *err = cJSON_GetObjectItem(root, "error");
        check(err != NULL && err->type == cJSON_String, "error field present");
        cJSON_Delete(root);
    }
}

static void test_405(void)
{
    printf("4. POST /api/v1/status\n");

    mf_rest_request_t req;
    memset(&req, 0, sizeof(req));
    req.method = "POST";
    req.path   = "/api/v1/status";
    req.body   = NULL;
    req.body_len = 0;

    mf_rest_response_t resp;
    memset(&resp, 0, sizeof(resp));
    int rc = mf_rest_dispatch(&req, &resp);

    check(rc == 0, "dispatch returned 0");
    check(resp.status == 405, "status 405");

    cJSON *root = cJSON_Parse(resp.body);
    check(root != NULL, "response parses as JSON");
    if (root) {
        cJSON *err = cJSON_GetObjectItem(root, "error");
        check(err != NULL && err->type == cJSON_String, "error field present");
        cJSON_Delete(root);
    }
}

static void test_create_device(void)
{
    printf("5. POST /api/v1/devices\n");

    const char *json_body =
        "{\"name\":\"test-battery\",\"kind\":\"battery\",\"driver\":\"demo\"}";

    mf_rest_request_t req;
    memset(&req, 0, sizeof(req));
    req.method  = "POST";
    req.path    = "/api/v1/devices";
    req.body    = json_body;
    req.body_len = strlen(json_body);

    mf_rest_response_t resp;
    memset(&resp, 0, sizeof(resp));
    int rc = mf_rest_dispatch(&req, &resp);

    check(rc == 0, "dispatch returned 0");
    check(resp.status == 201, "status 201");

    /* Check Location header */
    check(strlen(resp.location) > 0, "Location header set");
    if (strlen(resp.location) > 0) {
        check(strncmp(resp.location, "/api/v1/devices/", 16) == 0,
              "Location starts with /api/v1/devices/");
    }

    /* Check uuid in Location (36 char UUID at end) */
    if (strlen(resp.location) > 0) {
        const char *uuid_part = strrchr(resp.location, '/');
        if (uuid_part) {
            uuid_part++; /* skip '/' */
            check(strlen(uuid_part) == 36, "uuid is 36-char");
        }
    }

    /* Parse response body */
    cJSON *root = cJSON_Parse(resp.body);
    check(root != NULL, "response parses as JSON");
    if (root) {
        cJSON *id = cJSON_GetObjectItem(root, "id");
        check(id != NULL && id->type == cJSON_String, "id field present in body");

        cJSON *name = cJSON_GetObjectItem(root, "name");
        check(name != NULL && strcmp(name->valuestring, "test-battery") == 0,
              "name is test-battery");

        cJSON *kind = cJSON_GetObjectItem(root, "kind");
        check(kind != NULL && strcmp(kind->valuestring, "battery") == 0,
              "kind is battery");

        cJSON *driver = cJSON_GetObjectItem(root, "driver");
        check(driver != NULL && strcmp(driver->valuestring, "demo") == 0,
              "driver is demo");

        /* Save uuid for test 6 */
        char saved_uuid[37];
        if (id && strlen(id->valuestring) == 36) {
            strncpy(saved_uuid, id->valuestring, 36);
            saved_uuid[36] = '\0';

            printf("6. GET /api/v1/devices/%s\n", saved_uuid);
            /* Test 6: GET device by id */
            mf_rest_request_t req2;
            memset(&req2, 0, sizeof(req2));
            req2.method  = "GET";
            req2.path    = "/api/v1/devices";
            req2.body    = NULL;
            req2.body_len = 0;

            mf_rest_response_t resp2;
            memset(&resp2, 0, sizeof(resp2));

            /* Build path for the device */
            char path[256];
            snprintf(path, sizeof(path), "/api/v1/devices/%s", saved_uuid);
            req2.path = path;

            rc = mf_rest_dispatch(&req2, &resp2);
            check(rc == 0, "dispatch returned 0");
            check(resp2.status == 200, "status 200");

            cJSON *root2 = cJSON_Parse(resp2.body);
            check(root2 != NULL, "device response parses as JSON");
            if (root2) {
                cJSON *id2 = cJSON_GetObjectItem(root2, "id");
                check(id2 != NULL && strcmp(id2->valuestring, saved_uuid) == 0,
                      "id matches");
                cJSON_Delete(root2);
            }
        }

        cJSON_Delete(root);
    }
}

static void test_malformed_json(void)
{
    printf("7. POST /api/v1/devices malformed JSON\n");

    const char *bad_json = "not valid json {{{";

    mf_rest_request_t req;
    memset(&req, 0, sizeof(req));
    req.method  = "POST";
    req.path    = "/api/v1/devices";
    req.body    = bad_json;
    req.body_len = strlen(bad_json);

    mf_rest_response_t resp;
    memset(&resp, 0, sizeof(resp));
    int rc = mf_rest_dispatch(&req, &resp);

    check(rc == 0, "dispatch returned 0");
    check(resp.status == 400, "status 400");

    cJSON *root = cJSON_Parse(resp.body);
    check(root != NULL, "response parses as JSON");
    if (root) {
        cJSON *err = cJSON_GetObjectItem(root, "error");
        check(err != NULL && err->type == cJSON_String, "error field present");
        cJSON_Delete(root);
    }
}

static void test_delete(void)
{
    printf("8. DELETE three-state (sync slots)\n");

    const char *json_body =
        "{\"name\":\"to-remove\",\"kind\":\"battery\",\"driver\":\"demo\"}";
    mf_rest_request_t req;
    mf_rest_response_t resp;
    char path[128];
    const char *id;

    memset(&req, 0, sizeof(req));
    req.method = "POST";
    req.path = "/api/v1/devices";
    req.body = json_body;
    req.body_len = strlen(json_body);
    memset(&resp, 0, sizeof(resp));
    check(mf_rest_dispatch(&req, &resp) == 0 && resp.status == 201, "POST for delete");
    id = strrchr(resp.location, '/');
    check(id && id[1], "location id");
    if (!id)
        return;
    id++;
    snprintf(path, sizeof(path), "/api/v1/devices/%s", id);

    memset(&req, 0, sizeof(req));
    req.method = "DELETE";
    req.path = path;
    memset(&resp, 0, sizeof(resp));
    check(mf_rest_dispatch(&req, &resp) == 0, "DELETE dispatch");
    check(resp.status == 202, "DELETE live → 202");

    memset(&resp, 0, sizeof(resp));
    check(mf_rest_dispatch(&req, &resp) == 0, "DELETE again");
    check(resp.status == 404, "DELETE after free → 404");

    memset(&req, 0, sizeof(req));
    req.method = "DELETE";
    req.path = "/api/v1/devices/00000000-0000-4000-8000-000000000000";
    memset(&resp, 0, sizeof(resp));
    check(mf_rest_dispatch(&req, &resp) == 0 && resp.status == 404,
          "DELETE unknown → 404");
}

static void test_pr10(void)
{
    const char *body =
        "{\"name\":\"pr10-batt\",\"kind\":\"battery\",\"driver\":\"demo\"}";
    mf_rest_request_t req;
    mf_rest_response_t resp;
    char path[192];
    char setpath[192];
    char actpath[192];
    const char *id;
    char put[64];
    uint64_t gen = 0;

    printf("9. PR-10 settings/actions/config/discover\n");

    memset(&req, 0, sizeof(req));
    req.method = "POST";
    req.path = "/api/v1/devices";
    req.body = body;
    req.body_len = strlen(body);
    memset(&resp, 0, sizeof(resp));
    check(mf_rest_dispatch(&req, &resp) == 0 && resp.status == 201, "POST pr10 device");
    id = strrchr(resp.location, '/');
    check(id && id[1], "pr10 id");
    if (!id)
        return;
    id++;
    snprintf(path, sizeof(path), "/api/v1/devices/%s", id);
    snprintf(setpath, sizeof(setpath), "/api/v1/devices/%s/settings", id);
    snprintf(actpath, sizeof(actpath), "/api/v1/devices/%s/actions/set_switch", id);

    memset(&req, 0, sizeof(req));
    req.method = "PUT";
    req.path = setpath;
    req.body = "{\"poll_interval_s\":0.1}";
    req.body_len = strlen(req.body);
    memset(&resp, 0, sizeof(resp));
    check(mf_rest_dispatch(&req, &resp) == 0 && resp.status == 400,
          "PUT poll below clamp → 400");

    req.body = "{\"poll_interval_s\":2.0}";
    req.body_len = strlen(req.body);
    memset(&resp, 0, sizeof(resp));
    check(mf_rest_dispatch(&req, &resp) == 0 && resp.status == 200,
          "PUT poll 2.0 → 200");
    check(strstr(resp.body, "\"poll_interval_s\"") != NULL, "settings has poll");

    memset(&req, 0, sizeof(req));
    req.method = "GET";
    req.path = setpath;
    memset(&resp, 0, sizeof(resp));
    check(mf_rest_dispatch(&req, &resp) == 0 && resp.status == 200, "GET settings");

    memset(&req, 0, sizeof(req));
    req.method = "POST";
    req.path = actpath;
    req.body = "{\"key\":\"charge\",\"value\":true}";
    req.body_len = strlen(req.body);
    memset(&resp, 0, sizeof(resp));
    check(mf_rest_dispatch(&req, &resp) == 0 && resp.status == 400,
          "action without plugin ctx → 400");

    memset(&req, 0, sizeof(req));
    req.method = "GET";
    req.path = "/api/v1/config";
    memset(&resp, 0, sizeof(resp));
    check(mf_rest_dispatch(&req, &resp) == 0 && resp.status == 200, "GET config");
    check(strstr(resp.body, "\"config_gen\"") != NULL, "config_gen present");
    check(resp.etag[0] == '"', "ETag quoted");
    {
        cJSON *root = cJSON_Parse(resp.body);
        if (root) {
            cJSON *g = cJSON_GetObjectItem(root, "config_gen");
            if (g && cJSON_IsNumber(g))
                gen = (uint64_t)g->valuedouble;
            cJSON_Delete(root);
        }
    }

    memset(&req, 0, sizeof(req));
    req.method = "PUT";
    req.path = "/api/v1/config";
    req.if_match = "\"999\"";
    req.body = "{\"listen\":\"127.0.0.1:5250\"}";
    req.body_len = strlen(req.body);
    memset(&resp, 0, sizeof(resp));
    check(mf_rest_dispatch(&req, &resp) == 0 && resp.status == 409,
          "PUT config stale If-Match → 409");

    snprintf(put, sizeof(put), "{\"listen\":\"127.0.0.1:5250\",\"config_gen\":%llu}",
             (unsigned long long)gen);
    req.if_match = NULL;
    req.body = put;
    req.body_len = strlen(put);
    memset(&resp, 0, sizeof(resp));
    check(mf_rest_dispatch(&req, &resp) == 0 && resp.status == 200,
          "PUT config matching gen → 200");

    memset(&req, 0, sizeof(req));
    req.method = "POST";
    req.path = "/api/v1/discover";
    req.body = "{\"kind\":\"charger\",\"bus\":\"modbus\"}";
    req.body_len = strlen(req.body);
    memset(&resp, 0, sizeof(resp));
    check(mf_rest_dispatch(&req, &resp) == 0 && resp.status == 202,
          "POST discover → 202");
    check(strstr(resp.body, "started") != NULL, "started");

    memset(&req, 0, sizeof(req));
    req.method = "GET";
    req.path = "/api/v1/discover";
    memset(&resp, 0, sizeof(resp));
    check(mf_rest_dispatch(&req, &resp) == 0 && resp.status == 200, "GET discover");
    check(strstr(resp.body, "\"status\":\"running\"") != NULL, "discover running");

    memset(&req, 0, sizeof(req));
    req.method = "POST";
    req.path = "/api/v1/discover";
    req.body = "{\"kind\":\"charger\",\"bus\":\"modbus\"}";
    req.body_len = strlen(req.body);
    memset(&resp, 0, sizeof(resp));
    check(mf_rest_dispatch(&req, &resp) == 0 && resp.status == 409,
          "second POST discover → 409");

    memset(&req, 0, sizeof(req));
    req.method = "GET";
    req.path = "/api/v1/drivers";
    memset(&resp, 0, sizeof(resp));
    check(mf_rest_dispatch(&req, &resp) == 0 && resp.status == 200,
          "GET /drivers still 200");
}

int main(void)
{
    srand((unsigned)time(NULL));

    mf_devices_init(NULL, NULL, NULL, NULL);
    mf_rest_init();

    printf("=== route tests ===\n");

    test_status();
    test_drivers();
    test_404();
    test_405();
    test_create_device();
    test_malformed_json();
    test_delete();
    test_pr10();

    printf("\n");
    if (g_fail > 0) {
        printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("ok\n");
    return 0;
}
