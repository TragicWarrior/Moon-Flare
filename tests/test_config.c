/*
 * test_config — QW-3 DoD verification.
 *
 * All file paths are under controlled temp dirs; no real /etc or real
 * $HOME is touched.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cJSON.h>
#include "config/config.h"

/* ------------------------------------------------------------------ */
/* helpers                                                            */

static void mkdtemp_copy(const char *src, char *dst, size_t dstsz)
{
    /* copy file; caller ensures dst dir exists */
    FILE *f = fopen(src, "rb");
    if (!f)
    {
        perror(src);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz);
    size_t rd = fread(buf, 1, (size_t)sz, f);
    (void)rd;
    fclose(f);
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s/%s", dst, "moonflared.json");
    FILE *out = fopen(tmp, "wb");
    fwrite(buf, 1, (size_t)sz, out);
    fclose(out);
    free(buf);
}

static void write_text(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (!f)
    {
        perror(path);
        exit(1);
    }
    fputs(content, f);
    fclose(f);
}

static void assert_int_eq(int expected, int actual, const char *msg)
{
    if (expected != actual)
    {
        fprintf(stderr, "FAIL: %s: expected %d, got %d\n", msg, expected, actual);
        exit(1);
    }
}

static void assert_str_eq(const char *expected, const char *actual, const char *msg)
{
    if (strcmp(expected, actual) != 0)
    {
        fprintf(stderr, "FAIL: %s: expected '%s', got '%s'\n", msg, expected, actual);
        exit(1);
    }
}

static void assert_true(int cond, const char *msg)
{
    if (!cond)
    {
        fprintf(stderr, "FAIL: %s\n", msg);
        exit(1);
    }
}

/* ------------------------------------------------------------------ */
/* 1 & 2. Search order + missing-file defaults                        */
/* ------------------------------------------------------------------ */

static void test_search_order(void)
{
    /* Create a fake HOME with ~/.config/moonflare/ */
    char fake_home[] = "/tmp/mf-test-home-XXXXXX";
    char *dir = mkdtemp(fake_home);

    char config_dir[4096];
    snprintf(config_dir, sizeof(config_dir), "%s/.config", dir);
    mkdir(config_dir, 0755);
    snprintf(config_dir, sizeof(config_dir), "%s/.config/moonflare", dir);
    mkdir(config_dir, 0755);

    /* Write user-level config (will be found via search order). */
    char user_cfg[4096];
    snprintf(user_cfg, sizeof(user_cfg), "%.4079s/moonflared.json", config_dir);
    write_text(user_cfg,
        "{\"listen\":\"10.0.0.1:9999\",\"devices\":[{\"uuid\":\"a1b2c3d4-e5f6-7890-abcd-ef1234567890\",\"name\":\"user-dev\",\"kind\":\"battery\",\"driver\":\"xd\"}]}");

    /* Set fake HOME. */
    setenv("HOME", dir, 1);

    /* No --config → should find user's file via search order. */
    mf_daemon_config_t cfg;
    int rc = mf_config_load(NULL, &cfg);
    assert_int_eq(0, rc, "search-order: load returns 0");
    assert_str_eq("10.0.0.1:9999", cfg.listen,
                  "search-order: user HOME config overrides listen");
    assert_int_eq(1, cfg.n_devices,
                  "search-order: user config has 1 device");
    assert_str_eq("a1b2c3d4-e5f6-7890-abcd-ef1234567890", cfg.devices[0].uuid,
                  "search-order: user device uuid");

    /* Now provide explicit --config → should NOT look in HOME. */
    char explicit_cfg[] = "/tmp/mf-test-explicit-XXXXXX";
    memcpy(explicit_cfg + strlen(explicit_cfg) - 6, "json\0", 5);

    /* Actually, create a proper path. */
    snprintf(explicit_cfg, sizeof(explicit_cfg), "/tmp/mf-test-explicit.json");
    write_text(explicit_cfg,
        "{\"listen\":\"192.168.1.1:8888\",\"devices\":[]}");

    rc = mf_config_load(explicit_cfg, &cfg);
    assert_int_eq(0, rc, "search-order: --config load returns 0");
    assert_str_eq("192.168.1.1:8888", cfg.listen,
                  "search-order: --config overrides HOME");
    assert_int_eq(0, cfg.n_devices,
                  "search-order: --config has zero devices");

    /* Provide a non-existent --config → should fall through to defaults. */
    rc = mf_config_load("/nonexistent/path/moonflared.json", &cfg);
    assert_int_eq(-1, rc,
                  "search-order: missing explicit --config returns -1");

    /* Missing HOME config → should get defaults. Remove the user config. */
    unlink(user_cfg);
    rc = mf_config_load(NULL, &cfg);
    assert_int_eq(0, rc, "search-order: missing HOME → defaults");
    assert_str_eq("0.0.0.0:5250", cfg.listen,
                  "search-order: missing file → default listen");
    assert_int_eq(0, cfg.n_devices,
                  "search-order: missing file → zero devices");

    /* Clean up HOME */
    setenv("HOME", "/root", 1); /* restore */
    rmdir(config_dir);
    rmdir(dir);
    unlink(explicit_cfg);

    printf("PASS: search order + missing-file defaults\n");
}

