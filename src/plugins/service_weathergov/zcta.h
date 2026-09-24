#ifndef MF_WX_ZCTA_H
#define MF_WX_ZCTA_H

/* Offline ZIP-code centroids for the weather.gov plugin.
 *
 * Table format: a "#moonflare-zcta <year>" header, then one "ZIP\tLAT\tLON"
 * line per ZIP (ZCTA), sorted.  The baseline ships with moon-flare; newer
 * years come from the Census Bureau's Gazetteer ZCTA file and are written
 * to the plugin's state directory, which takes priority. */

#include <stddef.h>

/* Where the tables live.  state: $MF_WEATHER_DIR, else
 * $STATE_DIRECTORY/weathergov, else /var/lib/moonflare/weathergov.
 * base: $MF_ZCTA_BASELINE, else <datadir>/moon-flare/zcta.txt. */
void zcta_paths(char *state_tab, size_t scap, char *meta, size_t mcap,
                char *base_tab, size_t bcap);

/* The table to read: the state copy when present, else the baseline. */
const char *zcta_pick(const char *state_tab, const char *base_tab);

/* The year in a table's header, or 0. */
int  zcta_file_year(const char *path);

/* 0 found, -1 no such ZIP, -2 the table cannot be read. */
int  zcta_lookup(const char *path, const char *zip, double *lat, double *lon);

/* Census Gazetteer ZCTA text (tab- or '|'-delimited; GEOID first, latitude
 * and longitude last) -> our table in malloc'd *out.  Returns the number of
 * ZIPs, or -1. */
int  zcta_from_gazetteer(const char *txt, size_t len, int year,
                         char **out, size_t *outlen);

/* Inflate the single member of a .zip archive into malloc'd *out.  0 ok. */
int  zcta_unzip_single(const unsigned char *zip, size_t len,
                       char **out, size_t *outlen);

/* Write via a temp file and rename; creates the directory if needed. */
int  zcta_write_atomic(const char *path, const char *data, size_t len);

/* When the plugin last asked census.gov for a newer table (epoch), or 0. */
long zcta_meta_checked(const char *meta);
int  zcta_meta_write(const char *meta, long checked);

#endif
