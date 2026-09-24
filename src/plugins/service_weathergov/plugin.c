/* Weather service: api.weather.gov (US National Weather Service).
 *
 * kind "service", driver "weathergov".  Needs a location and a contact
 * address (weather.gov asks for one in the User-Agent).  The location is a
 * ZIP code, looked up offline in the Census ZIP centroid table (zcta.c),
 * or an explicit latitude/longitude.  Every weather.zip_update_days days the
 * plugin asks census.gov whether a newer yearly table exists and installs
 * it in its state directory.  Requests run on
 * libcurl's multi interface, driven from the daemon's select() loop through
 * prepare_fds() and step(), so nothing blocks:
 *
 *   /points/{lat},{lon}           -> forecast URL, stations URL, place name
 *   {stations}?limit=5            -> the nearest observation stations
 *   /stations/{id}/observations/latest   every poll_interval_s (>= 300 s)
 *
 * Observations come from weather.station when set, else the nearest
 * station.  An observation with no temperature or older than two hours is
 * skipped for the next nearest station; if none is good the last good
 * reading stays up.
 *   {forecast}                    every 30 minutes
 *
 * The reading is provider-neutral, so the dashboard's Info panel works with
 * any weather service that publishes the same "weather" object:
 *   {"provider": "weather.gov",
 *    "weather": {"temp_f", "conditions", "icon", "is_day", "humidity_pct",
 *                "wind_mph", "wind_dir", "station", "place",
 *                "observed_local",
 *                "forecast": [{"name", "temp_f", "short", "icon",
 *                              "is_day"}, ...]}}
 * "icon" is a neutral condition key (clear, partly_cloudy, mostly_cloudy,
 * cloudy, wind, rain, showers, thunderstorm, snow, blizzard, sleet,
 * freezing_rain, fog, haze, tornado, hurricane, hot, cold); the dashboard
 * picks the symbol.
 */

#include "mf_plugin.h"
#include "zcta.h"

#include <cJSON.h>
#include <ctype.h>
#include <curl/curl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
#define WX_ZCTA_URL     "https://www2.census.gov/geo/docs/maps-data/data/" \
                        "gazetteer/%d_Gazetteer/%d_Gaz_zcta_national.zip"
#define WX_ZCTA_MAX     (8 * 1024 * 1024)
#define WX_ZCTA_RETRY_S 3600
#define WX_DAY_S        86400
#define WX_NST          5               /* nearby stations to fall back on */
#define WX_OBS_FRESH_S  (2 * 3600)      /* older observations are skipped */

typedef enum {
    WX_NONE, WX_POINTS, WX_STATION, WX_OBS, WX_FORECAST,
    WX_ZCTA_HEAD, WX_ZCTA_GET      /* is there a newer ZIP table? fetch it */
} wx_req_t;

typedef struct {
    char   name[24];
    double temp_f;
    char   shortf[48];
    char   icon[16];
    int    is_day;
} wx_period_t;

/* One observation, as shown in the Info panel. */
typedef struct {
    double             temp_f;          /* NAN when the station omits it */
    double             hum;
    double             wind_mph;
    char               wind_dir[4];
    char               cond[48];
    char               icon[16];
    int                is_day;
    char               observed_local[8];
    char               station[16];
    long               observed_epoch;
} wx_obs_t;

