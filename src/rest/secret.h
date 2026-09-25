#ifndef MF_SECRET_H
#define MF_SECRET_H

/*
 * Plugin fields that are not plain settings, as describe()'s "fields"
 * declare them (see PLUGINS.md):
 *
 *   "type": "secret"   clients never read it back: REST shows a mask,
 *                      "********" and the last four characters, and a mask
 *                      sent back means "unchanged";
 *   "type": "action"   a button in the settings form, never a setting;
 *   "readonly": true   a value to show (credits left), never saved.
 *
 * Settings travel flat ({"textbelt.key": ...}); the config file keeps them
 * in the plugin's block ({"textbelt": {"key": ...}}).
 */

#include <cJSON.h>
#include <stddef.h>

#define MF_SECRET_MASK "********"

int  mf_field_is_secret(const cJSON *fields, const char *key);
/* An action or a read-only value: nothing to save. */
int  mf_field_not_setting(const cJSON *fields, const char *key);

/* "" for an empty value; else the mask, plus the last four characters
 * when the value is long enough (12+) that they give nothing away. */
void mf_secret_mask(const char *value, char *out, size_t cap);
int  mf_secret_is_mask(const char *value);

/* Flat settings for a client: secrets masked. */
void mf_secret_mask_settings(cJSON *settings, const cJSON *fields);

/* A flat settings PUT or add request: drop the masks sent back for
 * secrets, and the keys that are not settings. */
void mf_secret_clean_body(cJSON *body, const cJSON *fields);

/* A module as the config file has it: secrets masked (GET /config). */
void mf_secret_mask_module(cJSON *module, const cJSON *fields);

/* A config PUT's plugin blocks (extra_json text): where a secret came back
 * as a mask, null or not at all, keep the live value.  0 with the result in
 * out; -1 (out untouched) if either side is not JSON or it would not fit. */
int  mf_secret_restore_extra(const char *next_extra, const char *live_extra,
                             const cJSON *fields, char *out, size_t cap);

#endif
