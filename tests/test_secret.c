/*
 * Secret, action and read-only plugin fields (src/rest/secret.c): masks in
 * settings and in the config, masks sent back, and a config round trip
 * that must not lose a key.
 */

#include "secret.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); g_fail++; } \
} while (0)

static const char *FIELDS =
    "[{\"key\":\"x.key\",\"type\":\"secret\"},"
    "{\"key\":\"x.to\",\"type\":\"string\"},"
    "{\"key\":\"x.test\",\"type\":\"action\",\"action\":\"test\"},"
    "{\"key\":\"x.credits\",\"type\":\"number\",\"readonly\":true}]";

static const char *str(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);

    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static void test_fields(const cJSON *f)
{
    char out[32];

    CHECK(mf_field_is_secret(f, "x.key") && !mf_field_is_secret(f, "x.to"),
          "secret field");
    CHECK(mf_field_not_setting(f, "x.test") && mf_field_not_setting(f, "x.credits") &&
          !mf_field_not_setting(f, "x.key") && !mf_field_not_setting(f, "name"),
          "actions and read-only values are not settings");
    mf_secret_mask("", out, sizeof(out));
    CHECK(out[0] == '\0', "empty stays empty");
    mf_secret_mask("short", out, sizeof(out));
    CHECK(strcmp(out, "********") == 0, "a short secret shows nothing");
    mf_secret_mask("abcdefghijkl1234", out, sizeof(out));
    CHECK(strcmp(out, "********1234") == 0, "a long one shows its last four");
    mf_secret_mask("********1234", out, sizeof(out));
    CHECK(strcmp(out, "********1234") == 0, "a mask stays as it is");
    CHECK(mf_secret_is_mask("********") && !mf_secret_is_mask("*******x"),
          "mask detection");
}

static void test_settings(const cJSON *f)
{
    cJSON *s = cJSON_Parse("{\"name\":\"SMS\",\"x.key\":\"abcdefghijkl1234\","
                           "\"x.to\":\"5558838530\"}");
    cJSON *b;

    mf_secret_mask_settings(s, f);
    CHECK(strcmp(str(s, "x.key"), "********1234") == 0 &&
          strcmp(str(s, "x.to"), "5558838530") == 0 && strcmp(str(s, "name"), "SMS") == 0,
          "settings: the secret is masked, nothing else");
    cJSON_Delete(s);

    b = cJSON_Parse("{\"name\":\"SMS\",\"x.key\":\"********1234\",\"x.to\":\"5\","
                    "\"x.test\":\"\",\"x.credits\":3}");
    mf_secret_clean_body(b, f);
    CHECK(!cJSON_GetObjectItemCaseSensitive(b, "x.key") &&
          !cJSON_GetObjectItemCaseSensitive(b, "x.test") &&
          !cJSON_GetObjectItemCaseSensitive(b, "x.credits") &&
          str(b, "x.to") && str(b, "name"),
          "PUT: a mask sent back, a button and a shown value are dropped");
    cJSON_Delete(b);
    b = cJSON_Parse("{\"x.key\":\"newkey987654321\"}");
    mf_secret_clean_body(b, f);
    CHECK(str(b, "x.key") && strcmp(str(b, "x.key"), "newkey987654321") == 0,
          "PUT: a new secret goes through");
    cJSON_Delete(b);
}

static void test_config(const cJSON *f)
{
    cJSON *m = cJSON_Parse("{\"kind\":\"service\",\"driver\":\"x\","
                           "\"x\":{\"key\":\"abcdefghijkl1234\",\"to\":\"5\"}}");
    char out[512];

    mf_secret_mask_module(m, f);
    CHECK(strcmp(str(cJSON_GetObjectItemCaseSensitive(m, "x"), "key"),
                 "********1234") == 0 &&
          strcmp(str(cJSON_GetObjectItemCaseSensitive(m, "x"), "to"), "5") == 0,
          "config: the block's secret is masked");
    cJSON_Delete(m);

    CHECK(mf_secret_restore_extra("{\"x\":{\"key\":\"********3456\",\"to\":\"6\"}}",
                                  "{\"x\":{\"key\":\"realkey123456\",\"to\":\"5\"}}",
                                  f, out, sizeof(out)) == 0 &&
          strstr(out, "\"key\":\"realkey123456\"") && strstr(out, "\"to\":\"6\""),
          "round trip: a mask keeps the live key, the rest is new");
    CHECK(mf_secret_restore_extra("{\"x\":{\"key\":null}}",
                                  "{\"x\":{\"key\":\"realkey123456\"}}",
                                  f, out, sizeof(out)) == 0 &&
          strstr(out, "realkey123456"), "null keeps the live key");
    CHECK(mf_secret_restore_extra("", "{\"x\":{\"key\":\"realkey123456\"}}",
                                  f, out, sizeof(out)) == 0 &&
          strstr(out, "realkey123456"), "a missing block keeps the live key");
    CHECK(mf_secret_restore_extra("{\"x\":{\"key\":\"brandnew12345\"}}",
                                  "{\"x\":{\"key\":\"realkey123456\"}}",
                                  f, out, sizeof(out)) == 0 &&
          strstr(out, "brandnew12345") && !strstr(out, "realkey"),
          "a new key wins");
    CHECK(mf_secret_restore_extra("{\"x\":{\"to\":\"6\"}}", "{\"x\":{\"to\":\"5\"}}",
                                  f, out, sizeof(out)) == 0 &&
          strcmp(out, "{\"x\":{\"to\":\"6\"}}") == 0, "no live key: unchanged");
    CHECK(mf_secret_restore_extra("{not json", "{}", f, out, sizeof(out)) == -1,
          "bad JSON is left alone");
}

int main(void)
{
    cJSON *f = cJSON_Parse(FIELDS);

    test_fields(f);
    test_settings(f);
    test_config(f);
    cJSON_Delete(f);
    if (g_fail)
    {
        fprintf(stderr, "%d failure(s)\n", g_fail);
        return 1;
    }
    printf("test_secret: ok\n");
    return 0;
}