typedef struct {
    CURLM             *multi;
    CURL              *easy;
    struct curl_slist *hdrs;
    wx_req_t           inflight;

    char              *body;
    size_t             len;
    size_t             cap;
    size_t             body_max;

    char               zip[8];          /* "" when lat/lon were given */
    int                have_loc;
    int                zip_missing;     /* ZIP not in the table: wait for an update */
    double             update_days;     /* 0 = never check for a newer table */
    long               next_zcta;       /* wall clock (epoch) of the next check */
    int                zcta_year;       /* year being checked/fetched */
    char               zcta_state[256];
    char               zcta_meta[256];
    char               zcta_base[256];

    double             lat;
    double             lon;
    int                ll_given;        /* lat/lon set by the user, not the ZIP */
    double             obs_every;
    char               contact[128];
    char               ua[192];

    char               forecast_url[256];
    char               stations_url[256];
    char               want_station[16];  /* weather.station; "" = nearest */
    char               stations[WX_NST + 1][16];  /* candidates, in order */
    int                nst;             /* 0 = list not fetched yet */
    int                st_try;          /* candidate being tried */
    char               place[64];
    double             next_points;
    double             next_station;
    double             next_obs;
    double             next_fc;

    int                have_obs;
    double             obs_at;          /* mono time of the last observation */
    wx_obs_t           obs;             /* the reading on display */
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
        "{\"key\":\"weather.zip\",\"label\":\"ZIP Code\",\"hint\":\"(5 digits)\",\"type\":\"string\"},"
        "{\"key\":\"weather.station\",\"label\":\"Station\",\"hint\":\"(blank=near)\",\"type\":\"string\"},"
        "{\"key\":\"weather.zip_update_days\",\"label\":\"ZIP Table Check\",\"hint\":\"(days,0=off)\",\"type\":\"number\",\"default\":7},"
        "{\"key\":\"weather.lat\",\"label\":\"Latitude\",\"hint\":\"(optional)\",\"type\":\"number\"},"
        "{\"key\":\"weather.lon\",\"label\":\"Longitude\",\"hint\":\"(optional)\",\"type\":\"number\"},"
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

    if (c->len + add + 1 > c->body_max)
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
    c->body_max = what == WX_ZCTA_GET ? WX_ZCTA_MAX : WX_BODY_MAX;
    if (what == WX_ZCTA_HEAD)
        curl_easy_setopt(c->easy, CURLOPT_NOBODY, 1L);
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

/* weather.gov icon codes -> the neutral keys every weather service uses. */
static const char *icon_key(const char *code)
{
    static const char *const map[][2] = {
        { "skc", "clear" }, { "few", "clear" }, { "sct", "partly_cloudy" },
        { "bkn", "mostly_cloudy" }, { "ovc", "cloudy" },
        { "wind_skc", "wind" }, { "wind_few", "wind" }, { "wind_sct", "wind" },
        { "wind_bkn", "wind" }, { "wind_ovc", "wind" },
        { "rain", "rain" }, { "rain_showers", "showers" },
        { "rain_showers_hi", "showers" }, { "tsra", "thunderstorm" },
        { "tsra_sct", "thunderstorm" }, { "tsra_hi", "thunderstorm" },
        { "snow", "snow" }, { "blizzard", "blizzard" },
        { "rain_snow", "sleet" }, { "rain_sleet", "sleet" },
        { "snow_sleet", "sleet" }, { "sleet", "sleet" },
        { "fzra", "freezing_rain" }, { "rain_fzra", "freezing_rain" },
        { "snow_fzra", "freezing_rain" }, { "fog", "fog" },
        { "haze", "haze" }, { "smoke", "haze" }, { "dust", "haze" },
        { "tornado", "tornado" }, { "hurricane", "hurricane" },
        { "tropical_storm", "hurricane" }, { "hot", "hot" }, { "cold", "cold" },
    };
    size_t i;

    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (strcmp(code, map[i][0]) == 0)
            return map[i][1];
    return "";
}

/* When a station sends no icon, read the description instead. */
static const char *icon_from_text(const char *t)
{
    static const char *const map[][2] = {
        { "thunder", "thunderstorm" }, { "tornado", "tornado" },
        { "blizzard", "blizzard" }, { "freezing", "freezing_rain" },
        { "sleet", "sleet" }, { "snow", "snow" }, { "shower", "showers" },
        { "rain", "rain" }, { "drizzle", "rain" }, { "fog", "fog" },
        { "mist", "fog" }, { "haze", "haze" }, { "smoke", "haze" },
        { "dust", "haze" }, { "partly", "partly_cloudy" },
        { "mostly cloudy", "mostly_cloudy" }, { "mostly sunny", "partly_cloudy" },
        { "overcast", "cloudy" }, { "cloudy", "cloudy" }, { "wind", "wind" },
        { "breezy", "wind" }, { "clear", "clear" }, { "sunny", "clear" },
        { "fair", "clear" },
    };
    char low[64];
    size_t i;

    for (i = 0; t[i] && i < sizeof(low) - 1; i++)
        low[i] = (char)tolower((unsigned char)t[i]);
    low[i] = '\0';
    for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (strstr(low, map[i][0]))
            return map[i][1];
    return "";
}

/* ".../icons/land/night/tsra_hi,40/rain,60?size=medium" -> key of the
 * first code ("thunderstorm") and whether it is the day icon. */
static void parse_icon(const char *url, const char *text, char *key,
                       size_t cap, int *is_day)
{
    const char *p = url ? strstr(url, "/land/") : NULL;
    char code[32];
    size_t n;

    key[0] = '\0';
    if (p)
    {
        p += 6;
        if (is_day)
            *is_day = strncmp(p, "night/", 6) != 0;
        p = strchr(p, '/');
        if (p)
        {
            p++;
            n = strcspn(p, ",/?");
            if (n >= sizeof(code))
                n = sizeof(code) - 1;
            memcpy(code, p, n);
            code[n] = '\0';
            snprintf(key, cap, "%s", icon_key(code));
        }
    }
    if (!key[0] && text)
        snprintf(key, cap, "%s", icon_from_text(text));
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
    c->nst = 0;                         /* re-fetch the nearby stations */
    return 0;
}

/* Candidates: the chosen station first (if any), then the nearby ones in
 * weather.gov's order (nearest first). */
static int parse_station(wx_ctx_t *c, const cJSON *root)
{
    const cJSON *f;
    int n = 0;

    if (c->want_station[0])
        snprintf(c->stations[n++], sizeof(c->stations[0]), "%s", c->want_station);
    cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(root, "features"))
    {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(props(f),
                                                           "stationIdentifier");

        if (n > WX_NST || !cJSON_IsString(id) ||
            strcasecmp(id->valuestring, c->want_station) == 0)
            continue;
        snprintf(c->stations[n++], sizeof(c->stations[0]), "%s", id->valuestring);
    }
    if (n == 0)
        return -1;
    c->nst = n;
    c->st_try = 0;
    return 0;
}

