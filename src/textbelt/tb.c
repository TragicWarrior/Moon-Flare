/*
 * Textbelt SMS: phone numbers, form bodies, replies and message text.
 * See tb.h.
 */

#include "tb.h"

#include <cJSON.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int tb_phone_norm(const char *in, char *out, size_t cap)
{
    char digits[32];
    size_t n = 0;
    int plus = 0;
    const char *p;

    if (!in || !out || cap < TB_PHONE_MAX)
        return -1;
    for (p = in; *p && isspace((unsigned char)*p); p++)
        ;
    if (*p == '+')
    {
        plus = 1;
        p++;
    }
    for (; *p; p++)
    {
        if (isdigit((unsigned char)*p))
        {
            if (n + 1 >= sizeof(digits))
                return -1;
            digits[n++] = *p;
        }
        else if (!strchr(" -.()", *p))
            return -1;
    }
    digits[n] = '\0';
    if (plus)
    {
        if (n < 8 || n > 15 || digits[0] == '0')
            return -1;
        snprintf(out, cap, "+%s", digits);
        return 0;
    }
    if (n == 11 && digits[0] == '1')
    {
        snprintf(out, cap, "+%s", digits);
        return 0;
    }
    if (n != 10)
        return -1;
    snprintf(out, cap, "%s", digits);
    return 0;
}

void tb_phone_mask(const char *phone, char *out, size_t cap)
{
    size_t n;

    if (!out || cap == 0)
        return;
    n = phone ? strlen(phone) : 0;
    if (n < 4)
    {
        snprintf(out, cap, "***");
        return;
    }
    snprintf(out, cap, "***%s", phone + n - 4);
}

int tb_recipients(const char *list, char (*out)[TB_PHONE_MAX], int max,
                  char *bad, size_t badcap)
{
    const char *p = list;
    int n = 0;

    if (bad && badcap)
        bad[0] = '\0';
    while (p && *p)
    {
        const char *end = p + strcspn(p, ",;");
        char item[64];
        size_t len = (size_t)(end - p);
        char *s = item, *e;

        if (len >= sizeof(item))
            len = sizeof(item) - 1;
        memcpy(item, p, len);
        item[len] = '\0';
        while (isspace((unsigned char)*s))
            s++;
        e = s + strlen(s);
        while (e > s && isspace((unsigned char)e[-1]))
            *--e = '\0';
        if (*s)
        {
            if (n >= max || tb_phone_norm(s, out[n], TB_PHONE_MAX) != 0)
            {
                if (bad && badcap)
                    snprintf(bad, badcap, "%s", n >= max ? "too many numbers" : s);
                return -1;
            }
            n++;
        }
        p = *end ? end + 1 : end;
    }
    return n;
}

char *tb_escape(const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = s ? strlen(s) : 0, i, o = 0;
    char *out = malloc(n * 3 + 1);

    if (!out)
        return NULL;
    for (i = 0; i < n; i++)
    {
        unsigned char ch = (unsigned char)s[i];

        if (isalnum(ch) || ch == '-' || ch == '.' || ch == '_' || ch == '~')
            out[o++] = (char)ch;
        else
        {
            out[o++] = '%';
            out[o++] = hex[ch >> 4];
            out[o++] = hex[ch & 15];
        }
    }
    out[o] = '\0';
    return out;
}

char *tb_form(const char *phone, const char *message, const char *key)
{
    char *p = tb_escape(phone), *m = tb_escape(message), *k = tb_escape(key);
    char *out = NULL;
    size_t n;

    if (p && m && k)
    {
        n = strlen(p) + strlen(m) + strlen(k) + 32;
        out = malloc(n);
        if (out)
            snprintf(out, n, "phone=%s&message=%s&key=%s", p, m, k);
    }
    free(p);
    free(m);
    free(k);
    return out;
}

int tb_parse_reply(const char *body, tb_reply_t *r)
{
    cJSON *root = body ? cJSON_Parse(body) : NULL;
    const cJSON *ok, *q, *id, *err;

    memset(r, 0, sizeof(*r));
    r->quota = -1;
    ok = cJSON_GetObjectItemCaseSensitive(root, "success");
    if (!cJSON_IsBool(ok))
    {
        cJSON_Delete(root);
        return -1;
    }
    r->success = cJSON_IsTrue(ok);
    q = cJSON_GetObjectItemCaseSensitive(root, "quotaRemaining");
    if (cJSON_IsNumber(q))
        r->quota = (long)q->valuedouble;
    id = cJSON_GetObjectItemCaseSensitive(root, "textId");
    if (cJSON_IsString(id))
        snprintf(r->text_id, sizeof(r->text_id), "%s", id->valuestring);
    else if (cJSON_IsNumber(id))
        snprintf(r->text_id, sizeof(r->text_id), "%.0f", id->valuedouble);
    err = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsString(err))
        snprintf(r->error, sizeof(r->error), "%s", err->valuestring);
    cJSON_Delete(root);
    return 0;
}

/* Append s to out as display text: controls as spaces, runs collapsed. */
static size_t put_text(char *out, size_t off, size_t cap, const char *s)
{
    for (; s && *s && off + 1 < cap; s++)
    {
        char ch = (unsigned char)*s < 32 ? ' ' : *s;

        if (ch == ' ' && (off == 0 || out[off - 1] == ' '))
            continue;
        out[off++] = ch;
    }
    out[off] = '\0';
    return off;
}

void tb_message(const char *title, const char *message, char *out, size_t cap)
{
    char buf[TB_MSG_MAX * 2 + 8];
    size_t off = 0, max = cap - 1 < TB_MSG_MAX ? cap - 1 : TB_MSG_MAX;

    if (!out || cap == 0)
        return;
    buf[0] = '\0';
    /* A title alone is not a message. */
    put_text(buf, 0, sizeof(buf), message);
    if (!buf[0] || strcmp(buf, " ") == 0)
    {
        out[0] = '\0';
        return;
    }
    buf[0] = '\0';
    if (title && title[0])
    {
        off = put_text(buf, off, sizeof(buf), title);
        while (off && buf[off - 1] == ' ')
            buf[--off] = '\0';
        if (off && off + 2 < sizeof(buf))
        {
            buf[off++] = ':';
            buf[off++] = ' ';
            buf[off] = '\0';
        }
    }
    off = put_text(buf, off, sizeof(buf), message);
    while (off && buf[off - 1] == ' ')
        buf[--off] = '\0';
    if (off > max && max >= 4)
    {
        off = max - 3;
        /* Never split a UTF-8 character. */
        while (off && ((unsigned char)buf[off] & 0xC0) == 0x80)
            off--;
        while (off && buf[off - 1] == ' ')
            off--;
        memcpy(buf + off, "...", 4);
    }
    snprintf(out, cap, "%s", buf);
}
