/* SMS alerts through Textbelt (https://textbelt.com).
 *
 * kind "service", driver "textbelt".  A notification pathway: it
 * advertises MF_CAP_NOTIFY and a "notify" block in describe(), and
 * delivers the "notify" action as one text per number in its "to"
 * (PLUGINS.md, "Notification pathways").  It keeps no recipients:
 * whoever raises an alert chooses whom it goes to.  Its settings are the
 * API key (a "secret" field, which the daemon never shows back) and a cap
 * on texts per hour (textbelt.max_per_hour).  The "test" action texts a
 * number the user gives.
 *
 * Credits left come from every send's reply and from GET /quota/{key},
 * checked every poll_interval_s (at least 60 s, default an hour), at once
 * when the key changes, and on the "refresh" action.  The module is
 * offline without a key, when Textbelt does not know the key, when it
 * cannot be reached, and when no credits are left.
 *
 * Requests run on libcurl's multi interface, one at a time and sends a
 * second apart (Textbelt asks for no more than 1-2 a second), driven from
 * the daemon's loop through prepare_fds() and step(), so nothing blocks.
 * MF_TEXTBELT_URL replaces https://textbelt.com (tests use a local fake).
 */

#include "mf_plugin.h"
#include "tb.h"

#include <cJSON.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TB_API          "https://textbelt.com"
#define TB_TIMEOUT_S    20L
#define TB_CHECK_MIN_S  60.0
#define TB_CHECK_S      3600.0          /* default credit check period */
#define TB_RETRY_S      60.0            /* after a request that failed */
#define TB_GAP_S        1.0             /* between sends */
#define TB_BODY_MAX     16384
#define TB_KEY_MAX      128
#define TB_TO_MAX       8               /* numbers in one notify */
#define TB_QUEUE        16              /* texts waiting to be sent */
#define TB_RATE_MAX     100             /* highest textbelt.max_per_hour */
#define TB_RATE_DEFAULT 20
#define TB_HOUR_S       3600.0
#define TB_MASK         "********"      /* the daemon's secret mask */

enum { REQ_NONE = 0, REQ_QUOTA, REQ_SEND };
enum { JOB_TEST = 1, JOB_NOTIFY };
enum { ACT_NONE = 0, ACT_RUNNING, ACT_DONE, ACT_FAILED };

typedef struct {
    int  kind;
    char to[TB_PHONE_MAX];
    char msg[TB_MSG_MAX + 1];
} tb_job_t;

typedef struct {
    char     name[64];
    char     key[TB_KEY_MAX];
    int      max_per_hour;              /* 0 = no limit */
    double   check_every;
    char     base[160];

    CURLM   *multi;
    CURL    *easy;
    int      req;
    char    *body;
    size_t   len;
    size_t   cap;

    tb_job_t queue[TB_QUEUE];
    int      qhead;
    int      qlen;
    tb_job_t cur;                       /* the send in flight */
    double   queued_at[TB_RATE_MAX];    /* when texts were accepted */
    int      nqueued_at;

    double   next_check;
    double   next_send;
    int      key_state;                 /* -1 unknown, 0 unknown to Textbelt,
                                           1 good */
    long     credits;                   /* -1 unknown */
    time_t   credits_at;
    char     net_err[112];              /* the last request failed */

    unsigned sent;
    unsigned failed;
    struct {
        int    have;
        int    kind;
        char   to[16];                  /* masked */
        int    ok;
        char   text_id[32];
        char   error[96];
        time_t at;
    } last;
    struct {                            /* the last "test", for the form */
        unsigned seq;
        int      state;
        char     text[128];
    } act;
    int      changed;
    char     err[128];
} tb_ctx_t;

static int g_curl_ready;

static double mono_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int fail(char *err, size_t errsz, int rc, const char *msg)
{
    if (err && errsz)
        snprintf(err, errsz, "%s", msg);
    return rc;
}

/* A setting from open()'s spec: nested ({"textbelt":{"key":..}}, as the
 * config has it) or dotted ("textbelt.key"). */
static const cJSON *spec_item(const cJSON *spec, const char *key)
{
    char dotted[48];
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(spec, "textbelt"), key);

    if (v)
        return v;
    snprintf(dotted, sizeof(dotted), "textbelt.%s", key);
    return cJSON_GetObjectItemCaseSensitive(spec, dotted);
}

