/*
 * The Textbelt plugin against a fake Textbelt: a child process that serves
 * GET /quota/{key} and POST /text on 127.0.0.1 and logs every request.
 * Covers the credit check, the test and notify actions, a failed send, the
 * hourly cap, settings (the key masked, a mask sent back), a key Textbelt
 * does not know, no credits, no key, and an unreachable server.
 *
 *   test_textbelt_plugin libmf_service_textbelt.so
 */

#define _GNU_SOURCE

#include "mf_plugin.h"

#include <cJSON.h>
#include <dlfcn.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int g_fail;
static const mf_plugin_ops_t *g_ops;
static char g_log[64];

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); g_fail++; } \
} while (0)

#define GOOD_KEY  "goodkey1234567"
#define NEW_KEY   "otherkey7654321"

/* ---- the fake Textbelt ------------------------------------------------- */

static void reply(int fd, const char *json)
{
    char out[1024];
    int n = snprintf(out, sizeof(out), "HTTP/1.1 200 OK\r\n"
                     "Content-Type: application/json\r\nContent-Length: %zu\r\n"
                     "Connection: close\r\n\r\n%s", strlen(json), json);

    if (n > 0 && write(fd, out, (size_t)n) != n)
        _exit(3);
}

static void serve(int lfd)
{
    int credits = 40, next_id = 100;

    for (;;)
    {
        char req[8192], method[8] = "", path[512] = "", json[256];
        const char *body = "";
        size_t n = 0;
        long clen = 0;
        int fd = accept(lfd, NULL, NULL);
        FILE *log;

        if (fd < 0)
            continue;
        for (;;)
        {
            ssize_t r = read(fd, req + n, sizeof(req) - 1 - n);
            char *hdr, *cl;

            if (r <= 0)
                break;
            n += (size_t)r;
            req[n] = '\0';
            hdr = strstr(req, "\r\n\r\n");
            if (!hdr)
                continue;
            cl = strcasestr(req, "Content-Length:");
            clen = cl && cl < hdr ? atol(cl + 15) : 0;
            body = hdr + 4;
            if ((long)strlen(body) >= clen)
                break;
        }
        sscanf(req, "%7s %511s", method, path);
        log = fopen(g_log, "a");
        if (log)
        {
            fprintf(log, "%s %s %s\n", method, path, body);
            fclose(log);
        }
        if (strncmp(path, "/quota/", 7) == 0)
        {
            const char *k = path + 7;

            if (strcmp(k, GOOD_KEY) == 0)
                snprintf(json, sizeof(json),
                         "{\"success\":true,\"quotaRemaining\":%d}", credits);
            else if (strcmp(k, NEW_KEY) == 0)
                snprintf(json, sizeof(json), "{\"success\":true,\"quotaRemaining\":0}");
            else
                snprintf(json, sizeof(json), "{\"success\":false,\"quotaRemaining\":0}");
        }
        else if (strcmp(path, "/text") == 0)
        {
            if (strstr(body, "phone=5550000000"))
                snprintf(json, sizeof(json), "{\"success\":false,"
                         "\"quotaRemaining\":%d,\"error\":\"Invalid phone number\"}",
                         credits);
            else
                snprintf(json, sizeof(json), "{\"success\":true,"
                         "\"quotaRemaining\":%d,\"textId\":\"%d\"}", --credits,
                         next_id++);
        }
        else
            snprintf(json, sizeof(json), "{\"success\":false,\"error\":\"?\"}");
        reply(fd, json);
        close(fd);
    }
}

static pid_t start_server(int *port)
{
    struct sockaddr_in a;
    socklen_t al = sizeof(a);
    int lfd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    pid_t pid;

    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (lfd < 0 || bind(lfd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
        listen(lfd, 8) != 0 || getsockname(lfd, (struct sockaddr *)&a, &al) != 0)
        return -1;
    *port = ntohs(a.sin_port);
    pid = fork();
    if (pid == 0)
    {
        serve(lfd);
        _exit(0);
    }
    close(lfd);
    return pid;
}

/* How many logged requests contain s. */
static int logged(const char *s)
{
    FILE *f = fopen(g_log, "r");
    char line[4096];
    int n = 0;

    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f))
        if (strstr(line, s))
            n++;
    fclose(f);
    return n;
}

/* ---- driving the plugin like the daemon ------------------------------- */