/* ------------------------------------------------------------------ */
/* 2. TUI missing-file defaults                                       */
/* ------------------------------------------------------------------ */

static void test_tui_defaults(void)
{
    char fake_home[] = "/tmp/mf-test-tui-XXXXXX";
    char *dir = mkdtemp(fake_home);
    char config_dir[4096];
    snprintf(config_dir, sizeof(config_dir), "%s/.config", dir);
    mkdir(config_dir, 0755);
    snprintf(config_dir, sizeof(config_dir), "%s/.config/moonflare", dir);
    mkdir(config_dir, 0755);

    setenv("HOME", dir, 1);

    mf_tui_config_t cfg;
    int rc = mf_tui_config_load(NULL, &cfg);
    assert_int_eq(0, rc, "tui: missing file returns 0");
    assert_int_eq(1, cfg.n_profiles, "tui: 1 default profile");
    assert_str_eq("127.0.0.1", cfg.profiles[0].host,
                  "tui: default profile host");
    assert_int_eq(5250, cfg.profiles[0].port, "tui: default profile port");
    assert_str_eq("local", cfg.profiles[0].name,
                  "tui: default profile name");
    assert_str_eq("local", cfg.default_profile,
                  "tui: default_profile name");
    assert_int_eq(1, cfg.refresh_interval_s == 1.0 ? 1 : 0,
                  "tui: default refresh_interval_s 1.0");

    setenv("HOME", "/root", 1);
    rmdir(config_dir);
    rmdir(dir);

    printf("PASS: tui missing-file defaults\n");
}

/* ------------------------------------------------------------------ */
/* 3. Round-trip moonflared.json.example                              */
/* ------------------------------------------------------------------ */

static const mf_config_device_t *find_dev(const mf_daemon_config_t *cfg,
                                          const char *uuid)
{
    int i;
    for (i = 0; i < cfg->n_devices; i++)
    {
        if (strcmp(cfg->devices[i].uuid, uuid) == 0)
            return &cfg->devices[i];
    }
    return NULL;
}