/* Parse into *o.  Returns 1 if good (has a temperature and is fresh),
 * 0 if usable but poor, -1 if not an observation. */
static int parse_obs(wx_ctx_t *c, const cJSON *root, wx_obs_t *o)
{
    const cJSON *p = props(root);
    const cJSON *txt = cJSON_GetObjectItemCaseSensitive(p, "textDescription");
    const cJSON *ts = cJSON_GetObjectItemCaseSensitive(p, "timestamp");
    const cJSON *ic = cJSON_GetObjectItemCaseSensitive(p, "icon");
    double tc = qv(p, "temperature");
    double ws = qv(p, "windSpeed");
    double wd = qv(p, "windDirection");
    time_t now = time(NULL);
    struct tm tm;

    if (!p)
        return -1;
    memset(o, 0, sizeof(*o));
    o->temp_f = isnan(tc) ? NAN : tc * 9.0 / 5.0 + 32.0;
    o->hum = qv(p, "relativeHumidity");
    o->wind_mph = isnan(ws) ? NAN : ws / 1.609344;
    snprintf(o->wind_dir, sizeof(o->wind_dir), "%s", isnan(wd) ? "" : compass(wd));
    snprintf(o->cond, sizeof(o->cond), "%s",
             cJSON_IsString(txt) ? txt->valuestring : "");
    localtime_r(&now, &tm);
    o->is_day = tm.tm_hour >= 6 && tm.tm_hour < 19;  /* if the URL won't say */
    parse_icon(cJSON_IsString(ic) ? ic->valuestring : NULL, o->cond,
               o->icon, sizeof(o->icon), &o->is_day);
    iso_to_local_hhmm(cJSON_IsString(ts) ? ts->valuestring : NULL,
                      o->observed_local, sizeof(o->observed_local));
    if (cJSON_IsString(ts))
    {
        struct tm u;

        memset(&u, 0, sizeof(u));
        if (strptime(ts->valuestring, "%Y-%m-%dT%H:%M:%S", &u))
            o->observed_epoch = (long)timegm(&u);
    }
    snprintf(o->station, sizeof(o->station), "%s",
             c->nst ? c->stations[c->st_try] : "");
    return !isnan(o->temp_f) && o->observed_epoch &&
           (long)now - o->observed_epoch < WX_OBS_FRESH_S;
}

