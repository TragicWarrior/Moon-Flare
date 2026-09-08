#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 500
#define _DEFAULT_SOURCE

#include "usb_id.h"

#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_fail;
static char g_root[256];

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); g_fail++; } \
} while (0)

static int mkdir_p(const char *path)
{
    char tmp[512];
    char *p;
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) < 0 && access(tmp, F_OK) != 0)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) < 0 && access(tmp, F_OK) != 0)
        return -1;
    return 0;
}

static int write_file(const char *path, const char *data)
{
    FILE *f;
    char dir[512];
    char *slash;
    snprintf(dir, sizeof(dir), "%s", path);
    slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        if (mkdir_p(dir) != 0)
            return -1;
    }
    f = fopen(path, "w");
    if (!f)
        return -1;
    fputs(data, f);
    fclose(f);
    return 0;
}

static int add_tty(const char *tty, const char *serial, const char *vend,
                   const char *prod, const char *by_id)
{
    char p[768], link[768], tgt[256];
    snprintf(p, sizeof(p),
             "%s/sys/devices/pci0000:00/usb1/1-1/%s/serial", g_root, tty);
    if (write_file(p, serial) != 0)
        return -1;
    snprintf(p, sizeof(p),
             "%s/sys/devices/pci0000:00/usb1/1-1/%s/idVendor", g_root, tty);
    if (write_file(p, vend) != 0)
        return -1;
    snprintf(p, sizeof(p),
             "%s/sys/devices/pci0000:00/usb1/1-1/%s/idProduct", g_root, tty);
    if (write_file(p, prod) != 0)
        return -1;
    snprintf(p, sizeof(p),
             "%s/sys/devices/pci0000:00/usb1/1-1/%s/%s:1.0/%s",
             g_root, tty, tty, tty);
    if (mkdir_p(p) != 0)
        return -1;
    snprintf(p, sizeof(p), "%s/sys/class/tty/%s", g_root, tty);
    if (mkdir_p(p) != 0)
        return -1;
    snprintf(link, sizeof(link), "%s/sys/class/tty/%s/device", g_root, tty);
    snprintf(tgt, sizeof(tgt),
             "../../../devices/pci0000:00/usb1/1-1/%s/%s:1.0/%s",
             tty, tty, tty);
    unlink(link);
    if (symlink(tgt, link) != 0)
        return -1;
    snprintf(p, sizeof(p), "%s/dev/%s", g_root, tty);
    if (write_file(p, "") != 0)
        return -1;
    snprintf(p, sizeof(p), "%s/dev/serial/by-id", g_root);
    if (mkdir_p(p) != 0)
        return -1;
    snprintf(link, sizeof(link), "%s/dev/serial/by-id/%s", g_root, by_id);
    snprintf(tgt, sizeof(tgt), "../../%s", tty);
    unlink(link);
    if (symlink(tgt, link) != 0)
        return -1;
    return 0;
}

static int rm_cb(const char *fpath, const struct stat *sb, int typeflag,
                 struct FTW *ftwbuf)
{
    (void)sb;
    (void)ftwbuf;
    if (typeflag == FTW_DP)
        return rmdir(fpath);
    return unlink(fpath);
}