static void test_roundtrip_example(void)
{
    /* Copy the example file to a temp location. */
    char example_src[4096];
    snprintf(example_src, sizeof(example_src),
             "%s/etc/moonflared.json.example", MF_SOURCE_DIR);

    char tmpdir[] = "/tmp/mf-test-rt-XXXXXX";
    char *dir = mkdtemp(tmpdir);
    char cfg_path[4096];
    snprintf(cfg_path, sizeof(cfg_path), "%s/moonflared.json", dir);

    mkdtemp_copy(example_src, dir, sizeof(cfg_path));

    mf_daemon_config_t cfg;
    int rc = mf_config_load(cfg_path, &cfg);
    assert_int_eq(0, rc, "roundtrip: load example");

    /* Soak starter: demo + Classic on; XD/JK templates stay disabled. */
    assert_int_eq(4, cfg.n_devices, "roundtrip: 4 devices");

    const mf_config_device_t *demo = find_dev(
        &cfg, "3b2c0e5a-7c1d-4f2a-9b11-0c9e4d1a0004");
    const mf_config_device_t *classic = find_dev(
        &cfg, "3b2c0e5a-7c1d-4f2a-9b11-0c9e4d1a0003");
    const mf_config_device_t *xd = find_dev(
        &cfg, "3b2c0e5a-7c1d-4f2a-9b11-0c9e4d1a0001");
    const mf_config_device_t *jk = find_dev(
        &cfg, "3b2c0e5a-7c1d-4f2a-9b11-0c9e4d1a0002");
    assert_int_eq(1, demo && classic && xd && jk ? 1 : 0,
                  "roundtrip: all four example uuids present");
    assert_int_eq(1, demo->enabled ? 1 : 0, "roundtrip: pack-demo enabled");
    assert_int_eq(1, classic->enabled ? 1 : 0, "roundtrip: classic-1 enabled");
    assert_int_eq(0, xd->enabled ? 1 : 0, "roundtrip: pack-xd disabled");
    assert_int_eq(0, jk->enabled ? 1 : 0, "roundtrip: pack-jk disabled");

    assert_str_eq("172.16.0.20", classic->modbus.ip,
                  "roundtrip: classic modbus ip");
    assert_int_eq(10, classic->modbus.unit_id,
                  "roundtrip: classic unit_id");

    /* Serialize and re-parse (round-trip). */
    char *json = mf_config_serialize(&cfg);
    assert_int_eq(1, json != NULL ? 1 : 0, "roundtrip: serialize succeeded");

    mf_daemon_config_t cfg2;
    mf_config_defaults(&cfg2);
    cJSON *root = cJSON_Parse(json);
    assert_int_eq(1, root != NULL ? 1 : 0, "roundtrip: re-parse JSON");
    mf_config_apply_json(&cfg2, root);
    cJSON_Delete(root);

    /* Verify ip, unit_id, and soak enabled flags survived serialize/parse. */
    {
        const mf_config_device_t *c2 = find_dev(
            &cfg2, "3b2c0e5a-7c1d-4f2a-9b11-0c9e4d1a0003");
        const mf_config_device_t *xd2 = find_dev(
            &cfg2, "3b2c0e5a-7c1d-4f2a-9b11-0c9e4d1a0001");
        const mf_config_device_t *jk2 = find_dev(
            &cfg2, "3b2c0e5a-7c1d-4f2a-9b11-0c9e4d1a0002");
        assert_int_eq(1, c2 && xd2 && jk2 ? 1 : 0,
                      "roundtrip: devices survived re-parse");
        assert_str_eq("172.16.0.20", c2->modbus.ip,
                      "roundtrip: ip survived round-trip");
        assert_int_eq(10, c2->modbus.unit_id,
                      "roundtrip: unit_id survived round-trip");
        assert_int_eq(0, xd2->enabled ? 1 : 0,
                      "roundtrip: pack-xd stayed disabled");
        assert_int_eq(0, jk2->enabled ? 1 : 0,
                      "roundtrip: pack-jk stayed disabled");
    }

    free(json);
    /* Clean up temp dir (best effort). */
    unlink(cfg_path);
    rmdir(dir);

    printf("PASS: round-trip moonflared.json.example\n");
}

/* ------------------------------------------------------------------ */
/* 4. Unknown keys ignored                                            */
/* ------------------------------------------------------------------ */