static double num_of(const cJSON *v, double dflt)
{
    if (cJSON_IsNumber(v))
        return v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring[0])
        return atof(v->valuestring);
    return dflt;
}

/* Keys are printable ASCII without spaces; a mask is never a key. */
static int key_ok(const char *k, char *err, size_t errsz)
{
    const char *p;

    if (strlen(k) >= TB_KEY_MAX)
        return fail(err, errsz, -1, "textbelt.key is too long");
    for (p = k; *p; p++)
        if (*p <= ' ' || *p > '~')
            return fail(err, errsz, -1, "textbelt.key has spaces or odd characters");
    return 0;
}

static int rate_ok(double v, char *err, size_t errsz)
{
    if (v < 0 || v > TB_RATE_MAX || v != (double)(int)v)
        return fail(err, errsz, -1, "textbelt.max_per_hour must be a whole "
                    "number from 0 (no limit) to 100");
    return 0;
}

static int to_parse(const char *list, char (*out)[TB_PHONE_MAX], int *n,
                    char *err, size_t errsz)
{
    char bad[64];
    int k = tb_recipients(list, out, TB_TO_MAX, bad, sizeof(bad));

    if (k < 0)
    {
        if (err && errsz)
            snprintf(err, errsz, "%s%.60s",
                     strcmp(bad, "too many numbers") == 0 ? "\"to\": " :
                     "not a phone number: ", bad);
        return -1;
    }
    *n = k;
    return 0;
}

/* The key as the settings show it: "********" and, when the key is long
 * enough that they give nothing away, its last four characters.  The
 * daemon masks "secret" fields the same way; a mask sent back is ignored. */
static void mask_key(const char *k, char *out, size_t cap)
{
    size_t n = strlen(k);

    if (!n)
        snprintf(out, cap, "%s", "");
    else if (n >= 12)
        snprintf(out, cap, "%s%s", TB_MASK, k + n - 4);
    else
        snprintf(out, cap, "%s", TB_MASK);
}

/* ---- the queue and the hourly cap ------------------------------------- */

static int texts_in_hour(const tb_ctx_t *c, double now)
{
    int i, n = 0;

    for (i = 0; i < c->nqueued_at; i++)
        if (now - c->queued_at[i] < TB_HOUR_S)
            n++;
    return n;
}

static void note_queued(tb_ctx_t *c, double now)
{
    int i, old = 0;

    if (c->nqueued_at < TB_RATE_MAX)
    {
        c->queued_at[c->nqueued_at++] = now;
        return;
    }
    for (i = 1; i < TB_RATE_MAX; i++)
        if (c->queued_at[i] < c->queued_at[old])
            old = i;
    c->queued_at[old] = now;
}

/* Can n more texts go out?  MF_OK, or an error for the action. */
static int admit(tb_ctx_t *c, int n, double now, char *err, size_t errsz)
{
    int used;

    if (!c->key[0])
        return fail(err, errsz, MF_ERR_INVAL, "set the Textbelt API key first");
    if (c->key_state == 0)
        return fail(err, errsz, MF_ERR_OFFLINE, "Textbelt does not know this API key");
    if (n > TB_QUEUE - c->qlen)
        return fail(err, errsz, MF_ERR_BUSY, "too many texts waiting to go out");
    used = texts_in_hour(c, now);
    if (c->max_per_hour > 0 && used + n > c->max_per_hour)
    {
        if (err && errsz)
            snprintf(err, errsz, "over the limit of %d texts an hour "
                     "(textbelt.max_per_hour)", c->max_per_hour);
        return MF_ERR_BUSY;
    }
    return MF_OK;
}

static void queue_push(tb_ctx_t *c, int kind, const char *to, const char *msg,
                       double now)
{
    tb_job_t *j = &c->queue[(c->qhead + c->qlen) % TB_QUEUE];

    j->kind = kind;
    snprintf(j->to, sizeof(j->to), "%s", to);
    snprintf(j->msg, sizeof(j->msg), "%s", msg);
    c->qlen++;
    note_queued(c, now);
}

