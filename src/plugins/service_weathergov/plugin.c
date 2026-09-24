/* Weather service: api.weather.gov (US National Weather Service).
 *
 * kind "service", driver "weathergov".  Needs a location and a contact
 * address (weather.gov asks for one in the User-Agent).  Requests run on
 * libcurl's multi interface, driven from the daemon's select() loop through
 * prepare_fds() and step(), so nothing blocks:
 *
 *   /points/{lat},{lon}           -> forecast URL, stations URL, place name
 *   {stations}?limit=1            -> nearest observation station
 *   /stations/{id}/observations/latest   every poll_interval_s (>= 300 s)
 *   {forecast}                    every 30 minutes
 *
 * The reading is provider-neutral, so the dashboard's Info panel works with
 * any weather service that publishes the same "weather" object:
 *   {"provider": "weather.gov",
 *    "weather": {"temp_f", "conditions", "humidity_pct", "wind_mph",
 *                "wind_dir", "station", "place", "observed_local",
 *                "forecast": [{"name", "temp_f", "short"}, ...]}}
 */

#include "mf_plugin.h"

#include <cJSON.h>
#include <curl/curl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WX_API          "https://api.weather.gov"
#define WX_TIMEOUT_S    20L
#define WX_OBS_MIN_S    300.0
#define WX_FORECAST_S   1800.0
#define WX_POINTS_S     86400.0
#define WX_RETRY_S      120.0
#define WX_STALE_S      (3.0 * 3600.0)
#define WX_BODY_MAX     (512 * 1024)
#define WX_NFC          2

typedef enum { WX_NONE, WX_POINTS, WX_STATION, WX_OBS, WX_FORECAST } wx_req_t;

typedef struct {
    char   name[24];
    double temp_f;
    char   shortf[48];
} wx_period_t;

typedef struct {
    CURLM             *multi;
    CURL              *easy;
    struct curl_slist *hdrs;
    wx_req_t           inflight;

    char              *body;
    size_t             len;
    size_t             cap;

    double             lat;
    double             lon;
    double             obs_every;
    char               ua[192];

    char               forecast_url[256];
    char               stations_url[256];
    char               station[16];
    char               place[64];
    double             next_points;
    double             next_station;
    double             next_obs;
    double             next_fc;

    int                have_obs;
    double             obs_at;          /* mono time of the last observation */
    double             temp_f;          /* NAN when the station omits it */
    double             hum;
    double             wind_mph;
    char               wind_dir[4];
    char               cond[48];
    char               observed_local[8];
    wx_period_t        fc[WX_NFC];
    int                nfc;

    int                changed;
    char               err[128];
} wx_ctx_t;

static int g_curl_ready;

static double mono_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void set_err(wx_ctx_t *c, const char *what, const char *detail)
{
    snprintf(c->err, sizeof(c->err), "%s%s%s", what,
             detail && detail[0] ? ": " : "", detail ? detail : "");
}

/* ---- settings ------------------------------------------------------ */

static const char *wx_describe(void)
{
    return
        "{\"fields\":["
        "{\"key\":\"poll_interval_s\",\"label\":\"Update Every\",\"hint\":\"(>=300 sec)\",\"type\":\"number\",\"default\":600},"
        "{\"key\":\"capture_interval_s\",\"label\":\"Capture Interval\",\"hint\":\"(Sec 0=off)\",\"type\":\"number\",\"default\":0},"
        "{\"key\":\"weather.lat\",\"label\":\"Latitude\",\"hint\":\"(32.78)\",\"type\":\"number\",\"required\":true},"
        "{\"key\":\"weather.lon\",\"label\":\"Longitude\",\"hint\":\"(-96.80)\",\"type\":\"number\",\"required\":true},"
        "{\"key\":\"weather.contact\",\"label\":\"Contact Email\",\"hint\":\"(for NWS)\",\"type\":\"string\",\"required\":true}"
        "]}";
}

static double json_num(const cJSON *o, const char *key, double dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);

    if (cJSON_IsNumber(v))
        return v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring[0])
        return atof(v->valuestring);
    return dflt;
}

/* ---- HTTP (libcurl multi) ------------------------------------------ */