static void test_unknown_keys(void)
{
    char json[] =
        "{"
        "  \"listen\":\"0.0.0.0:5250\","
        "  \"unknown_field_xyz\": true,"
        "  \"devices\": ["
        "    {"
        "      \"uuid\": \"aaaa-bbbb-cccc-dddd-eeeeeeeeeeee\","
        "      \"name\": \"test-dev\","
        "      \"gibberish\": 42,"
        "      \"also_unknown\": \"hello\""
        "    }"
        "  ]"
        "}";

    mf_daemon_config_t cfg;
    mf_config_defaults(&cfg);

    cJSON *root = cJSON_Parse(json);
    assert_int_eq(1, root != NULL ? 1 : 0, "unknown-keys: parse");
    mf_config_apply_json(&cfg, root);
    cJSON_Delete(root);

    assert_int_eq(1, cfg.n_devices, "unknown-keys: 1 device loaded");
    assert_str_eq("aaaa-bbbb-cccc-dddd-eeeeeeeeeeee", cfg.devices[0].uuid,
                  "unknown-keys: uuid survived");
    assert_str_eq("test-dev", cfg.devices[0].name,
                  "unknown-keys: name survived");
    assert_str_eq("0.0.0.0:5250", cfg.listen,
                  "unknown-keys: listen survived");

    printf("PASS: unknown keys ignored\n");
}

/* ------------------------------------------------------------------ */
/* 5. Atomic save (write temp + rename)                               */
/* ------------------------------------------------------------------ */

static void test_atomic_save(void)
{
    char tmpdir[] = "/tmp/mf-test-save-XXXXXX";
    char *dir = mkdtemp(tmpdir);
    char cfg_path[4096];
    snprintf(cfg_path, sizeof(cfg_path), "%s/moonflared.json", dir);

    /* Build a minimal config. */
    mf_daemon_config_t cfg;
    mf_config_defaults(&cfg);
    cfg.devices[0].uuid[0] = 'x';
    cfg.n_devices = 1;

    int rc = mf_config_save(&cfg, cfg_path);
    assert_int_eq(0, rc, "atomic: save returns 0");

    /* Verify file exists. */
    struct stat st;
    assert_int_eq(0, stat(cfg_path, &st), "atomic: file exists after save");
    assert_int_eq(1, S_ISREG(st.st_mode), "atomic: is regular file");

    /* Re-load the saved file and verify data. */
    mf_daemon_config_t cfg2;
    rc = mf_config_load(cfg_path, &cfg2);
    assert_int_eq(0, rc, "atomic: reload after save");
    assert_str_eq("0.0.0.0:5250", cfg2.listen, "atomic: listen after reload");

    /* Cleanup. */
    unlink(cfg_path);
    rmdir(dir);

    printf("PASS: atomic save\n");
}

/* ------------------------------------------------------------------ */
/* 6. Device UUID required-on-load                                    */
/* ------------------------------------------------------------------ */

static void test_device_uuid_required(void)
{
    char json[] =
        "{\"devices\":["
        "  {\"uuid\":\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\",\"name\":\"ok\"},"
        "  {\"name\":\"no-uuid\"}"
        "]}";

    mf_daemon_config_t cfg;
    mf_config_defaults(&cfg);

    cJSON *root = cJSON_Parse(json);
    mf_config_apply_json(&cfg, root);
    cJSON_Delete(root);

    /* Only the device with a valid UUID should be loaded. */
    assert_int_eq(1, cfg.n_devices, "uuid-required: 1 device loaded");
    assert_str_eq("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee",
                  cfg.devices[0].uuid,
                  "uuid-required: only uuid device loaded");
    assert_str_eq("ok", cfg.devices[0].name,
                  "uuid-required: name of valid device");

    /* File-less defaults: zero devices (UUID generated on POST later). */
    mf_daemon_config_t empty;
    mf_config_defaults(&empty);
    assert_int_eq(0, empty.n_devices,
                  "uuid-required: defaults have zero devices");

    printf("PASS: device UUID required-on-load\n");
}

/* ------------------------------------------------------------------ */
/* 7. Passwords round-trip + config_redact                            */
/* ------------------------------------------------------------------ */

