#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "usb_id.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_root[256];

#define RING_CAP 16
static mf_endpoint_event_t g_ring[RING_CAP];
static int g_rhead, g_rcnt;

void mf_usb_set_root(const char *root)
{
    g_root[0] = '\0';
    if (root && root[0])
        snprintf(g_root, sizeof(g_root), "%s", root);
}

void mf_endpoint_reset(void)
{
    memset(g_ring, 0, sizeof(g_ring));
    g_rhead = 0;
    g_rcnt = 0;
}

int mf_endpoint_enqueue(const mf_endpoint_event_t *ev)
{
    int slot;
    if (!ev || !ev->uuid[0])
        return -1;
    if (g_rcnt >= RING_CAP)
        return -1;
    slot = (g_rhead + g_rcnt) % RING_CAP;
    g_ring[slot] = *ev;
    g_rcnt++;
    return 0;
}

int mf_endpoint_drain(mf_endpoint_event_t *out, int max)
{
    int n = 0;
    if (!out || max <= 0)
        return 0;
    while (n < max && g_rcnt > 0) {
        out[n] = g_ring[g_rhead];
        g_rhead = (g_rhead + 1) % RING_CAP;
        g_rcnt--;
        n++;
    }
    return n;
}

static void join_root(char *out, size_t cap, const char *abs)
{
    if (g_root[0])
        snprintf(out, cap, "%s%s", g_root, abs);
    else
        snprintf(out, cap, "%s", abs);
}

static void trim_nl(char *s)
{
    size_t n;
    if (!s)
        return;
    n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                     s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[n - 1] = '\0';
        n--;
    }
}