/* Keep a good observation; otherwise try the next nearby station.  With no
 * good one anywhere, keep what we have (or take the first ever). */
static int take_obs(wx_ctx_t *c, const cJSON *root)
{
    wx_obs_t o;
    int q = parse_obs(c, root, &o);

    if (q < 0)
        return -1;
    if (q == 0 && c->st_try + 1 < c->nst)
    {
        c->st_try++;
        c->next_obs = 0;                /* ask the next station now */
        return 1;
    }
    if (q == 1 || !c->have_obs)
    {
        c->obs = o;
        c->have_obs = 1;
        c->obs_at = mono_now();
    }
    c->st_try = 0;
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
        const cJSON *ic = cJSON_GetObjectItemCaseSensitive(it, "icon");
        const cJSON *dy = cJSON_GetObjectItemCaseSensitive(it, "isDaytime");

        if (n >= WX_NFC)
            break;
        snprintf(c->fc[n].name, sizeof(c->fc[n].name), "%s",
                 cJSON_IsString(nm) ? nm->valuestring : "");
        c->fc[n].temp_f = cJSON_IsNumber(t) ? t->valuedouble : NAN;
        snprintf(c->fc[n].shortf, sizeof(c->fc[n].shortf), "%s",
                 cJSON_IsString(sf) ? sf->valuestring : "");
        c->fc[n].is_day = !cJSON_IsFalse(dy);
        parse_icon(cJSON_IsString(ic) ? ic->valuestring : NULL, c->fc[n].shortf,
                   c->fc[n].icon, sizeof(c->fc[n].icon), &c->fc[n].is_day);
        n++;
    }
    c->nfc = n;
    return 0;
}

static void zcta_next_check(wx_ctx_t *c, long now)
{
    c->next_zcta = now + (long)(c->update_days * WX_DAY_S);
    (void)zcta_meta_write(c->zcta_meta, now);
}

/* Unzip, convert and install a newer Census table; re-resolve the ZIP. */
static void zcta_install(wx_ctx_t *c)
{
    char *txt = NULL, *tab = NULL;
    size_t tl = 0, bl = 0;
    int rows = -1;

    if (zcta_unzip_single((const unsigned char *)c->body, c->len, &txt, &tl) == 0)
        rows = zcta_from_gazetteer(txt, tl, c->zcta_year, &tab, &bl);
    if (rows > 1000 && zcta_write_atomic(c->zcta_state, tab, bl) == 0)
    {
        if (c->zip[0])
            c->have_loc = 0;            /* look the ZIP up in the new table */
        c->zip_missing = 0;
    }
    else
        set_err(c, "ZIP table update could not be installed", NULL);
    free(txt);
    free(tab);
}