static double now_mono(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static mf_step_t tick(void *c)
{
    fd_set r, w;
    int maxfd = -1;
    struct timeval tv = { 0, 50000 };

    FD_ZERO(&r);
    FD_ZERO(&w);
    g_ops->prepare_fds(c, &r, &w, &maxfd);
    (void)select(maxfd + 1, &r, &w, NULL, &tv);
    return g_ops->step(c);
}

static cJSON *settings(void *c)
{
    static char buf[4096];

    if (g_ops->get_settings(c, buf, sizeof(buf)) != MF_OK)
        return NULL;
    return cJSON_Parse(buf);
}

static cJSON *reading(void *c, char *raw, size_t cap)
{
    static char buf[4096];

    if (g_ops->get_reading(c, buf, sizeof(buf)) != MF_OK)
        return NULL;
    if (raw)
        snprintf(raw, cap, "%s", buf);
    return cJSON_Parse(buf);
}

static double num(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);

    return cJSON_IsNumber(v) ? v->valuedouble : -999;
}

static const char *str(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);

    return cJSON_IsString(v) ? v->valuestring : "";
}

/* Step until the plugin is online (or not), up to secs. */
static int run_until_online(void *c, double secs, int want)
{
    double end = now_mono() + secs;
    mf_step_t st = MF_STEP_ERROR;

    while (now_mono() < end)
    {
        st = tick(c);
        if ((st != MF_STEP_ERROR) == want)
            return 1;
    }
    return 0;
}

/* Step until the last test is no longer "running". */
static cJSON *run_until_test_done(void *c, double secs)
{
    double end = now_mono() + secs;

    while (now_mono() < end)
    {
        cJSON *s;
        const cJSON *a;

        tick(c);
        s = settings(c);
        a = cJSON_GetObjectItemCaseSensitive(s, "_action");
        if (a && strcmp(str(a, "state"), "running") != 0)
            return s;
        cJSON_Delete(s);
    }
    return NULL;
}

static void run_for(void *c, double secs)
{
    double end = now_mono() + secs;

    while (now_mono() < end)
        tick(c);
}

static void *open_ctx(const char *key, int max_per_hour, char *err, size_t errsz)
{
    char spec[512];

    snprintf(spec, sizeof(spec),
             "{\"uuid\":\"bbbbbbbb-0000-4000-8000-000000000001\",\"name\":\"SMS Alerts\","
             "\"kind\":\"service\",\"driver\":\"textbelt\",\"poll_interval_s\":3600,"
             "\"textbelt\":{\"key\":\"%s\",\"max_per_hour\":%d}}",
             key, max_per_hour);
    err[0] = '\0';
    return g_ops->open(spec, err, errsz);
}

static void test_config_rules(void)
{
    char err[256];
    void *c;

    c = open_ctx(GOOD_KEY, 101, err, sizeof(err));
    CHECK(!c && strstr(err, "max_per_hour"), "a cap over 100 is refused");
    c = open_ctx("has space", 5, err, sizeof(err));
    CHECK(!c && strstr(err, "textbelt.key"), "a key with spaces is refused");
}