static void test_passwords(void)
{
    char json[] =
        "{\"listen\":\"0.0.0.0:5250\","
        " \"devices\":["
        "   {\"uuid\":\"1111-2222-3333-4444-555566667777\","
        "    \"kind\":\"battery\",\"driver\":\"jk\","
        "    \"bus\":\"ble\","
        "    \"ble\":{\"address\":\"AA:BB:CC:DD:EE:FF\","
        "             \"password\":\"super-secret\"}}"
        "  ]"
        "}";

    /* Parse */
    mf_daemon_config_t cfg;
    mf_config_defaults(&cfg);
    cJSON *root = cJSON_Parse(json);
    mf_config_apply_json(&cfg, root);
    cJSON_Delete(root);

    assert_int_eq(1, cfg.n_devices, "passwords: 1 device");
    assert_str_eq("super-secret", cfg.devices[0].ble.password,
                  "passwords: password round-trips in memory");

    /* Serialize → redact → serialize again */
    char *before = mf_config_serialize(&cfg);
    assert_int_eq(1, before != NULL ? 1 : 0, "passwords: serialize before redact");

    /* The serialized JSON should contain "super-secret". */
    assert_int_eq(1, strstr(before, "super-secret") != NULL,
                  "passwords: unredacted JSON has password");
    free(before);

    /* Redact and re-serialize. */
    mf_config_redact(&cfg);

    char *after = mf_config_serialize(&cfg);
    assert_int_eq(1, after != NULL ? 1 : 0, "passwords: serialize after redact");

    assert_int_eq(1, strstr(after, "super-secret") == NULL,
                  "passwords: redacted JSON does NOT contain password");
    assert_int_eq(1, (strstr(after, "\"password\":null") != NULL ||
                      strstr(after, "\"password\":\"\"") != NULL),
                  "passwords: redacted JSON has null/empty password");

    free(after);

    printf("PASS: passwords round-trip + config_redact\n");
}

/* ------------------------------------------------------------------ */
/* 8. Overlay merge                                                   */
/* ------------------------------------------------------------------ */

static void test_overlay_merge(void)
{
    /* Base config with a classic device. */
    char base_json[] =
        "{\"listen\":\"0.0.0.0:5250\","
        " \"devices\":["
        "   {"
        "     \"uuid\":\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\","
        "     \"name\":\"base-dev\","
        "     \"kind\":\"charger\",\"driver\":\"classic\","
        "     \"bus\":\"modbus-tcp\","
        "     \"modbus\":{\"ip\":\"10.0.0.1\",\"port\":502,\"unit_id\":5}"
        "   }"
        "  ]"
        "}";

    /* Overlay patches usb.path and modbus.ip. */
    char overlay_json[] =
        "{\"devices\":["
        "  {"
        "    \"uuid\":\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\","
        "    \"usb\":{\"path\":\"/dev/ttyUSB1\"},"
        "    \"modbus\":{\"ip\":\"192.168.0.99\"}"
        "  }"
        "]}";

    /* Parse base. */
    mf_daemon_config_t base;
    mf_config_defaults(&base);
    cJSON *root = cJSON_Parse(base_json);
    mf_config_apply_json(&base, root);
    cJSON_Delete(root);

    assert_int_eq(1, base.n_devices, "overlay: base has 1 device");

    /* Parse overlay. */
    mf_daemon_config_t overlay;
    mf_config_defaults(&overlay);
    root = cJSON_Parse(overlay_json);
    mf_config_apply_json(&overlay, root);
    cJSON_Delete(root);

    assert_int_eq(1, overlay.n_devices, "overlay: overlay has 1 device");

    /* Merge. */
    mf_config_overlay_merge(&base, &overlay);

    /* Verify: modbus.ip changed, unit_id preserved from base,
     * usb.path added from overlay. */
    assert_str_eq("192.168.0.99", base.devices[0].modbus.ip,
                  "overlay: modbus.ip patched");
    assert_int_eq(5, base.devices[0].modbus.unit_id,
                  "overlay: unit_id preserved from base");

    /* The base didn't have usb.path before; overlay adds it. */
    assert_str_eq("/dev/ttyUSB1", base.devices[0].usb.path,
                  "overlay: usb.path added from overlay");

    /* Base name should be preserved (not overwritten by overlay). */
    assert_str_eq("base-dev", base.devices[0].name,
                  "overlay: base name preserved");

    printf("PASS: overlay merge\n");
}