static void finish_zcta(wx_ctx_t *c, wx_req_t what, CURLcode rc, long code)
{
    long now = (long)time(NULL);
    char url[256];

    if (rc != CURLE_OK || (code != 200 && code != 404 && code != 403))
    {
        c->next_zcta = now + WX_ZCTA_RETRY_S;   /* census.gov unreachable */
        return;
    }
    if (what == WX_ZCTA_HEAD && code == 200)
    {
        snprintf(url, sizeof(url), WX_ZCTA_URL, c->zcta_year, c->zcta_year);
        if (start_get(c, WX_ZCTA_GET, url) != 0)
            c->next_zcta = now + WX_ZCTA_RETRY_S;
        return;
    }
    if (what == WX_ZCTA_GET && code == 200)
        zcta_install(c);
    zcta_next_check(c, now);            /* up to date, or just updated */
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
    if (what == WX_ZCTA_HEAD || what == WX_ZCTA_GET)
    {
        finish_zcta(c, what, rc, code);
        return;
    }
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
    {
        ok = take_obs(c, root);
        if (ok == 1)                    /* moving on to the next station */
        {
            cJSON_Delete(root);
            return;
        }
    }
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
    else if (what == WX_OBS && c->st_try + 1 < c->nst)
    {
        c->st_try++;                    /* this station failed: next one */
        c->next_obs = 0;
    }
    else if (what == WX_STATION)
        c->next_station = now + WX_RETRY_S;
    else if (what == WX_OBS)
        c->next_obs = now + WX_RETRY_S;
    else if (what == WX_FORECAST)
        c->next_fc = now + WX_RETRY_S;
}

/* ZIP -> latitude/longitude from the offline table. */
static void resolve_zip(wx_ctx_t *c)
{
    const char *tab = zcta_pick(c->zcta_state, c->zcta_base);
    double lat, lon;
    int rc;

    if (c->have_loc || c->zip_missing || !c->zip[0])
        return;
    rc = zcta_lookup(tab, c->zip, &lat, &lon);
    if (rc == 0)
    {
        if (lat != c->lat || lon != c->lon)
        {
            c->forecast_url[0] = '\0';  /* new place: look it up again */
            c->nst = 0;
            c->next_points = 0;
        }
        c->lat = lat;
        c->lon = lon;
        c->have_loc = 1;
        return;
    }
    c->zip_missing = 1;
    if (rc == -2)
        set_err(c, "ZIP table not found", tab);
    else
    {
        char msg[48];

        snprintf(msg, sizeof(msg), "ZIP %s not found", c->zip);
        set_err(c, msg, NULL);
    }
}

static void start_zcta_check(wx_ctx_t *c)
{
    char url[256];
    long now = (long)time(NULL);

    if (c->update_days <= 0 || now < c->next_zcta)
        return;
    c->zcta_year = zcta_file_year(zcta_pick(c->zcta_state, c->zcta_base)) + 1;
    if (c->zcta_year < 2000)            /* no readable table: try this year */
    {
        time_t t = (time_t)now;
        struct tm tm;

        gmtime_r(&t, &tm);
        c->zcta_year = tm.tm_year + 1900;
    }
    snprintf(url, sizeof(url), WX_ZCTA_URL, c->zcta_year, c->zcta_year);
    if (start_get(c, WX_ZCTA_HEAD, url) != 0)
        c->next_zcta = now + WX_ZCTA_RETRY_S;
}

static void start_next(wx_ctx_t *c)
{
    char url[320];
    double now = mono_now();

    resolve_zip(c);
    if (!c->have_loc)
    {
        start_zcta_check(c);            /* a newer table may know the ZIP */
        return;
    }
    if (!c->forecast_url[0] || now >= c->next_points)
    {
        if (now < c->next_points)
            return;                     /* waiting out a failed lookup */
        snprintf(url, sizeof(url), WX_API "/points/%.4f,%.4f", c->lat, c->lon);
        (void)start_get(c, WX_POINTS, url);
    }
    else if (!c->nst)
    {
        if (now < c->next_station)
            return;
        snprintf(url, sizeof(url), "%s?limit=%d", c->stations_url, WX_NST);
        (void)start_get(c, WX_STATION, url);
    }
    else if (now >= c->next_obs)
    {
        snprintf(url, sizeof(url), WX_API "/stations/%s/observations/latest",
                 c->stations[c->st_try]);
        (void)start_get(c, WX_OBS, url);
    }
    else if (now >= c->next_fc)
        (void)start_get(c, WX_FORECAST, c->forecast_url);
    else
        start_zcta_check(c);            /* only when the weather is idle */
}