static void test_main_flow(void)
{
    char err[256], raw[4096];
    void *c;
    cJSON *s, *r;
    int rc;

    c = open_ctx(GOOD_KEY, 5, err, sizeof(err));
    CHECK(c != NULL, "open");
    if (!c)
        return;
    CHECK(g_ops->caps(c) & MF_CAP_NOTIFY, "advertises MF_CAP_NOTIFY");

    /* The credit check brings it online. */
    CHECK(run_until_online(c, 5.0, 1), "online after the credit check");
    CHECK(logged("GET /quota/" GOOD_KEY) == 1, "checked the credits once");
    r = reading(c, raw, sizeof(raw));
    CHECK(r && num(r, "credits_remaining") == 40 && cJSON_IsTrue(
          cJSON_GetObjectItemCaseSensitive(r, "ready")) &&
          strcmp(str(r, "channel"), "sms") == 0, "reading: 40 credits, ready, sms");
    CHECK(!strstr(raw, GOOD_KEY), "the reading never holds the key");
    cJSON_Delete(r);
    s = settings(c);
    CHECK(s && strcmp(str(s, "textbelt.key"), "********4567") == 0 &&
          num(s, "textbelt.credits") == 40 &&
          strcmp(str(s, "textbelt.last_text"), "none yet") == 0,
          "settings: the key masked, credits, no text yet");
    cJSON_Delete(s);

    /* A test text. */
    rc = g_ops->action(c, "test", "{\"to\":\"(555) 111-2222\"}", err, sizeof(err));
    CHECK(rc == MF_OK, "test accepted");
    s = settings(c);
    CHECK(s && strcmp(str(cJSON_GetObjectItemCaseSensitive(s, "_action"), "state"),
                      "running") == 0, "test running");
    cJSON_Delete(s);
    s = run_until_test_done(c, 5.0);
    CHECK(s && strcmp(str(cJSON_GetObjectItemCaseSensitive(s, "_action"), "state"),
                      "done") == 0 &&
          strcmp(str(cJSON_GetObjectItemCaseSensitive(s, "_action"), "text"),
                 "Sent to ***2222. 39 credits left.") == 0, "test done, credits");
    cJSON_Delete(s);
    CHECK(logged("POST /text phone=5551112222&message=Moon%20Flare%20test%20from"
                 "%20SMS%20Alerts%3A%20SMS%20alerts%20work.&key=" GOOD_KEY) == 1,
          "the test text as Textbelt gets it");
    rc = g_ops->action(c, "test", "{\"to\":\"12345\"}", err, sizeof(err));
    CHECK(rc == MF_ERR_INVAL && strstr(err, "not a phone number: 12345"),
          "a test to a bad number is refused");

    /* notify: a text to each number in "to", a second apart.  The module
     * keeps no recipients of its own. */
    rc = g_ops->action(c, "notify", "{\"message\":\"Battery low\"}", err, sizeof(err));
    CHECK(rc == MF_ERR_INVAL && strstr(err, "needs \"to\""), "notify needs \"to\"");
    rc = g_ops->action(c, "notify", "{\"message\":\"x\",\"to\":[]}", err, sizeof(err));
    CHECK(rc == MF_ERR_INVAL && strstr(err, "at least one"), "an empty \"to\"");
    rc = g_ops->action(c, "notify", "{\"title\":\"Moon Flare\",\"message\":"
                       "\"Battery low: 19%\",\"to\":\"555-883-8530, +44 7700 900123\"}",
                       err, sizeof(err));
    CHECK(rc == MF_OK, "notify accepted");
    run_for(c, 2.6);
    CHECK(logged("phone=5558838530&message=Moon%20Flare%3A%20Battery%20low%3A%2019%25")
          == 1 && logged("phone=%2B447700900123&message=Moon%20Flare%3A%20Battery") == 1,
          "notify texted both numbers");
    r = reading(c, raw, sizeof(raw));
    CHECK(r && num(r, "texts_sent") == 3 && num(r, "credits_remaining") == 37 &&
          num(r, "texts_queued") == 0, "3 sent, 37 credits");
    CHECK(!strstr(raw, "5558838530") && !strstr(raw, "447700900123"),
          "the reading holds no full number");
    cJSON_Delete(r);

    /* A send Textbelt refuses. */
    rc = g_ops->action(c, "notify", "{\"message\":\"x\",\"to\":\"555-000-0000\"}",
                       err, sizeof(err));
    CHECK(rc == MF_OK, "notify to one number");
    run_for(c, 1.5);
    r = reading(c, NULL, 0);
    CHECK(r && num(r, "texts_failed") == 1 &&
          strcmp(str(cJSON_GetObjectItemCaseSensitive(r, "last_text"), "error"),
                 "Invalid phone number") == 0, "a refused text is reported");
    cJSON_Delete(r);
    rc = g_ops->action(c, "notify", "{\"message\":\"  \"}", err, sizeof(err));
    CHECK(rc == MF_ERR_INVAL, "notify needs a message");
    rc = g_ops->action(c, "notify", "{\"message\":\"x\",\"to\":[\"5551112222\",7]}",
                       err, sizeof(err));
    CHECK(rc == MF_ERR_INVAL, "notify refuses a bad list");

    /* The cap: 4 texts so far this hour, 5 allowed. */
    rc = g_ops->action(c, "notify", "{\"message\":\"x\",\"to\":[\"5558838530\","
                       "\"+447700900123\"]}", err, sizeof(err));
    CHECK(rc == MF_ERR_BUSY && strstr(err, "limit of 5"), "the cap holds (4 + 2 > 5)");
    rc = g_ops->action(c, "notify", "{\"message\":\"y\",\"to\":[\"5551112222\"]}",
                       err, sizeof(err));
    CHECK(rc == MF_OK, "one more fits the cap");
    run_for(c, 1.5);

    /* refresh checks the credits again. */
    CHECK(g_ops->action(c, "refresh", "{}", err, sizeof(err)) == MF_OK, "refresh");
    run_for(c, 0.5);
    CHECK(logged("GET /quota/" GOOD_KEY) == 2, "refresh checked the credits");

    /* Settings: a mask sent back changes nothing; bad values are refused. */
    rc = g_ops->put_settings(c, "{\"textbelt.key\":\"********4567\"}", err, sizeof(err));
    run_for(c, 0.3);
    CHECK(rc == MF_OK && logged("GET /quota/") == 2, "a mask sent back keeps the key");
    rc = g_ops->put_settings(c, "{\"textbelt.to\":\"5558838530\"}", err, sizeof(err));
    s = settings(c);
    CHECK(rc == MF_OK && s && !cJSON_GetObjectItemCaseSensitive(s, "textbelt.to"),
          "no recipients are kept");
    cJSON_Delete(s);
    rc = g_ops->put_settings(c, "{\"textbelt.max_per_hour\":2.5}", err, sizeof(err));
    CHECK(rc == MF_ERR_INVAL, "a fractional cap is refused");

    /* A new key is checked at once; this one has no credits left. */
    rc = g_ops->put_settings(c, "{\"textbelt.key\":\"" NEW_KEY "\"}", err, sizeof(err));
    CHECK(rc == MF_OK, "new key");
    CHECK(run_until_online(c, 3.0, 0), "offline");
    run_for(c, 0.5);
    CHECK(logged("GET /quota/" NEW_KEY) == 1 && g_ops->last_error(c) &&
          strstr(g_ops->last_error(c), "no Textbelt credits left"),
          "no credits: offline, saying so");

    /* A key Textbelt does not know. */
    rc = g_ops->put_settings(c, "{\"textbelt.key\":\"nosuchkey99999\"}", err, sizeof(err));
    run_for(c, 0.5);
    CHECK(g_ops->last_error(c) && strstr(g_ops->last_error(c), "does not know"),
          "an unknown key: offline, saying so");
    rc = g_ops->action(c, "notify", "{\"message\":\"x\",\"to\":\"5551112222\"}",
                       err, sizeof(err));
    CHECK(rc == MF_ERR_OFFLINE, "no sends with an unknown key");

    /* No key at all. */
    rc = g_ops->put_settings(c, "{\"textbelt.key\":\"\"}", err, sizeof(err));
    run_for(c, 0.2);
    CHECK(g_ops->last_error(c) && strstr(g_ops->last_error(c), "no API key"),
          "no key: offline, saying so");
    rc = g_ops->action(c, "test", "{\"to\":\"5551112222\"}", err, sizeof(err));
    CHECK(rc == MF_ERR_INVAL && strstr(err, "API key"), "no test without a key");
    g_ops->close(c);
}