static int queue_pop(tb_ctx_t *c, tb_job_t *out)
{
    if (!c->qlen)
        return 0;
    *out = c->queue[c->qhead];
    c->qhead = (c->qhead + 1) % TB_QUEUE;
    c->qlen--;
    return 1;
}

/* ---- HTTP (libcurl multi) --------------------------------------------- */

static size_t on_body(char *p, size_t sz, size_t n, void *arg)
{
    tb_ctx_t *c = arg;
    size_t add = sz * n;

    if (c->len + add + 1 > TB_BODY_MAX)
        return 0;                       /* not a Textbelt reply */
    if (c->len + add + 1 > c->cap)
    {
        size_t ncap = c->cap ? c->cap * 2 : 4096;
        char *nb;

        while (ncap < c->len + add + 1)
            ncap *= 2;
        nb = realloc(c->body, ncap);
        if (!nb)
            return 0;
        c->body = nb;
        c->cap = ncap;
    }
    memcpy(c->body + c->len, p, add);
    c->len += add;
    c->body[c->len] = '\0';
    return add;
}

static int start(tb_ctx_t *c, int req, const char *url, const char *post)
{
    c->easy = curl_easy_init();
    if (!c->easy)
        return -1;
    c->len = 0;
    if (c->body)
        c->body[0] = '\0';
    curl_easy_setopt(c->easy, CURLOPT_URL, url);
    curl_easy_setopt(c->easy, CURLOPT_USERAGENT, "moonflare-textbelt/0.1");
    curl_easy_setopt(c->easy, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(c->easy, CURLOPT_WRITEDATA, c);
    curl_easy_setopt(c->easy, CURLOPT_TIMEOUT, TB_TIMEOUT_S);
    curl_easy_setopt(c->easy, CURLOPT_NOSIGNAL, 1L);
    if (post)
        curl_easy_setopt(c->easy, CURLOPT_COPYPOSTFIELDS, post);
    if (curl_multi_add_handle(c->multi, c->easy) != CURLM_OK)
    {
        curl_easy_cleanup(c->easy);
        c->easy = NULL;
        return -1;
    }
    c->req = req;
    return 0;
}

static void end_request(tb_ctx_t *c)
{
    if (c->easy)
    {
        curl_multi_remove_handle(c->multi, c->easy);
        curl_easy_cleanup(c->easy);
        c->easy = NULL;
    }
    c->req = REQ_NONE;
}

static void set_credits(tb_ctx_t *c, long q)
{
    if (q < 0)
        return;
    c->credits = q;
    c->credits_at = time(NULL);
}

/* A text went out, or did not: the reading, the counters, and the form's
 * answer to a test. */
static void finish_send(tb_ctx_t *c, const tb_reply_t *r, const char *why)
{
    c->last.have = 1;
    c->last.kind = c->cur.kind;
    tb_phone_mask(c->cur.to, c->last.to, sizeof(c->last.to));
    c->last.at = time(NULL);
    c->last.text_id[0] = '\0';
    c->last.error[0] = '\0';
    c->last.ok = r && r->success;
    if (r)
    {
        c->net_err[0] = '\0';
        set_credits(c, r->quota);
    }
    if (c->last.ok)
    {
        c->sent++;
        c->key_state = 1;
        snprintf(c->last.text_id, sizeof(c->last.text_id), "%s", r->text_id);
    }
    else
    {
        c->failed++;
        snprintf(c->last.error, sizeof(c->last.error), "%s",
                 r ? (r->error[0] ? r->error : "Textbelt did not send it") : why);
        if (!r)
            snprintf(c->net_err, sizeof(c->net_err), "%s", why);
    }
    if (c->cur.kind == JOB_TEST && c->act.state == ACT_RUNNING)
    {
        if (!c->last.ok)
        {
            c->act.state = ACT_FAILED;
            snprintf(c->act.text, sizeof(c->act.text), "%s", c->last.error);
        }
        else
        {
            c->act.state = ACT_DONE;
            if (c->credits >= 0)
                snprintf(c->act.text, sizeof(c->act.text),
                         "Sent to %s. %ld credits left.", c->last.to, c->credits);
            else
                snprintf(c->act.text, sizeof(c->act.text), "Sent to %s.",
                         c->last.to);
        }
    }
    c->changed = 1;
}

static void finish(tb_ctx_t *c, CURLcode rc)
{
    long code = 0;
    tb_reply_t r;
    char why[112] = "";
    int req = c->req;
    double now = mono_now();

    curl_easy_getinfo(c->easy, CURLINFO_RESPONSE_CODE, &code);
    if (rc != CURLE_OK)
        snprintf(why, sizeof(why), "can't reach Textbelt: %.80s",
                 curl_easy_strerror(rc));
    else if (tb_parse_reply(c->body, &r) != 0)
        snprintf(why, sizeof(why), "Textbelt answered HTTP %ld, not a reply", code);
    end_request(c);
    if (req == REQ_SEND)
    {
        finish_send(c, why[0] ? NULL : &r, why);
        return;
    }
    if (why[0])
    {
        snprintf(c->net_err, sizeof(c->net_err), "%s", why);
        c->next_check = now + TB_RETRY_S;
    }
    else
    {
        c->net_err[0] = '\0';
        c->key_state = r.success ? 1 : 0;
        if (r.success)
            set_credits(c, r.quota);
        c->next_check = now + c->check_every;
    }
    c->changed = 1;
}

static void start_check(tb_ctx_t *c, double now)
{
    char url[sizeof(c->base) + 3 * TB_KEY_MAX + 16];
    char *k = tb_escape(c->key);

    c->next_check = now + TB_RETRY_S;   /* until the answer comes */
    if (!k)
        return;
    snprintf(url, sizeof(url), "%s/quota/%s", c->base, k);
    free(k);
    if (start(c, REQ_QUOTA, url, NULL) != 0)
        snprintf(c->net_err, sizeof(c->net_err), "could not start a request");
    memset(url, 0, sizeof(url));        /* it held the key */
}

static void start_send(tb_ctx_t *c, double now)
{
    char url[sizeof(c->base) + 8];
    char *form;

    if (!queue_pop(c, &c->cur))
        return;
    c->next_send = now + TB_GAP_S;
    form = tb_form(c->cur.to, c->cur.msg, c->key);
    snprintf(url, sizeof(url), "%s/text", c->base);
    if (!form || start(c, REQ_SEND, url, form) != 0)
        finish_send(c, NULL, "could not start a request");
    if (form)
    {
        memset(form, 0, strlen(form));  /* it held the key */
        free(form);
    }
}

/* Why the module is offline (and c->err says so), or 0 when it is up. */
static int offline(tb_ctx_t *c)
{
    const char *why = NULL;

    if (!c->key[0])
        why = "no API key: set it in the module's settings";
    else if (c->key_state == 0)
        why = "Textbelt does not know this API key";
    else if (c->net_err[0])
        why = c->net_err;
    else if (c->key_state < 0)
        why = "checking the API key with Textbelt";
    else if (c->credits == 0)
        why = "no Textbelt credits left";
    if (!why)
    {
        c->err[0] = '\0';
        return 0;
    }
    snprintf(c->err, sizeof(c->err), "%s", why);
    return 1;
}

/* ---- plugin ops -------------------------------------------------------- */

static void *tb_open(const char *spec_json, char *err, size_t errsz)
{
    cJSON *spec = spec_json ? cJSON_Parse(spec_json) : NULL;
    const cJSON *v;
    const char *base = getenv("MF_TEXTBELT_URL");
    tb_ctx_t *c;
    double rate;

    if (!cJSON_IsObject(spec))
    {
        cJSON_Delete(spec);
        fail(err, errsz, 0, "bad module config");
        return NULL;
    }
    c = calloc(1, sizeof(*c));
    if (!c)
    {
        cJSON_Delete(spec);
        return NULL;
    }
    v = cJSON_GetObjectItemCaseSensitive(spec, "name");
    snprintf(c->name, sizeof(c->name), "%s",
             cJSON_IsString(v) ? v->valuestring : "Moon Flare");
    v = spec_item(spec, "key");
    if (cJSON_IsString(v) && strncmp(v->valuestring, TB_MASK, strlen(TB_MASK)) != 0)
        snprintf(c->key, sizeof(c->key), "%s", v->valuestring);
    rate = num_of(spec_item(spec, "max_per_hour"), TB_RATE_DEFAULT);
    c->check_every = num_of(cJSON_GetObjectItemCaseSensitive(spec, "poll_interval_s"),
                            TB_CHECK_S);
    cJSON_Delete(spec);
    if (c->check_every < TB_CHECK_MIN_S)
        c->check_every = TB_CHECK_MIN_S;
    if (key_ok(c->key, err, errsz) != 0 || rate_ok(rate, err, errsz) != 0)
    {
        memset(c->key, 0, sizeof(c->key));
        free(c);
        return NULL;
    }
    c->max_per_hour = (int)rate;
    if (!g_curl_ready)
    {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        {
            memset(c->key, 0, sizeof(c->key));
            free(c);
            fail(err, errsz, 0, "curl_global_init failed");
            return NULL;
        }
        g_curl_ready = 1;
    }
    c->multi = curl_multi_init();
    if (!c->multi)
    {
        memset(c->key, 0, sizeof(c->key));
        free(c);
        fail(err, errsz, 0, "curl setup failed");
        return NULL;
    }
    snprintf(c->base, sizeof(c->base), "%s", base && base[0] ? base : TB_API);
    c->key_state = -1;
    c->credits = -1;
    return c;                           /* the key is checked in step() */
}

static void tb_close(void *ctx)
{
    tb_ctx_t *c = ctx;

    if (!c)
        return;
    end_request(c);
    curl_multi_cleanup(c->multi);
    free(c->body);
    memset(c->key, 0, sizeof(c->key));
    free(c);
}

/* All I/O goes through prepare_fds; there is no single fd. */
static int tb_fd(void *ctx)
{
    (void)ctx;
    return -1;
}

static unsigned tb_select_mask(void *ctx)
{
    (void)ctx;
    return 0;
}

static void tb_prepare_fds(void *ctx, fd_set *r, fd_set *w, int *maxfd)
{
    tb_ctx_t *c = ctx;
    fd_set ex;
    int mx = -1;

    if (!c || !c->easy)
        return;
    FD_ZERO(&ex);
    if (curl_multi_fdset(c->multi, r, w, &ex, &mx) == CURLM_OK &&
        maxfd && mx > *maxfd)
        *maxfd = mx;
}

static mf_step_t tb_step(void *ctx)
{
    tb_ctx_t *c = ctx;
    double now = mono_now();
    int running = 0, left = 0;
    CURLMsg *msg;

    if (c->easy)
    {
        curl_multi_perform(c->multi, &running);
        while ((msg = curl_multi_info_read(c->multi, &left)) != NULL)
        {
            if (msg->msg == CURLMSG_DONE && msg->easy_handle == c->easy)
            {
                finish(c, msg->data.result);
                break;
            }
        }
    }
    if (!c->easy && c->key[0])
    {
        if (c->qlen && now >= c->next_send)
            start_send(c, now);
        else if (now >= c->next_check)
            start_check(c, now);
    }
    if (offline(c))
        return MF_STEP_ERROR;
    if (c->changed)
    {
        c->changed = 0;
        return MF_STEP_UPDATED;
    }
    return MF_STEP_IDLE;
}

static unsigned tb_caps(void *ctx)
{
    (void)ctx;
    return MF_CAP_READ | MF_CAP_WRITE_SETTINGS | MF_CAP_ACTION_REFRESH |
           MF_CAP_NOTIFY;
}

static const char *tb_last_error(void *ctx)
{
    tb_ctx_t *c = ctx;

    return c && c->err[0] ? c->err : NULL;
}

static int print_to(cJSON *o, char *json, size_t cap)
{
    char *s = cJSON_PrintUnformatted(o);
    int rc = MF_ERR_INVAL;

    cJSON_Delete(o);
    if (s && strlen(s) < cap)
    {
        memcpy(json, s, strlen(s) + 1);
        rc = MF_OK;
    }
    free(s);
    return rc;
}

/* The reading carries no phone number in full and never the key. */
static int tb_get_reading(void *ctx, char *json, size_t cap)
{
    tb_ctx_t *c = ctx;
    cJSON *o = cJSON_CreateObject();
    cJSON *l;

    if (!o)
        return MF_ERR_INVAL;
    cJSON_AddStringToObject(o, "provider", "textbelt");
    cJSON_AddStringToObject(o, "channel", "sms");
    cJSON_AddBoolToObject(o, "ready", !offline(c));
    if (c->credits >= 0)
    {
        cJSON_AddNumberToObject(o, "credits_remaining", (double)c->credits);
        cJSON_AddNumberToObject(o, "credits_ts", (double)c->credits_at);
    }
    else
    {
        cJSON_AddNullToObject(o, "credits_remaining");
        cJSON_AddNullToObject(o, "credits_ts");
    }
    cJSON_AddNumberToObject(o, "texts_sent", c->sent);
    cJSON_AddNumberToObject(o, "texts_failed", c->failed);
    cJSON_AddNumberToObject(o, "texts_queued", c->qlen + (c->req == REQ_SEND));
    if (!c->last.have)
        cJSON_AddNullToObject(o, "last_text");
    else
    {
        l = cJSON_AddObjectToObject(o, "last_text");
        cJSON_AddStringToObject(l, "kind", c->last.kind == JOB_TEST ? "test" : "notify");
        cJSON_AddStringToObject(l, "to", c->last.to);
        cJSON_AddBoolToObject(l, "ok", c->last.ok);
        if (c->last.text_id[0])
            cJSON_AddStringToObject(l, "text_id", c->last.text_id);
        else
            cJSON_AddNullToObject(l, "text_id");
        if (c->last.error[0])
            cJSON_AddStringToObject(l, "error", c->last.error);
        else
            cJSON_AddNullToObject(l, "error");
        cJSON_AddNumberToObject(l, "ts", (double)c->last.at);
    }
    return print_to(o, json, cap);
}

/* The settings form: the key (masked) and the cap, the credits and last
 * text (read-only), and "_action", how the last test went, which the form
 * waits on. */
static int tb_get_settings(void *ctx, char *json, size_t cap)
{
    tb_ctx_t *c = ctx;
    cJSON *o = cJSON_CreateObject();
    char last[128], hhmm[8] = "", masked[16];

    if (!o)
        return MF_ERR_INVAL;
    mask_key(c->key, masked, sizeof(masked));
    cJSON_AddStringToObject(o, "textbelt.key", masked);
    cJSON_AddNumberToObject(o, "textbelt.max_per_hour", c->max_per_hour);
    if (c->credits >= 0)
        cJSON_AddNumberToObject(o, "textbelt.credits", (double)c->credits);
    else
        cJSON_AddStringToObject(o, "textbelt.credits", "unknown");
    if (c->last.have)
    {
        struct tm tm;

        if (localtime_r(&c->last.at, &tm))
            strftime(hhmm, sizeof(hhmm), "%H:%M", &tm);
        if (c->last.ok)
            snprintf(last, sizeof(last), "%s %s to %s", hhmm,
                     c->last.kind == JOB_TEST ? "test sent" : "sent", c->last.to);
        else
            snprintf(last, sizeof(last), "%s failed: %.90s", hhmm, c->last.error);
    }
    else
        snprintf(last, sizeof(last), "none yet");
    cJSON_AddStringToObject(o, "textbelt.last_text", last);
    if (c->act.seq)
    {
        cJSON *a = cJSON_AddObjectToObject(o, "_action");

        cJSON_AddStringToObject(a, "name", "test");
        cJSON_AddNumberToObject(a, "seq", c->act.seq);
        cJSON_AddStringToObject(a, "state", c->act.state == ACT_RUNNING ? "running" :
                                c->act.state == ACT_DONE ? "done" : "failed");
        cJSON_AddStringToObject(a, "text", c->act.text);
    }
    return print_to(o, json, cap);
}

/* Key, cap and check period change live.  Everything is checked before
 * anything changes; a new key is checked with Textbelt at once. */
static int tb_put_settings(void *ctx, const char *json, char *err, size_t errsz)
{
    tb_ctx_t *c = ctx;
    cJSON *b = json ? cJSON_Parse(json) : NULL;
    const cJSON *kj, *rj, *pj;
    char key[TB_KEY_MAX];
    int rc = MF_OK;
    double rate = c->max_per_hour, now = mono_now();

    if (!cJSON_IsObject(b))
    {
        cJSON_Delete(b);
        return fail(err, errsz, MF_ERR_INVAL, "settings are not a JSON object");
    }
    kj = cJSON_GetObjectItemCaseSensitive(b, "textbelt.key");
    rj = cJSON_GetObjectItemCaseSensitive(b, "textbelt.max_per_hour");
    pj = cJSON_GetObjectItemCaseSensitive(b, "poll_interval_s");
    snprintf(key, sizeof(key), "%s", c->key);
    /* A mask sent back is the key unchanged. */
    if (cJSON_IsString(kj) &&
        strncmp(kj->valuestring, TB_MASK, strlen(TB_MASK)) != 0)
    {
        if (strlen(kj->valuestring) >= sizeof(key))
            rc = fail(err, errsz, MF_ERR_INVAL, "textbelt.key is too long");
        else
        {
            snprintf(key, sizeof(key), "%s", kj->valuestring);
            if (key_ok(key, err, errsz) != 0)
                rc = MF_ERR_INVAL;
        }
    }
    else if (kj && !cJSON_IsString(kj))
        rc = fail(err, errsz, MF_ERR_INVAL, "textbelt.key must be text");
    if (rc == MF_OK && rj)
    {
        rate = num_of(rj, -1.0);
        if (rate_ok(rate, err, errsz) != 0)
            rc = MF_ERR_INVAL;
    }
    cJSON_Delete(b);
    if (rc != MF_OK)
    {
        memset(key, 0, sizeof(key));
        return rc;
    }
    if (pj)
    {
        c->check_every = num_of(pj, c->check_every);
        if (c->check_every < TB_CHECK_MIN_S)
            c->check_every = TB_CHECK_MIN_S;
        c->next_check = now + c->check_every;
    }
    if (strcmp(key, c->key) != 0)
    {
        snprintf(c->key, sizeof(c->key), "%s", key);
        c->key_state = -1;
        c->credits = -1;
        c->net_err[0] = '\0';
        c->next_check = 0.0;            /* check the new key now */
    }
    memset(key, 0, sizeof(key));
    c->max_per_hour = (int)rate;
    c->changed = 1;
    return MF_OK;
}

/* "to" in a notify: one number, a comma-separated list, or an array. */
static int notify_to(const cJSON *to, char (*out)[TB_PHONE_MAX], int *n,
                     char *err, size_t errsz)
{
    const cJSON *it;
    int k = 0;

    if (!to)
        return fail(err, errsz, -1, "notify needs \"to\": a phone number or a "
                    "list");
    if (cJSON_IsString(to))
        return to_parse(to->valuestring, out, n, err, errsz);
    if (!cJSON_IsArray(to))
        return fail(err, errsz, -1, "\"to\" must be a phone number or a list");
    cJSON_ArrayForEach(it, to)
    {
        if (k >= TB_TO_MAX)
            return fail(err, errsz, -1, "\"to\": too many numbers");
        if (!cJSON_IsString(it) || tb_phone_norm(it->valuestring, out[k],
                                                 TB_PHONE_MAX) != 0)
        {
            if (err && errsz)
                snprintf(err, errsz, "not a phone number: %.60s",
                         cJSON_IsString(it) ? it->valuestring : "(not text)");
            return -1;
        }
        k++;
    }
    *n = k;
    return 0;
}

/* "test" {"to"}: one test text.  "notify" {"message", "title"?, "to"}:
 * one text to each number in "to".
 * "refresh": check the credits now.  Texts are queued; how they went shows
 * in the reading ("last_text") and, for a test, in the form. */
static int tb_action(void *ctx, const char *action, const char *json,
                     char *err, size_t errsz)
{
    tb_ctx_t *c = ctx;
    cJSON *b = json ? cJSON_Parse(json) : NULL;
    double now = mono_now();
    int rc = MF_OK;

    if (action && strcmp(action, "refresh") == 0)
    {
        cJSON_Delete(b);
        c->next_check = 0.0;
        return MF_OK;
    }
    if (action && strcmp(action, "test") == 0)
    {
        const cJSON *to = cJSON_GetObjectItemCaseSensitive(b, "to");
        char phone[TB_PHONE_MAX], msg[TB_MSG_MAX + 1], masked[16];

        if (!cJSON_IsString(to) || tb_phone_norm(to->valuestring, phone,
                                                 sizeof(phone)) != 0)
        {
            if (err && errsz)
                snprintf(err, errsz, "not a phone number: %.60s",
                         cJSON_IsString(to) ? to->valuestring : "(none given)");
            rc = MF_ERR_INVAL;
        }
        else if ((rc = admit(c, 1, now, err, errsz)) == MF_OK)
        {
            snprintf(msg, sizeof(msg), "Moon Flare test from %.60s: SMS alerts "
                     "work.", c->name);
            queue_push(c, JOB_TEST, phone, msg, now);
            tb_phone_mask(phone, masked, sizeof(masked));
            c->act.seq++;
            c->act.state = ACT_RUNNING;
            snprintf(c->act.text, sizeof(c->act.text), "Sending a test to %s...",
                     masked);
            c->changed = 1;
        }
        cJSON_Delete(b);
        return rc;
    }
    if (action && strcmp(action, "notify") == 0)
    {
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(b, "message");
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(b, "title");
        char list[TB_TO_MAX][TB_PHONE_MAX], text[TB_MSG_MAX + 1];
        int n = 0, i;

        text[0] = '\0';
        if (cJSON_IsString(m))
            tb_message(cJSON_IsString(t) ? t->valuestring : NULL, m->valuestring,
                       text, sizeof(text));
        if (!cJSON_IsString(m) || !text[0])
            rc = fail(err, errsz, MF_ERR_INVAL, "notify needs a \"message\"");
        else if (notify_to(cJSON_GetObjectItemCaseSensitive(b, "to"), list, &n,
                           err, errsz) != 0)
            rc = MF_ERR_INVAL;
        else if (n == 0)
            rc = fail(err, errsz, MF_ERR_INVAL, "notify needs at least one "
                      "number in \"to\"");
        else if ((rc = admit(c, n, now, err, errsz)) == MF_OK)
        {
            for (i = 0; i < n; i++)
                queue_push(c, JOB_NOTIFY, list[i], text, now);
            c->changed = 1;
        }
        cJSON_Delete(b);
        return rc;
    }
    cJSON_Delete(b);
    if (err && errsz)
        snprintf(err, errsz, "unknown action: %.40s", action ? action : "");
    return MF_ERR_UNSUPPORTED;
}

static const char *tb_describe(void)
{
    return
        "{\"fields\":["
        "{\"key\":\"poll_interval_s\",\"label\":\"Check Credits\","
        "\"hint\":\"(>=60 sec)\",\"type\":\"number\",\"default\":3600},"
        "{\"key\":\"textbelt.key\",\"label\":\"API Key\","
        "\"hint\":\"(textbelt.com)\",\"type\":\"secret\",\"required\":true},"
        "{\"key\":\"textbelt.max_per_hour\",\"label\":\"Max Texts/Hour\","
        "\"hint\":\"(0=no limit)\",\"type\":\"number\",\"default\":20},"
        "{\"key\":\"textbelt.test\",\"label\":\"Send Test SMS\","
        "\"hint\":\"(phone number)\",\"type\":\"action\",\"action\":\"test\","
        "\"param\":\"to\",\"button\":\"Send\"},"
        "{\"key\":\"textbelt.credits\",\"label\":\"Credits Left\","
        "\"type\":\"number\",\"readonly\":true},"
        "{\"key\":\"textbelt.last_text\",\"label\":\"Last Text\","
        "\"type\":\"string\",\"readonly\":true}"
        "],"
        "\"notify\":{\"channel\":\"sms\",\"action\":\"notify\","
        "\"max_chars\":320}}";
}

static const mf_plugin_ops_t g_ops = {
    .abi          = MF_PLUGIN_ABI,
    .ops_size     = sizeof(mf_plugin_ops_t),
    .kind         = "service",
    .driver       = "textbelt",
    .version      = "0.1.0",
    .open         = tb_open,
    .close        = tb_close,
    .fd           = tb_fd,
    .select_mask  = tb_select_mask,
    .prepare_fds  = tb_prepare_fds,
    .step         = tb_step,
    .caps         = tb_caps,
    .last_error   = tb_last_error,
    .get_reading  = tb_get_reading,
    .get_settings = tb_get_settings,
    .put_settings = tb_put_settings,
    .action       = tb_action,
    .describe     = tb_describe,
};

size_t mf_plugin_entries(const mf_plugin_ops_t **out)
{
    *out = &g_ops;
    return 1;
}