/* ---- plugin ops ---------------------------------------------------- */

static void *wx_open(const char *spec_json, char *err, size_t errsz)
{
    cJSON *spec = spec_json ? cJSON_Parse(spec_json) : NULL;
    const cJSON *w = cJSON_GetObjectItemCaseSensitive(spec, "weather");
    const cJSON *contact = cJSON_GetObjectItemCaseSensitive(w, "contact");
    wx_ctx_t *c;

    const cJSON *zj = cJSON_GetObjectItemCaseSensitive(w, "zip");
    char zip[8] = "";
    int have_ll = cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(w, "lat")) &&
                  cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(w, "lon"));

    if (cJSON_IsNumber(zj))
        snprintf(zip, sizeof(zip), "%05d", (int)zj->valuedouble);
    else if (cJSON_IsString(zj))
        snprintf(zip, sizeof(zip), "%.5s", zj->valuestring);
    if (zip[0] && (strlen(zip) != 5 || strspn(zip, "0123456789") != 5))
    {
        if (err && errsz)
            snprintf(err, errsz, "weather.zip must be 5 digits");
        cJSON_Delete(spec);
        return NULL;
    }
    if (!zip[0] && !have_ll)
    {
        if (err && errsz)
            snprintf(err, errsz, "set weather.zip (or weather.lat and weather.lon)");
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
    /* Explicit coordinates win; otherwise the ZIP is looked up in step(). */
    if (have_ll)
    {
        c->lat = json_num(w, "lat", 0.0);
        c->lon = json_num(w, "lon", 0.0);
        c->have_loc = 1;
        c->ll_given = 1;
    }
    else
        snprintf(c->zip, sizeof(c->zip), "%s", zip);
    c->update_days = json_num(w, "zip_update_days", 7.0);
    zcta_paths(c->zcta_state, sizeof(c->zcta_state), c->zcta_meta,
               sizeof(c->zcta_meta), c->zcta_base, sizeof(c->zcta_base));
    c->next_zcta = zcta_meta_checked(c->zcta_meta) +
                   (long)(c->update_days * WX_DAY_S);
    c->obs_every = json_num(spec, "poll_interval_s", 600.0);
    if (c->obs_every < WX_OBS_MIN_S)
        c->obs_every = WX_OBS_MIN_S;
    snprintf(c->contact, sizeof(c->contact), "%s", contact->valuestring);
    snprintf(c->ua, sizeof(c->ua), "moonflare-weathergov/0.1 (%s)", c->contact);
    {
        const cJSON *st = cJSON_GetObjectItemCaseSensitive(w, "station");

        if (cJSON_IsString(st))
            snprintf(c->want_station, sizeof(c->want_station), "%s",
                     st->valuestring);
    }
    c->obs.temp_f = c->obs.hum = c->obs.wind_mph = NAN;
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

/* The settings dialog shows these, flat and dotted like the Add form. */
static int wx_get_settings(void *ctx, char *json, size_t cap)
{
    wx_ctx_t *c = ctx;
    cJSON *o = cJSON_CreateObject();
    char *s;

    if (!c || !o)
        return MF_ERR_INVAL;
    cJSON_AddStringToObject(o, "weather.zip", c->zip);
    cJSON_AddStringToObject(o, "weather.station", c->want_station);
    cJSON_AddNumberToObject(o, "weather.zip_update_days", c->update_days);
    if (c->ll_given)
    {
        cJSON_AddNumberToObject(o, "weather.lat", c->lat);
        cJSON_AddNumberToObject(o, "weather.lon", c->lon);
    }
    else
    {
        cJSON_AddStringToObject(o, "weather.lat", "");
        cJSON_AddStringToObject(o, "weather.lon", "");
    }
    cJSON_AddStringToObject(o, "weather.contact", c->contact);
    s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!s || strlen(s) >= cap)
    {
        free(s);
        return MF_ERR_INVAL;
    }
    memcpy(json, s, strlen(s) + 1);
    free(s);
    return MF_OK;
}