static void test_unreachable(void)
{
    char err[256];
    void *c;

    setenv("MF_TEXTBELT_URL", "http://127.0.0.1:9", 1);
    c = open_ctx(GOOD_KEY, 5, err, sizeof(err));
    CHECK(c != NULL, "open (unreachable)");
    if (!c)
        return;
    run_for(c, 1.0);
    CHECK(g_ops->last_error(c) && strstr(g_ops->last_error(c), "can't reach Textbelt"),
          "unreachable: offline, saying so");
    g_ops->close(c);
}

int main(int argc, char **argv)
{
    void *dl;
    size_t (*entries)(const mf_plugin_ops_t **);
    char url[64];
    int port = 0, fd;
    pid_t srv;

    if (argc < 2)
    {
        fprintf(stderr, "usage: %s libmf_service_textbelt.so\n", argv[0]);
        return 2;
    }
    snprintf(g_log, sizeof(g_log), "/tmp/mf-textbelt-XXXXXX");
    fd = mkstemp(g_log);
    if (fd < 0)
        return 1;
    close(fd);
    srv = start_server(&port);
    if (srv <= 0)
    {
        fprintf(stderr, "FAIL: fake Textbelt: %s\n", strerror(errno));
        return 1;
    }
    snprintf(url, sizeof(url), "http://127.0.0.1:%d", port);
    setenv("MF_TEXTBELT_URL", url, 1);
    dl = dlopen(argv[1], RTLD_NOW);
    if (!dl)
    {
        fprintf(stderr, "FAIL: dlopen %s: %s\n", argv[1], dlerror());
        kill(srv, SIGTERM);
        return 1;
    }
    *(void **)&entries = dlsym(dl, "mf_plugin_entries");
    if (!entries || entries(&g_ops) != 1)
    {
        fprintf(stderr, "FAIL: mf_plugin_entries\n");
        kill(srv, SIGTERM);
        return 1;
    }
    CHECK(strcmp(g_ops->kind, "service") == 0 && strcmp(g_ops->driver, "textbelt") == 0,
          "service/textbelt");
    {
        cJSON *d = cJSON_Parse(g_ops->describe());
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(d, "notify");

        CHECK(n && strcmp(str(n, "channel"), "sms") == 0 &&
              strcmp(str(n, "action"), "notify") == 0, "describe() has the notify block");
        cJSON_Delete(d);
    }
    test_config_rules();
    test_main_flow();
    test_unreachable();
    kill(srv, SIGTERM);
    waitpid(srv, NULL, 0);
    unlink(g_log);
    dlclose(dl);
    if (g_fail)
    {
        fprintf(stderr, "%d failure(s)\n", g_fail);
        return 1;
    }
    printf("test_textbelt_plugin: ok\n");
    return 0;
}