static void test_overlay_name_persist(void)
{
    char dir[] = "/tmp/mf-ov-XXXXXX";
    char path[256];
    mf_daemon_config_t base, loaded;
    cJSON *root;

    assert_true(mkdtemp(dir) != NULL, "overlay tmpdir");
    snprintf(path, sizeof(path), "%s/settings.json", dir);
    setenv("MF_SETTINGS_OVERLAY", path, 1);

    mf_config_defaults(&base);
    root = cJSON_Parse(
        "{\"devices\":[{\"uuid\":\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\","
        "\"name\":\"pack-xd\",\"poll_interval_s\":2.0}]}");
    mf_config_apply_json(&base, root);
    cJSON_Delete(root);
    snprintf(base.devices[0].name, sizeof(base.devices[0].name), "XD Battery");
    base.devices[0].poll_interval_s = 2.5;
    assert_int_eq(0, mf_config_save_overlay(&base), "overlay save");

    mf_config_defaults(&loaded);
    root = cJSON_Parse(
        "{\"devices\":[{\"uuid\":\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\","
        "\"name\":\"pack-xd\",\"poll_interval_s\":2.0}]}");
    mf_config_apply_json(&loaded, root);
    cJSON_Delete(root);
    assert_int_eq(0, mf_config_load_overlay(&loaded), "overlay load");
    assert_str_eq("XD Battery", loaded.devices[0].name, "overlay name wins");
    assert_true(loaded.devices[0].poll_interval_s > 2.4, "overlay poll wins");

    unsetenv("MF_SETTINGS_OVERLAY");
    unlink(path);
    rmdir(dir);
    printf("PASS: overlay name persist\n");
}

static void test_active_and_system(void)
{
    char dir[] = "/tmp/mf-act-XXXXXX";
    char path[256];
    mf_daemon_config_t cfg, loaded;
    cJSON *root;
    char *json;
    const char *base_json =
        "{\"system\":{\"input_max_w\":4200},"
        "\"devices\":[{\"uuid\":\"aaaaaaaa-bbbb-cccc-dddd-000000000001\","
        "\"name\":\"jk\",\"active\":false,\"capture_interval_s\":30},"
        "{\"uuid\":\"aaaaaaaa-bbbb-cccc-dddd-000000000002\",\"name\":\"xd\"}]}";

    mf_config_defaults(&cfg);
    assert_true(cfg.system.input_max_w == MF_SYSTEM_INPUT_MAX_W_DEFAULT,
                "system: input max default");
    root = cJSON_Parse(base_json);
    mf_config_apply_json(&cfg, root);
    cJSON_Delete(root);
    assert_true(cfg.system.input_max_w == 4200.0, "system: input max parsed");
    assert_true(cfg.system.discharge_max_w == MF_SYSTEM_DISCHARGE_MAX_W_DEFAULT,
                "system: discharge max keeps default");
    assert_true(!cfg.devices[0].active, "active:false parsed");
    assert_true(cfg.devices[1].active, "active defaults to true");

    json = mf_config_serialize(&cfg);
    assert_true(json && strstr(json, "\"active\":false") &&
                strstr(json, "\"input_max_w\":4200"),
                "active and system serialized");
    free(json);

    /* Overlay round-trip carries active and capture_interval_s. */
    assert_true(mkdtemp(dir) != NULL, "active tmpdir");
    snprintf(path, sizeof(path), "%s/settings.json", dir);
    setenv("MF_SETTINGS_OVERLAY", path, 1);
    cfg.devices[1].active = false;
    cfg.devices[1].capture_interval_s = 60.0;
    assert_int_eq(0, mf_config_save_overlay(&cfg), "active overlay save");
    mf_config_defaults(&loaded);
    root = cJSON_Parse(base_json);
    mf_config_apply_json(&loaded, root);
    cJSON_Delete(root);
    assert_int_eq(0, mf_config_load_overlay(&loaded), "active overlay load");
    assert_true(!loaded.devices[1].active, "overlay active wins");
    assert_true(loaded.devices[1].capture_interval_s == 60.0,
                "overlay capture_interval_s wins");

    /* An overlay from before "active" existed must not reset it to true. */
    write_text(path, "{\"devices\":[{\"uuid\":"
               "\"aaaaaaaa-bbbb-cccc-dddd-000000000001\",\"name\":\"jk\"}]}");
    mf_config_defaults(&loaded);
    root = cJSON_Parse(base_json);
    mf_config_apply_json(&loaded, root);
    cJSON_Delete(root);
    assert_int_eq(0, mf_config_load_overlay(&loaded), "old overlay load");
    assert_true(!loaded.devices[0].active, "old overlay keeps base active");
    assert_true(loaded.devices[0].capture_interval_s == 30.0,
                "old overlay keeps base capture_interval_s");

    unsetenv("MF_SETTINGS_OVERLAY");
    unlink(path);
    rmdir(dir);
    printf("PASS: active flag and system section\n");
}