static int read_text(const char *path, char *buf, size_t cap)
{
    FILE *f;
    if (!path || !buf || cap == 0)
        return -1;
    buf[0] = '\0';
    f = fopen(path, "r");
    if (!f)
        return -1;
    if (!fgets(buf, (int)cap, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    trim_nl(buf);
    return buf[0] ? 0 : -1;
}

static const char *tty_basename(const char *dev_path)
{
    const char *s;
    if (!dev_path || !dev_path[0])
        return NULL;
    s = strrchr(dev_path, '/');
    return s ? s + 1 : dev_path;
}

static void make_dev_path(char *out, size_t cap, const char *tty)
{
    if (tty && tty[0] == '/')
        snprintf(out, cap, "%s", tty);
    else
        snprintf(out, cap, "/dev/%s", tty ? tty : "ttyUSB0");
}

/* udev by-id: usb-<stuff>_<SERIAL>-ifNN-portM — SERIAL is the last '_'
 * field before "-if". */
static int serial_from_byid(const char *by_id, char *out, size_t cap)
{
    const char *ifm, *us;
    size_t n;
    if (!by_id || !out || cap == 0)
        return -1;
    out[0] = '\0';
    ifm = strstr(by_id, "-if");
    if (!ifm || ifm == by_id)
        return -1;
    us = ifm;
    while (us > by_id && us[-1] != '_')
        us--;
    if (us == by_id || us >= ifm)
        return -1;
    n = (size_t)(ifm - us);
    if (n == 0 || n >= cap)
        return -1;
    memcpy(out, us, n);
    out[n] = '\0';
    return 0;
}

static int sysfs_identity(const char *tty, mf_usb_id_t *out)
{
    char linkp[512], resolved[512], serialp[576], vendp[576], prodp[576];
    char vend[8], prod[8];
    char rel[64];

    snprintf(rel, sizeof(rel), "/sys/class/tty/%s/device", tty);
    join_root(linkp, sizeof(linkp), rel);
    if (!realpath(linkp, resolved))
        return -1;
    snprintf(serialp, sizeof(serialp), "%s/../../serial", resolved);
    if (read_text(serialp, out->serial_id, sizeof(out->serial_id)) != 0)
        out->serial_id[0] = '\0';
    snprintf(vendp, sizeof(vendp), "%s/../../idVendor", resolved);
    snprintf(prodp, sizeof(prodp), "%s/../../idProduct", resolved);
    vend[0] = prod[0] = '\0';
    (void)read_text(vendp, vend, sizeof(vend));
    (void)read_text(prodp, prod, sizeof(prod));
    if (vend[0] && prod[0])
        snprintf(out->vid_pid, sizeof(out->vid_pid), "%s:%s", vend, prod);
    return (out->serial_id[0] || out->vid_pid[0]) ? 0 : -1;
}

static int byid_for_tty(const char *tty, char *by_id, size_t cap)
{
    char dirp[512], full[768], target[512];
    DIR *d;
    struct dirent *de;
    int nmatch = 0;

    by_id[0] = '\0';
    join_root(dirp, sizeof(dirp), "/dev/serial/by-id");
    d = opendir(dirp);
    if (!d)
        return -1;
    while ((de = readdir(d)) != NULL) {
        ssize_t n;
        const char *base;
        if (de->d_name[0] == '.')
            continue;
        snprintf(full, sizeof(full), "%s/%s", dirp, de->d_name);
        n = readlink(full, target, sizeof(target) - 1);
        if (n < 0)
            continue;
        target[n] = '\0';
        base = strrchr(target, '/');
        base = base ? base + 1 : target;
        if (strcmp(base, tty) != 0)
            continue;
        nmatch++;
        snprintf(by_id, cap, "%s", de->d_name);
    }
    closedir(d);
    return nmatch > 0 ? 0 : -1;
}

int mf_usb_identify(const char *dev_path, mf_usb_id_t *out)
{
    const char *tty;
    if (!out)
        return -1;
    memset(out, 0, sizeof(*out));
    tty = tty_basename(dev_path);
    if (!tty || !tty[0])
        return -1;
    make_dev_path(out->path, sizeof(out->path), tty);
    (void)byid_for_tty(tty, out->by_id, sizeof(out->by_id));
    (void)sysfs_identity(tty, out);
    if (!out->serial_id[0] && out->by_id[0])
        (void)serial_from_byid(out->by_id, out->serial_id,
                               sizeof(out->serial_id));
    if (!out->serial_id[0] && out->vid_pid[0]) {
        /* CH340-style: no serial node. Weak auto-port key. */
        snprintf(out->serial_id, sizeof(out->serial_id), "usb-%s",
                 out->vid_pid);
    }
    if (!out->serial_id[0] && !out->by_id[0] && !out->vid_pid[0])
        return -1;
    return 0;
}

int mf_usb_find_by_serial(const char *serial_id, const char *by_id_hint,
                          mf_usb_id_t *out)
{
    char dirp[512], full[768], target[512];
    DIR *d;
    struct dirent *de;
    mf_usb_id_t hits[8];
    int nhit = 0, i, hinted = -1;

    if (!serial_id || !serial_id[0] || !out)
        return -1;
    memset(out, 0, sizeof(*out));
    join_root(dirp, sizeof(dirp), "/dev/serial/by-id");
    d = opendir(dirp);
    if (!d)
        return -1;
    while ((de = readdir(d)) != NULL && nhit < 8) {
        ssize_t n;
        const char *base;
        mf_usb_id_t id;
        char devp[MF_USB_PATH_SIZE];
        if (de->d_name[0] == '.')
            continue;
        snprintf(full, sizeof(full), "%s/%s", dirp, de->d_name);
        n = readlink(full, target, sizeof(target) - 1);
        if (n < 0)
            continue;
        target[n] = '\0';
        base = strrchr(target, '/');
        base = base ? base + 1 : target;
        make_dev_path(devp, sizeof(devp), base);
        if (mf_usb_identify(devp, &id) != 0)
            continue;
        if (strcmp(id.serial_id, serial_id) != 0)
            continue;
        hits[nhit] = id;
        if (by_id_hint && by_id_hint[0] &&
            strcmp(id.by_id, by_id_hint) == 0)
            hinted = nhit;
        nhit++;
    }
    closedir(d);
    if (nhit == 0)
        return -1;
    if (nhit == 1) {
        *out = hits[0];
        return 0;
    }
    /* Duplicate ID_SERIAL_SHORT (CP2102N "0001"): require full by-id. */
    if (hinted >= 0) {
        *out = hits[hinted];
        return 0;
    }
    (void)i;
    return -2;
}

static int serial_matches(const mf_usb_id_t *id, const mf_usb_want_t *want)
{
    if (want->serial_id[0] && id->serial_id[0] &&
        strcmp(want->serial_id, id->serial_id) == 0)
        return 1;
    if (want->by_id[0] && id->by_id[0] &&
        strcmp(want->by_id, id->by_id) == 0)
        return 1;
    return 0;
}

static void publish(const char *uuid, const mf_usb_id_t *id)
{
    mf_endpoint_event_t ev;
    if (!uuid || !uuid[0] || !id)
        return;
    memset(&ev, 0, sizeof(ev));
    snprintf(ev.uuid, sizeof(ev.uuid), "%s", uuid);
    snprintf(ev.path, sizeof(ev.path), "%s", id->path);
    snprintf(ev.serial_id, sizeof(ev.serial_id), "%s", id->serial_id);
    snprintf(ev.by_id, sizeof(ev.by_id), "%s", id->by_id);
    (void)mf_endpoint_enqueue(&ev);
}

int mf_usb_resolve(const char *uuid, const mf_usb_want_t *want,
                   mf_usb_id_t *got)
{
    mf_usb_id_t id;
    int have_id;

    if (!want || !got)
        return MF_USB_ERROR;
    memset(got, 0, sizeof(*got));

    have_id = 0;
    if (want->path[0] && want->path_ok) {
        if (mf_usb_identify(want->path, &id) == 0)
            have_id = 1;
        if (!want->serial_id[0] && !want->by_id[0]) {
            /* First run: learn without scanning by-id. */
            if (have_id) {
                *got = id;
                snprintf(got->path, sizeof(got->path), "%s", want->path);
                publish(uuid, got);
                return MF_USB_LEARN;
            }
            snprintf(got->path, sizeof(got->path), "%s", want->path);
            return MF_USB_USE;
        }
        if (have_id && serial_matches(&id, want)) {
            *got = id;
            snprintf(got->path, sizeof(got->path), "%s", want->path);
            return MF_USB_USE;
        }
        /* Path opened but identity mismatch: fall through to relocate. */
    }

    if (!want->auto_port)
        return MF_USB_ERROR;
    if (!want->serial_id[0])
        return MF_USB_ERROR; /* never walk ttyUSB0..7 */

    if (mf_usb_find_by_serial(want->serial_id, want->by_id, &id) != 0)
        return MF_USB_ERROR;
    *got = id;
    if (want->path[0] && strcmp(got->path, want->path) == 0)
        return MF_USB_USE;
    publish(uuid, got);
    return MF_USB_RELOCATE;
}
