/*
 * Secret, action and read-only plugin fields.  See secret.h.
 */

#include "secret.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const cJSON *field(const cJSON *fields, const char *key)
{
    const cJSON *f;

    cJSON_ArrayForEach(f, fields)
    {
        const cJSON *k = cJSON_GetObjectItemCaseSensitive(f, "key");

        if (cJSON_IsString(k) && key && strcmp(k->valuestring, key) == 0)
            return f;
    }
    return NULL;
}

static int type_is(const cJSON *f, const char *type)
{
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(f, "type");

    return cJSON_IsString(t) && strcmp(t->valuestring, type) == 0;
}

int mf_field_is_secret(const cJSON *fields, const char *key)
{
    return type_is(field(fields, key), "secret");
}

int mf_field_not_setting(const cJSON *fields, const char *key)
{
    const cJSON *f = field(fields, key);

    return type_is(f, "action") ||
           cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(f, "readonly"));
}

void mf_secret_mask(const char *value, char *out, size_t cap)
{
    size_t n = value ? strlen(value) : 0;

    if (!out || cap == 0)
        return;
    if (!n)
        out[0] = '\0';
    else if (mf_secret_is_mask(value))
        snprintf(out, cap, "%s", value);
    else if (n >= 12)
        snprintf(out, cap, "%s%s", MF_SECRET_MASK, value + n - 4);
    else
        snprintf(out, cap, "%s", MF_SECRET_MASK);
}

int mf_secret_is_mask(const char *value)
{
    return value && strncmp(value, MF_SECRET_MASK, strlen(MF_SECRET_MASK)) == 0;
}

/* In place: the item keeps its key and its place. */
static void mask_item(cJSON *it)
{
    char out[32];

    if (!cJSON_IsString(it))
        return;
    mf_secret_mask(it->valuestring, out, sizeof(out));
    cJSON_SetValuestring(it, out);
}

void mf_secret_mask_settings(cJSON *settings, const cJSON *fields)
{
    cJSON *it;

    cJSON_ArrayForEach(it, settings)
        if (it->string && mf_field_is_secret(fields, it->string))
            mask_item(it);
}

void mf_secret_clean_body(cJSON *body, const cJSON *fields)
{
    cJSON *it = body ? body->child : NULL;

    while (it)
    {
        cJSON *next = it->next;

        if (it->string &&
            (mf_field_not_setting(fields, it->string) ||
             (mf_field_is_secret(fields, it->string) &&
              cJSON_IsString(it) && mf_secret_is_mask(it->valuestring))))
            cJSON_DeleteItemFromObjectCaseSensitive(body, it->string);
        it = next;
    }
}

/* "ns.leaf" -> ns and leaf; 0 when key is not dotted. */
static int split(const char *key, char *ns, size_t nscap, const char **leaf)
{
    const char *dot = key ? strchr(key, '.') : NULL;
    size_t n;

    if (!dot || dot == key || !dot[1])
        return 0;
    n = (size_t)(dot - key);
    if (n >= nscap)
        return 0;
    memcpy(ns, key, n);
    ns[n] = '\0';
    *leaf = dot + 1;
    return 1;
}

void mf_secret_mask_module(cJSON *module, const cJSON *fields)
{
    const cJSON *f;

    cJSON_ArrayForEach(f, fields)
    {
        const cJSON *k = cJSON_GetObjectItemCaseSensitive(f, "key");
        char ns[32];
        const char *leaf;
        cJSON *block;

        if (!type_is(f, "secret") || !cJSON_IsString(k) ||
            !split(k->valuestring, ns, sizeof(ns), &leaf))
            continue;
        block = cJSON_GetObjectItemCaseSensitive(module, ns);
        mask_item(cJSON_GetObjectItemCaseSensitive(block, leaf));
    }
}

int mf_secret_restore_extra(const char *next_extra, const char *live_extra,
                            const cJSON *fields, char *out, size_t cap)
{
    cJSON *next = cJSON_Parse(next_extra && next_extra[0] ? next_extra : "{}");
    cJSON *live = cJSON_Parse(live_extra && live_extra[0] ? live_extra : "{}");
    const cJSON *f;
    char *s = NULL;
    int rc = -1;

    if (!cJSON_IsObject(next) || !cJSON_IsObject(live))
        goto done;
    cJSON_ArrayForEach(f, fields)
    {
        const cJSON *k = cJSON_GetObjectItemCaseSensitive(f, "key");
        char ns[32];
        const char *leaf;
        cJSON *nb, *nv;
        const cJSON *lv;

        if (!type_is(f, "secret") || !cJSON_IsString(k) ||
            !split(k->valuestring, ns, sizeof(ns), &leaf))
            continue;
        lv = cJSON_GetObjectItemCaseSensitive(
            cJSON_GetObjectItemCaseSensitive(live, ns), leaf);
        if (!cJSON_IsString(lv) || !lv->valuestring[0])
            continue;
        nb = cJSON_GetObjectItemCaseSensitive(next, ns);
        if (!nb)
            nb = cJSON_AddObjectToObject(next, ns);
        if (!cJSON_IsObject(nb))
            continue;
        nv = cJSON_GetObjectItemCaseSensitive(nb, leaf);
        if (nv && !cJSON_IsNull(nv) &&
            !(cJSON_IsString(nv) && mf_secret_is_mask(nv->valuestring)))
            continue;                   /* a new value: keep it */
        if (nv)
            cJSON_DeleteItemFromObjectCaseSensitive(nb, leaf);
        cJSON_AddStringToObject(nb, leaf, lv->valuestring);
    }
    s = cJSON_PrintUnformatted(next);
    if (s && strlen(s) < cap)
    {
        snprintf(out, cap, "%s", s);
        rc = 0;
    }
done:
    free(s);
    cJSON_Delete(next);
    cJSON_Delete(live);
    return rc;
}