static void test_saved_config_reloads(void)
{
    mf_daemon_config_t cfg, again;
    cJSON *root;
    char *json;
    char a[160], b[160];

    /* Two devices with no transport: the saved form writes
     * "modbus":{"ip":null,...} for both. Re-loading must not invent an
     * endpoint (they would collide as duplicates) or turn on auto_net. */
    mf_config_defaults(&cfg);
    root = cJSON_Parse(
        "{\"devices\":[{\"uuid\":\"aaaaaaaa-bbbb-cccc-dddd-000000000011\","
        "\"name\":\"one\",\"kind\":\"battery\",\"driver\":\"demo\"},"
        "{\"uuid\":\"aaaaaaaa-bbbb-cccc-dddd-000000000012\","
        "\"name\":\"two\",\"kind\":\"charger\",\"driver\":\"demo\"}]}");
    mf_config_apply_json(&cfg, root);
    cJSON_Delete(root);
    json = mf_config_serialize(&cfg);
    assert_true(json != NULL, "reload: serialize");
    mf_config_defaults(&again);
    root = cJSON_Parse(json);
    free(json);
    mf_config_apply_json(&again, root);
    cJSON_Delete(root);
    assert_int_eq(2, again.n_devices, "reload: both devices back");
    mf_config_device_endpoint(&again.devices[0], a, sizeof(a));
    mf_config_device_endpoint(&again.devices[1], b, sizeof(b));
    assert_true(a[0] == '\0' && b[0] == '\0', "reload: no invented endpoint");
    assert_true(!again.devices[0].modbus.auto_net, "reload: auto_net stays off");

    /* A modbus key left out still reads as not-set (-1). */
    mf_config_defaults(&again);
    root = cJSON_Parse(
        "{\"devices\":[{\"uuid\":\"aaaaaaaa-bbbb-cccc-dddd-000000000013\","
        "\"name\":\"three\",\"modbus\":{\"ip\":\"10.0.0.9\"}}]}");
    mf_config_apply_json(&again, root);
    cJSON_Delete(root);
    assert_int_eq(-1, again.devices[0].modbus.unit_id,
                  "reload: absent modbus number is not-set");
    printf("PASS: saved config reloads\n");
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    test_search_order();
    test_tui_defaults();
    test_roundtrip_example();
    test_unknown_keys();
    test_atomic_save();
    test_device_uuid_required();
    test_passwords();
    test_overlay_merge();
    test_overlay_name_persist();
    test_active_and_system();
    test_saved_config_reloads();

    printf("\nAll config tests passed.\n");
    return 0;
}