int main(void)
{
    char tmpl[] = "/tmp/mf-usb-id-XXXXXX";
    char *root;
    mf_usb_id_t id, got;
    mf_usb_want_t want;
    mf_endpoint_event_t ev[4];
    int rc, n;
    const char *by0 =
        "usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_0001-if00-port0";
    const char *by1 =
        "usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_0002-if00-port0";
    const char *by1b =
        "usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_0001-if00-port1";

    root = mkdtemp(tmpl);
    if (!root) {
        perror("mkdtemp");
        return 1;
    }
    snprintf(g_root, sizeof(g_root), "%s", root);
    mf_usb_set_root(g_root);
    mf_endpoint_reset();

    if (add_tty("ttyUSB0", "0001\n", "10c4\n", "ea60\n", by0) != 0 ||
        add_tty("ttyUSB1", "0002\n", "10c4\n", "ea60\n", by1) != 0) {
        fprintf(stderr, "FAIL: build fake sysfs\n");
        nftw(g_root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
        return 1;
    }

    CHECK(mf_usb_identify("/dev/ttyUSB0", &id) == 0, "identify ttyUSB0");
    CHECK(strcmp(id.path, "/dev/ttyUSB0") == 0, "identify path");
    CHECK(strcmp(id.serial_id, "0001") == 0, "identify serial 0001");
    CHECK(strcmp(id.by_id, by0) == 0, "identify by-id basename");
    CHECK(strcmp(id.vid_pid, "10c4:ea60") == 0, "identify vid:pid");

    /* Matching path is never replaced. */
    memset(&want, 0, sizeof(want));
    snprintf(want.path, sizeof(want.path), "/dev/ttyUSB0");
    snprintf(want.serial_id, sizeof(want.serial_id), "0001");
    want.auto_port = 1;
    want.path_ok = 1;
    rc = mf_usb_resolve("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeee01", &want, &got);
    CHECK(rc == MF_USB_USE, "matching path → USE");
    CHECK(strcmp(got.path, "/dev/ttyUSB0") == 0, "path not replaced");
    n = mf_endpoint_drain(ev, 4);
    CHECK(n == 0, "matching path does not enqueue");

    /* Empty serial_id + path_ok learns without scanning. */
    memset(&want, 0, sizeof(want));
    snprintf(want.path, sizeof(want.path), "/dev/ttyUSB0");
    want.auto_port = 1;
    want.path_ok = 1;
    rc = mf_usb_resolve("aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeee01", &want, &got);
    CHECK(rc == MF_USB_LEARN, "empty serial_id learns");
    CHECK(strcmp(got.path, "/dev/ttyUSB0") == 0, "learn keeps path");
    CHECK(strcmp(got.serial_id, "0001") == 0, "learn serial");
    CHECK(strcmp(got.by_id, by0) == 0, "learn by_id");
    n = mf_endpoint_drain(ev, 4);
    CHECK(n == 1, "learn enqueues endpoint_changed");
    CHECK(strcmp(ev[0].uuid, "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeee01") == 0,
          "learn uuid");
    CHECK(strcmp(ev[0].path, "/dev/ttyUSB0") == 0, "learn event path");
    CHECK(strcmp(ev[0].serial_id, "0001") == 0, "learn event serial");
    n = mf_endpoint_drain(ev, 4);
    CHECK(n == 0, "ring empty after spy");

    /* auto_port:false does not scan on miss. */
    memset(&want, 0, sizeof(want));
    snprintf(want.path, sizeof(want.path), "/dev/ttyUSB9");
    snprintf(want.serial_id, sizeof(want.serial_id), "0002");
    want.auto_port = 0;
    want.path_ok = 0;
    rc = mf_usb_resolve("bbbbbbbb-cccc-4ddd-8eee-ffffffffffff", &want, &got);
    CHECK(rc == MF_USB_ERROR, "auto_port false → no scan");
    n = mf_endpoint_drain(ev, 4);
    CHECK(n == 0, "auto_port false does not enqueue");

    /* Empty serial_id + miss does not walk ttyUSB*. */
    memset(&want, 0, sizeof(want));
    snprintf(want.path, sizeof(want.path), "/dev/ttyUSB9");
    want.auto_port = 1;
    want.path_ok = 0;
    rc = mf_usb_resolve("bbbbbbbb-cccc-4ddd-8eee-ffffffffffff", &want, &got);
    CHECK(rc == MF_USB_ERROR, "empty serial_id cannot scan");

    /* Miss relocates via by-id serial match + ring. */
    memset(&want, 0, sizeof(want));
    snprintf(want.path, sizeof(want.path), "/dev/ttyUSB0");
    snprintf(want.serial_id, sizeof(want.serial_id), "0002");
    want.auto_port = 1;
    want.path_ok = 0; /* path missing / open failed */
    rc = mf_usb_resolve("cccccccc-dddd-4eee-8fff-aaaaaaaaaa03", &want, &got);
    CHECK(rc == MF_USB_RELOCATE, "miss + serial → RELOCATE");
    CHECK(strcmp(got.path, "/dev/ttyUSB1") == 0, "relocated to ttyUSB1");
    CHECK(strcmp(got.serial_id, "0002") == 0, "relocated serial");
    n = mf_endpoint_drain(ev, 4);
    CHECK(n == 1, "relocate enqueues");
    CHECK(strcmp(ev[0].path, "/dev/ttyUSB1") == 0, "event new path");
    CHECK(strcmp(ev[0].serial_id, "0002") == 0, "event serial");

    /* Duplicate short serial: refuse without by_id hint. */
    if (add_tty("ttyUSB2", "0001\n", "10c4\n", "ea60\n", by1b) != 0) {
        fprintf(stderr, "FAIL: second 0001 node\n");
        g_fail++;
    } else {
        CHECK(mf_usb_find_by_serial("0001", NULL, &id) == -2,
              "duplicate 0001 without by_id → ambiguous");
        CHECK(mf_usb_find_by_serial("0001", by0, &id) == 0,
              "duplicate 0001 with by_id hint");
        CHECK(strcmp(id.path, "/dev/ttyUSB0") == 0, "hint picks ttyUSB0");
    }

    mf_usb_set_root(NULL);
    nftw(g_root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    if (g_fail) {
        fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("usb_id: ok\n");
    return 0;
}