static int bad(char *err, size_t errsz, const char *msg)
{
    if (err && errsz)
        snprintf(err, errsz, "%s", msg);
    return MF_ERR_INVAL;
}

/* A number from a settings value, or NAN when it is blank or absent. */
static double num_or_nan(const cJSON *v)
{
    if (cJSON_IsNumber(v))
        return v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring[0])
        return atof(v->valuestring);
    return NAN;
}

/* Change location, station, table checks or contact on a running module.
 * Validates everything first, then applies; a new place or station makes
 * the plugin look everything up again. */
static int wx_put_settings(void *ctx, const char *json, char *err, size_t errsz)
{
    wx_ctx_t *c = ctx;
    cJSON *b = json ? cJSON_Parse(json) : NULL;
    const cJSON *zj, *sj, *dj, *cj;
    char zip[8], station[16], contact[128];
    double lat, lon, days;
    int ll, changed_place, changed_station, rc = MF_OK;

    if (!c || !cJSON_IsObject(b))
    {
        cJSON_Delete(b);
        return bad(err, errsz, "settings are not a JSON object");
    }
    zj = cJSON_GetObjectItemCaseSensitive(b, "weather.zip");
    sj = cJSON_GetObjectItemCaseSensitive(b, "weather.station");
    dj = cJSON_GetObjectItemCaseSensitive(b, "weather.zip_update_days");
    cj = cJSON_GetObjectItemCaseSensitive(b, "weather.contact");
    snprintf(zip, sizeof(zip), "%s", c->zip);
    if (cJSON_IsNumber(zj))
        snprintf(zip, sizeof(zip), "%05d", (int)zj->valuedouble);
    else if (cJSON_IsString(zj))
        snprintf(zip, sizeof(zip), "%.7s", zj->valuestring);
    snprintf(station, sizeof(station), "%s",
             cJSON_IsString(sj) ? sj->valuestring : c->want_station);
    snprintf(contact, sizeof(contact), "%s",
             cJSON_IsString(cj) ? cj->valuestring : c->contact);
    days = dj ? num_or_nan(dj) : c->update_days;
    {
        /* The daemon keeps poll_interval_s too; honour a new one here. */
        double pi = num_or_nan(cJSON_GetObjectItemCaseSensitive(b, "poll_interval_s"));

        if (!isnan(pi))
            c->obs_every = pi < WX_OBS_MIN_S ? WX_OBS_MIN_S : pi;
    }
    lat = cJSON_GetObjectItemCaseSensitive(b, "weather.lat") ?
          num_or_nan(cJSON_GetObjectItemCaseSensitive(b, "weather.lat")) :
          (c->ll_given ? c->lat : NAN);
    lon = cJSON_GetObjectItemCaseSensitive(b, "weather.lon") ?
          num_or_nan(cJSON_GetObjectItemCaseSensitive(b, "weather.lon")) :
          (c->ll_given ? c->lon : NAN);
    ll = !isnan(lat) && !isnan(lon);
    cJSON_Delete(b);

    if (zip[0] && (strlen(zip) != 5 || strspn(zip, "0123456789") != 5))
        rc = bad(err, errsz, "weather.zip must be 5 digits");
    else if (!zip[0] && !ll)
        rc = bad(err, errsz, "set weather.zip (or weather.lat and weather.lon)");
    else if (strlen(station) > 8 ||
             strspn(station, "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                             "abcdefghijklmnopqrstuvwxyz0123456789") != strlen(station))
        rc = bad(err, errsz, "weather.station is a station ID like KIAH");
    else if (isnan(days) || days < 0)
        rc = bad(err, errsz, "weather.zip_update_days must be 0 or more");
    else if (!contact[0])
        rc = bad(err, errsz, "weather.contact (an email) is required");
    if (rc != MF_OK)
        return rc;

    changed_place = strcmp(zip, c->zip) != 0 || ll != c->ll_given ||
                    (ll && (lat != c->lat || lon != c->lon));
    changed_station = strcasecmp(station, c->want_station) != 0;
    snprintf(c->zip, sizeof(c->zip), "%s", ll ? "" : zip);
    snprintf(c->want_station, sizeof(c->want_station), "%s", station);
    snprintf(c->contact, sizeof(c->contact), "%s", contact);
    snprintf(c->ua, sizeof(c->ua), "moonflare-weathergov/0.1 (%s)", c->contact);
    if (days != c->update_days)
    {
        c->update_days = days;
        c->next_zcta = zcta_meta_checked(c->zcta_meta) +
                       (long)(c->update_days * WX_DAY_S);
    }
    if (changed_place)
    {
        c->ll_given = ll;
        c->have_loc = ll;
        c->zip_missing = 0;
        if (ll)
        {
            c->lat = lat;
            c->lon = lon;
        }
        c->forecast_url[0] = '\0';      /* look the new place up */
        c->nst = 0;
        c->next_points = 0;
        c->next_obs = 0;
        c->next_fc = 0;
    }
    else if (changed_station)
    {
        c->nst = 0;                     /* rebuild the candidate list */
        c->next_station = 0;
        c->next_obs = 0;
    }
    c->err[0] = '\0';
    return MF_OK;
}

static unsigned wx_caps(void *ctx)
{
    (void)ctx;
    return MF_CAP_READ | MF_CAP_WRITE_SETTINGS;
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
    add_num_or_null(wx, "temp_f", c->obs.temp_f);
    cJSON_AddStringToObject(wx, "conditions", c->obs.cond);
    cJSON_AddStringToObject(wx, "icon", c->obs.icon);
    cJSON_AddBoolToObject(wx, "is_day", c->obs.is_day);
    add_num_or_null(wx, "humidity_pct", c->obs.hum);
    add_num_or_null(wx, "wind_mph", c->obs.wind_mph);
    cJSON_AddStringToObject(wx, "wind_dir", c->obs.wind_dir);
    cJSON_AddStringToObject(wx, "station", c->obs.station);
    cJSON_AddStringToObject(wx, "place", c->place);
    if (c->zip[0])
        cJSON_AddStringToObject(wx, "zip", c->zip);
    cJSON_AddStringToObject(wx, "observed_local", c->obs.observed_local);
    fc = cJSON_AddArrayToObject(wx, "forecast");
    for (i = 0; i < c->nfc; i++)
    {
        cJSON *p = cJSON_CreateObject();

        cJSON_AddStringToObject(p, "name", c->fc[i].name);
        add_num_or_null(p, "temp_f", c->fc[i].temp_f);
        cJSON_AddStringToObject(p, "short", c->fc[i].shortf);
        cJSON_AddStringToObject(p, "icon", c->fc[i].icon);
        cJSON_AddBoolToObject(p, "is_day", c->fc[i].is_day);
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
    .get_settings = wx_get_settings,
    .put_settings = wx_put_settings,
    .describe = wx_describe,
};

size_t mf_plugin_entries(const mf_plugin_ops_t **out)
{
    if (out)
        *out = &g_ops;
    return 1;
}