static size_t on_body(char *p, size_t sz, size_t n, void *arg)
{
    wx_ctx_t *c = arg;
    size_t add = sz * n;

    if (c->len + add + 1 > WX_BODY_MAX)
        return 0;                       /* abort: absurdly large reply */
    if (c->len + add + 1 > c->cap)
    {
        size_t ncap = c->cap ? c->cap * 2 : 16384;
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

static int start_get(wx_ctx_t *c, wx_req_t what, const char *url)
{
    c->easy = curl_easy_init();
    if (!c->easy)
    {
        set_err(c, "curl_easy_init failed", NULL);
        return -1;
    }
    c->len = 0;
    if (c->body)
        c->body[0] = '\0';
    curl_easy_setopt(c->easy, CURLOPT_URL, url);
    curl_easy_setopt(c->easy, CURLOPT_USERAGENT, c->ua);
    curl_easy_setopt(c->easy, CURLOPT_HTTPHEADER, c->hdrs);
    curl_easy_setopt(c->easy, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(c->easy, CURLOPT_WRITEDATA, c);
    curl_easy_setopt(c->easy, CURLOPT_TIMEOUT, WX_TIMEOUT_S);
    curl_easy_setopt(c->easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c->easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c->easy, CURLOPT_ACCEPT_ENCODING, "");
    if (curl_multi_add_handle(c->multi, c->easy) != CURLM_OK)
    {
        curl_easy_cleanup(c->easy);
        c->easy = NULL;
        set_err(c, "curl_multi_add_handle failed", NULL);
        return -1;
    }
    c->inflight = what;
    return 0;
}

static void end_request(wx_ctx_t *c)
{
    if (c->easy)
    {
        curl_multi_remove_handle(c->multi, c->easy);
        curl_easy_cleanup(c->easy);
        c->easy = NULL;
    }
    c->inflight = WX_NONE;
}

/* ---- parsing ------------------------------------------------------- */

static const char *compass(double deg)
{
    static const char *const pts[16] = {
        "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
        "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"
    };
    int i = (int)floor(fmod(deg + 11.25, 360.0) / 22.5);

    return pts[(i % 16 + 16) % 16];
}

/* "2026-09-24T05:05:00+00:00" -> local "00:05". */
static void iso_to_local_hhmm(const char *iso, char *out, size_t cap)
{
    struct tm tm;
    time_t t;

    out[0] = '\0';
    memset(&tm, 0, sizeof(tm));
    if (!iso || !strptime(iso, "%Y-%m-%dT%H:%M:%S", &tm))
        return;
    t = timegm(&tm);                    /* weather.gov stamps are UTC */
    if (localtime_r(&t, &tm))
        strftime(out, cap, "%H:%M", &tm);
}

static const cJSON *props(const cJSON *root)
{
    return cJSON_GetObjectItemCaseSensitive(root, "properties");
}

static double qv(const cJSON *p, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(p, key), "value");

    return cJSON_IsNumber(v) ? v->valuedouble : NAN;
}

static int parse_points(wx_ctx_t *c, const cJSON *root)
{
    const cJSON *p = props(root);
    const cJSON *fc = cJSON_GetObjectItemCaseSensitive(p, "forecast");
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(p, "observationStations");
    const cJSON *rl = props(cJSON_GetObjectItemCaseSensitive(p, "relativeLocation"));
    const cJSON *city = cJSON_GetObjectItemCaseSensitive(rl, "city");
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(rl, "state");

    if (!cJSON_IsString(fc) || !cJSON_IsString(st))
        return -1;
    snprintf(c->forecast_url, sizeof(c->forecast_url), "%s", fc->valuestring);
    snprintf(c->stations_url, sizeof(c->stations_url), "%s", st->valuestring);
    if (cJSON_IsString(city) && cJSON_IsString(state))
        snprintf(c->place, sizeof(c->place), "%s, %s",
                 city->valuestring, state->valuestring);
    c->station[0] = '\0';               /* re-pick the nearest station */
    return 0;
}

static int parse_station(wx_ctx_t *c, const cJSON *root)
{
    const cJSON *f = cJSON_GetArrayItem(
        cJSON_GetObjectItemCaseSensitive(root, "features"), 0);
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(props(f),
                                                       "stationIdentifier");

    if (!cJSON_IsString(id))
        return -1;
    snprintf(c->station, sizeof(c->station), "%s", id->valuestring);
    return 0;
}

static int parse_obs(wx_ctx_t *c, const cJSON *root)
{
    const cJSON *p = props(root);
    const cJSON *txt = cJSON_GetObjectItemCaseSensitive(p, "textDescription");
    const cJSON *ts = cJSON_GetObjectItemCaseSensitive(p, "timestamp");
    double tc = qv(p, "temperature");
    double ws = qv(p, "windSpeed");
    double wd = qv(p, "windDirection");

    if (!p)
        return -1;
    c->temp_f = isnan(tc) ? NAN : tc * 9.0 / 5.0 + 32.0;
    c->hum = qv(p, "relativeHumidity");
    c->wind_mph = isnan(ws) ? NAN : ws / 1.609344;
    snprintf(c->wind_dir, sizeof(c->wind_dir), "%s",
             isnan(wd) ? "" : compass(wd));
    snprintf(c->cond, sizeof(c->cond), "%s",
             cJSON_IsString(txt) ? txt->valuestring : "");
    iso_to_local_hhmm(cJSON_IsString(ts) ? ts->valuestring : NULL,
                      c->observed_local, sizeof(c->observed_local));
    c->have_obs = 1;
    c->obs_at = mono_now();
    return 0;
}

static int parse_forecast(wx_ctx_t *c, const cJSON *root)
{
    const cJSON *per = cJSON_GetObjectItemCaseSensitive(props(root), "periods");
    const cJSON *it;
    int n = 0;

    if (!cJSON_IsArray(per))
        return -1;
    cJSON_ArrayForEach(it, per)
    {
        const cJSON *nm = cJSON_GetObjectItemCaseSensitive(it, "name");
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(it, "temperature");
        const cJSON *sf = cJSON_GetObjectItemCaseSensitive(it, "shortForecast");

        if (n >= WX_NFC)
            break;
        snprintf(c->fc[n].name, sizeof(c->fc[n].name), "%s",
                 cJSON_IsString(nm) ? nm->valuestring : "");
        c->fc[n].temp_f = cJSON_IsNumber(t) ? t->valuedouble : NAN;
        snprintf(c->fc[n].shortf, sizeof(c->fc[n].shortf), "%s",
                 cJSON_IsString(sf) ? sf->valuestring : "");
        n++;
    }
    c->nfc = n;
    return 0;
}

/* A request finished: parse it and schedule what comes next. */
static void finish(wx_ctx_t *c, CURLcode rc)
{
    wx_req_t what = c->inflight;
    long code = 0;
    double now = mono_now();
    cJSON *root = NULL;
    int ok = -1;

    curl_easy_getinfo(c->easy, CURLINFO_RESPONSE_CODE, &code);
    end_request(c);
    if (rc != CURLE_OK)
        set_err(c, "weather.gov request failed", curl_easy_strerror(rc));
    else if (code != 200)
    {
        char buf[32];

        snprintf(buf, sizeof(buf), "HTTP %ld", code);
        set_err(c, "weather.gov", buf);
    }
    else if (!(root = cJSON_Parse(c->body)))
        set_err(c, "weather.gov reply is not JSON", NULL);
    else if (what == WX_POINTS)
        ok = parse_points(c, root);
    else if (what == WX_STATION)
        ok = parse_station(c, root);
    else if (what == WX_OBS)
        ok = parse_obs(c, root);
    else if (what == WX_FORECAST)
        ok = parse_forecast(c, root);
    cJSON_Delete(root);

    if (ok == 0)
    {
        c->err[0] = '\0';
        if (what == WX_POINTS)
            c->next_points = now + WX_POINTS_S;
        else if (what == WX_OBS)
            c->next_obs = now + c->obs_every;
        else if (what == WX_FORECAST)
            c->next_fc = now + WX_FORECAST_S;
        c->changed = 1;
        return;
    }
    if (!c->err[0])
        set_err(c, "weather.gov reply not understood", NULL);
    /* Retry this step later; keep showing the last good data meanwhile. */
    if (what == WX_POINTS)
        c->next_points = now + WX_RETRY_S;
    else if (what == WX_STATION)
        c->next_station = now + WX_RETRY_S;
    else if (what == WX_OBS)
        c->next_obs = now + WX_RETRY_S;
    else if (what == WX_FORECAST)
        c->next_fc = now + WX_RETRY_S;
}

static void start_next(wx_ctx_t *c)
{
    char url[320];
    double now = mono_now();

    if (!c->forecast_url[0] || now >= c->next_points)
    {
        if (now < c->next_points)
            return;                     /* waiting out a failed lookup */
        snprintf(url, sizeof(url), WX_API "/points/%.4f,%.4f", c->lat, c->lon);
        (void)start_get(c, WX_POINTS, url);
    }
    else if (!c->station[0])
    {
        if (now < c->next_station)
            return;
        snprintf(url, sizeof(url), "%s?limit=1", c->stations_url);
        (void)start_get(c, WX_STATION, url);
    }
    else if (now >= c->next_obs)
    {
        snprintf(url, sizeof(url), WX_API "/stations/%s/observations/latest",
                 c->station);
        (void)start_get(c, WX_OBS, url);
    }
    else if (now >= c->next_fc)
        (void)start_get(c, WX_FORECAST, c->forecast_url);
}

/* ---- plugin ops ---------------------------------------------------- */

static void *wx_open(const char *spec_json, char *err, size_t errsz)
{
    cJSON *spec = spec_json ? cJSON_Parse(spec_json) : NULL;
    const cJSON *w = cJSON_GetObjectItemCaseSensitive(spec, "weather");
    const cJSON *contact = cJSON_GetObjectItemCaseSensitive(w, "contact");
    wx_ctx_t *c;

    if (!cJSON_IsObject(w) || !cJSON_GetObjectItemCaseSensitive(w, "lat") ||
        !cJSON_GetObjectItemCaseSensitive(w, "lon"))
    {
        if (err && errsz)
            snprintf(err, errsz, "weather.lat and weather.lon are required");
        cJSON_Delete(spec);
        return NULL;
    }
    if (!cJSON_IsString(contact) || !contact->valuestring[0])
    {
        if (err && errsz)
            snprintf(err, errsz, "weather.contact (an email) is required");
        cJSON_Delete(spec);
        return NULL;
    }
    if (!g_curl_ready)
    {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        {
            if (err && errsz)
                snprintf(err, errsz, "curl_global_init failed");
            cJSON_Delete(spec);
            return NULL;
        }
        g_curl_ready = 1;
    }
    c = calloc(1, sizeof(*c));
    if (!c)
    {
        cJSON_Delete(spec);
        return NULL;
    }
    c->lat = json_num(w, "lat", 0.0);
    c->lon = json_num(w, "lon", 0.0);
    c->obs_every = json_num(spec, "poll_interval_s", 600.0);
    if (c->obs_every < WX_OBS_MIN_S)
        c->obs_every = WX_OBS_MIN_S;
    snprintf(c->ua, sizeof(c->ua), "moonflare-weathergov/0.1 (%s)",
             contact->valuestring);
    c->temp_f = c->hum = c->wind_mph = NAN;
    cJSON_Delete(spec);

    c->multi = curl_multi_init();
    c->hdrs = curl_slist_append(NULL, "Accept: application/geo+json");
    if (!c->multi || !c->hdrs)
    {
        if (c->multi)
            curl_multi_cleanup(c->multi);
        curl_slist_free_all(c->hdrs);
        free(c);
        if (err && errsz)
            snprintf(err, errsz, "curl setup failed");
        return NULL;
    }
    snprintf(c->err, sizeof(c->err), "waiting for weather.gov");
    return c;
}

static void wx_close(void *ctx)
{
    wx_ctx_t *c = ctx;

    if (!c)
        return;
    end_request(c);
    curl_multi_cleanup(c->multi);
    curl_slist_free_all(c->hdrs);
    free(c->body);
    free(c);
}

/* All I/O goes through prepare_fds; there is no single fd. */
static int wx_fd(void *ctx)
{
    (void)ctx;
    return -1;
}

static unsigned wx_select_mask(void *ctx)
{
    (void)ctx;
    return 0;
}

static void wx_prepare_fds(void *ctx, fd_set *r, fd_set *w, int *maxfd)
{
    wx_ctx_t *c = ctx;
    fd_set ex;
    int mx = -1;

    if (!c || !c->easy)
        return;
    FD_ZERO(&ex);
    if (curl_multi_fdset(c->multi, r, w, &ex, &mx) == CURLM_OK &&
        maxfd && mx > *maxfd)
        *maxfd = mx;
}

static mf_step_t wx_step(void *ctx)
{
    wx_ctx_t *c = ctx;
    int running = 0, left = 0;
    CURLMsg *msg;

    if (!c)
        return MF_STEP_ERROR;
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
    if (!c->easy)
        start_next(c);
    if (c->have_obs && mono_now() - c->obs_at > WX_STALE_S)
        return MF_STEP_ERROR;           /* hours without an observation */
    if (c->changed)
    {
        c->changed = 0;
        return MF_STEP_UPDATED;
    }
    return MF_STEP_IDLE;
}

static unsigned wx_caps(void *ctx)
{
    (void)ctx;
    return MF_CAP_READ;
}

static const char *wx_last_error(void *ctx)
{
    wx_ctx_t *c = ctx;

    return c && c->err[0] ? c->err : NULL;
}

static void add_num_or_null(cJSON *o, const char *key, double v)
{
    if (isnan(v))
        cJSON_AddNullToObject(o, key);
    else
        cJSON_AddNumberToObject(o, key, round(v * 10.0) / 10.0);
}

static int wx_get_reading(void *ctx, char *json, size_t cap)
{
    wx_ctx_t *c = ctx;
    cJSON *root, *wx, *fc;
    char *s;
    int i;

    if (!c || !c->have_obs)
        return MF_ERR_OFFLINE;          /* nothing to show yet */
    root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "provider", "weather.gov");
    wx = cJSON_AddObjectToObject(root, "weather");
    add_num_or_null(wx, "temp_f", c->temp_f);
    cJSON_AddStringToObject(wx, "conditions", c->cond);
    add_num_or_null(wx, "humidity_pct", c->hum);
    add_num_or_null(wx, "wind_mph", c->wind_mph);
    cJSON_AddStringToObject(wx, "wind_dir", c->wind_dir);
    cJSON_AddStringToObject(wx, "station", c->station);
    cJSON_AddStringToObject(wx, "place", c->place);
    cJSON_AddStringToObject(wx, "observed_local", c->observed_local);
    fc = cJSON_AddArrayToObject(wx, "forecast");
    for (i = 0; i < c->nfc; i++)
    {
        cJSON *p = cJSON_CreateObject();

        cJSON_AddStringToObject(p, "name", c->fc[i].name);
        add_num_or_null(p, "temp_f", c->fc[i].temp_f);
        cJSON_AddStringToObject(p, "short", c->fc[i].shortf);
        cJSON_AddItemToArray(fc, p);
    }
    s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s || strlen(s) >= cap)
    {
        free(s);
        return MF_ERR_INVAL;
    }
    memcpy(json, s, strlen(s) + 1);
    free(s);
    return MF_OK;
}

static const mf_plugin_ops_t g_ops = {
    .abi = MF_PLUGIN_ABI,
    .ops_size = sizeof(mf_plugin_ops_t),
    .kind = "service",
    .driver = "weathergov",
    .version = "0.1.0",
    .open = wx_open,
    .close = wx_close,
    .fd = wx_fd,
    .select_mask = wx_select_mask,
    .prepare_fds = wx_prepare_fds,
    .step = wx_step,
    .caps = wx_caps,
    .last_error = wx_last_error,
    .get_reading = wx_get_reading,
    .describe = wx_describe,
};

size_t mf_plugin_entries(const mf_plugin_ops_t **out)
{
    if (out)
        *out = &g_ops;
    return 1;
}
