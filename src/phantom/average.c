#include "average.h"

#include <math.h>
#include <stdlib.h>

#define MAX_READINGS 32
#define TYPE(n) ((n)->type & 0xFF)

static cJSON *avg_node(const cJSON *const *nodes, int n)
{
    const cJSON *first = nodes[0];
    const cJSON *sub[MAX_READINGS];
    int i, k;

    /* A value one unit omits (null) must not hide the others' numbers. */
    for (i = 0; i < n && cJSON_IsNull(first); i++)
        first = nodes[i];

    if (cJSON_IsNumber(first))
    {
        double sum = 0.0;
        int cnt = 0;

        for (i = 0; i < n; i++)
            if (cJSON_IsNumber(nodes[i]))
            {
                sum += nodes[i]->valuedouble;
                cnt++;
            }
        return cJSON_CreateNumber(round(sum / cnt * 1e4) / 1e4);
    }
    if (cJSON_IsObject(first))
    {
        cJSON *out = cJSON_CreateObject();
        const cJSON *it;

        cJSON_ArrayForEach(it, first)
        {
            int m = 0;

            if (!it->string)
                continue;
            for (i = 0; i < n; i++)
            {
                const cJSON *c = cJSON_GetObjectItemCaseSensitive(nodes[i],
                                                                  it->string);

                if (c && (m == 0 || TYPE(c) == TYPE(it) || cJSON_IsNull(sub[0])))
                    sub[m++] = c;
            }
            cJSON_AddItemToObject(out, it->string, avg_node(sub, m));
        }
        return out;
    }
    if (cJSON_IsArray(first))
    {
        cJSON *out = cJSON_CreateArray();
        int len = cJSON_GetArraySize(first);

        for (k = 0; k < len; k++)
        {
            int m = 0;

            for (i = 0; i < n; i++)
            {
                const cJSON *c = cJSON_IsArray(nodes[i])
                                 ? cJSON_GetArrayItem(nodes[i], k) : NULL;

                if (c && (m == 0 || TYPE(c) == TYPE(cJSON_GetArrayItem(first, k)) ||
                          cJSON_IsNull(sub[0])))
                    sub[m++] = c;
            }
            cJSON_AddItemToArray(out, avg_node(sub, m));
        }
        return out;
    }
    return cJSON_Duplicate(first, 1);   /* string, bool, null */
}

cJSON *mf_phantom_average(const cJSON *const *readings, int n)
{
    if (!readings || n < 1)
        return NULL;
    if (n > MAX_READINGS)
        n = MAX_READINGS;
    return avg_node(readings, n);
}
