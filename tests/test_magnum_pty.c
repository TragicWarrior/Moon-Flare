/*
 * The Magnum plugin against a pseudo-terminal: a replayed bus with ~100 ms
 * cycles, garbage, a bus pause mid-packet, a 2 s daemon stall, silence, a
 * second module on the same port, another process holding the port, the
 * adapter unplugged and plugged back in, and repeated add/remove (run under
 * valgrind too).
 *
 *   test_magnum_pty libmf_inverter_magnum.so [quick]
 */

#define _GNU_SOURCE

#include "mf_plugin.h"

#include <cJSON.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int g_fail;
static const mf_plugin_ops_t *g_ops;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); g_fail++; } \
} while (0)

static const char *CYCLES[][3] = {
    { "4000020E0016780001003D11332473010005025800",
      "00002808640A280000C89B840C14122014007300A0", "A102343A007F" },
    { "4000020E0016780001003D11332473010005025800",
      "00002808640A280000C89B840C1412200000280080",
      "814C09F1007407E00C08FF984FF000140A01" },
    { "4000020E0016780001003D11332473010005025800",
      "00002808640A280000C89B840C1412200000000000", "9120" },
};

static double now_mono(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void nap(double s)
{
    struct timespec ts = { (time_t)s, (long)((s - (double)(time_t)s) * 1e9) };

    nanosleep(&ts, NULL);
}

static void write_hex(int fd, const char *hex)
{
    unsigned char b[64];
    size_t n = 0;

    while (hex[0] && hex[1] && n < sizeof(b))
    {
        unsigned v;

        sscanf(hex, "%2x", &v);
        b[n++] = (unsigned char)v;
        hex += 2;
    }
    if (write(fd, b, n) != (ssize_t)n)
        fprintf(stderr, "warn: short pty write\n");
}

/* One bus cycle, the packets a few ms apart (as the devices send them). */
static void write_cycle(int master, int k)
{
    int i;

    for (i = 0; i < 3; i++)
    {
        write_hex(master, CYCLES[k % 3][i]);
        nap(0.004);
    }
}

/* Run the plugin like the daemon does: select on its fd or a 50 ms tick,
 * then step.  Writes a bus cycle every 100 ms when `bus` is set. */
static mf_step_t run(void *ctx, int master, double secs, int bus, int *cycle)
{
    double end = now_mono() + secs, next = 0.0;
    mf_step_t last = MF_STEP_IDLE;

    while (now_mono() < end)
    {
        fd_set r;
        struct timeval tv = { 0, 50000 };
        int fd = g_ops->fd(ctx);

        if (bus && now_mono() >= next)
        {
            write_cycle(master, (*cycle)++);
            next = now_mono() + 0.100;
        }
        FD_ZERO(&r);
        if (fd >= 0)
            FD_SET(fd, &r);
        (void)select(fd >= 0 ? fd + 1 : 0, &r, NULL, NULL, &tv);
        last = g_ops->step(ctx);
    }
    return last;
}

static void *open_ctx(const char *path, const char *serial, char *err, size_t errsz)
{
    char spec[512];

    snprintf(spec, sizeof(spec),
             "{\"uuid\":\"aaaaaaaa-0000-4000-8000-00000000000%d\",\"name\":\"t\","
             "\"kind\":\"inverter\",\"driver\":\"magnum\",\"poll_interval_s\":0.5,"
             "\"usb\":{\"path\":\"%s\",\"serial_id\":\"%s\",\"auto_port\":false},"
             "\"magnum\":{\"tap\":\"test tap\"}}", 1, path ? path : "",
             serial ? serial : "");
    err[0] = '\0';
    return g_ops->open(spec, err, errsz);
}

static cJSON *reading(void *ctx)
{
    static char buf[16384];

    if (g_ops->get_reading(ctx, buf, sizeof(buf)) != MF_OK)
        return NULL;
    return cJSON_Parse(buf);
}

static int make_pty(int *master, char *name, size_t cap)
{
    int slave;
    char sname[128];

    if (openpty(master, &slave, sname, NULL, NULL) != 0)
        return -1;
    snprintf(name, cap, "%s", sname);
    close(slave);           /* the plugin must be the only one with it open */
    return 0;
}

static void test_config_rules(void)
{
    char err[256];
    void *c;

    c = open_ctx("/dev/ttyUSB0", NULL, err, sizeof(err));
    CHECK(c == NULL && strstr(err, "by-id"), "a ttyUSBn path is refused");
    c = open_ctx("", "", err, sizeof(err));
    CHECK(c == NULL && strstr(err, "usb.path"), "no path or serial is refused");
}

static void test_bus(void)
{
    char name[128], err[256];
    int master, cycle = 0;
    void *c, *c2;
    cJSON *o;
    mf_step_t st;
    unsigned char junk[40];
    int i;

    if (make_pty(&master, name, sizeof(name)) != 0)
    {
        fprintf(stderr, "FAIL: openpty: %s\n", strerror(errno));
        g_fail++;
        return;
    }
    c = open_ctx(name, NULL, err, sizeof(err));
    CHECK(c != NULL, "open on a pty path");
    if (!c)
        return;
    st = g_ops->step(c);                        /* opens the port */
    CHECK(st == MF_STEP_ERROR && g_ops->get_reading(c, err, sizeof(err)) == MF_ERR_OFFLINE,
          "offline until the first inverter packet");
    CHECK(g_ops->last_error(c) && strstr(g_ops->last_error(c), "waiting for bus data"),
          "says it is waiting for data");

    run(c, master, 1.0, 1, &cycle);
    o = reading(c);
    CHECK(o != NULL, "online after inverter packets");
    if (o)
    {
        cJSON *ags = cJSON_GetObjectItem(o, "ags");
        cJSON *diag = cJSON_GetObjectItem(o, "diag");

        CHECK(fabs(cJSON_GetObjectItem(o, "dc_voltage_v")->valuedouble - 52.6) < 1e-6,
              "dc_voltage_v 52.6");
        CHECK(strcmp(cJSON_GetObjectItem(o, "tap")->valuestring, "test tap") == 0, "tap");
        CHECK(ags && cJSON_GetObjectItem(ags, "status_text"), "ags decoded");
        CHECK(cJSON_GetObjectItem(o, "bmk") && cJSON_GetObjectItem(o, "router"),
              "bmk and router decoded");
        CHECK(diag && cJSON_GetObjectItem(diag, "bad_packets")->valuedouble == 0,
              "no bad packets on a clean bus");
        cJSON_Delete(o);
    }

    /* A second module on the same port is refused. */
    c2 = open_ctx(name, NULL, err, sizeof(err));
    CHECK(c2 != NULL, "second module opens (port opens later)");
    if (c2)
    {
        CHECK(g_ops->step(c2) == MF_STEP_ERROR && g_ops->last_error(c2) &&
              strstr(g_ops->last_error(c2), "already open"), "second module refused");
        g_ops->close(c2);
    }

    /* Garbage and a 300 ms stall mid-stream: still online, resynced. */
    memset(junk, 0xFF, sizeof(junk));
    if (write(master, junk, sizeof(junk)) != (ssize_t)sizeof(junk))
        fprintf(stderr, "warn: short junk write\n");
    write_hex(master, "4000020E00167800");  /* the start of a packet, then stall */
    run(c, master, 0.30, 0, &cycle);
    run(c, master, 0.6, 1, &cycle);
    o = reading(c);
    CHECK(o != NULL, "online after garbage and a stall");
    if (o)
    {
        cJSON *diag = cJSON_GetObjectItem(o, "diag");

        CHECK(diag && cJSON_GetObjectItem(diag, "unknown_bytes")->valuedouble >= 40,
              "garbage counted");
        CHECK(diag && cJSON_GetObjectItem(diag, "ff_bytes")->valuedouble >= 40,
              "0xFF counted");
        cJSON_Delete(o);
    }

    /* "refresh" resets the diag counters. */
    CHECK(g_ops->action(c, "refresh", "{}", err, sizeof(err)) == MF_OK, "refresh");
    g_ops->step(c);
    o = reading(c);
    if (o)
    {
        cJSON *diag = cJSON_GetObjectItem(o, "diag");

        CHECK(diag && cJSON_GetObjectItem(diag, "unknown_bytes")->valuedouble < 40,
              "counters reset");
        cJSON_Delete(o);
    }

    /* The daemon stalls for 2 s while the bus keeps talking: the bytes wait
     * in the tty buffer and every cycle decodes when it resumes. */
    CHECK(g_ops->action(c, "refresh", "{}", err, sizeof(err)) == MF_OK,
          "refresh before the stall");
    for (i = 0; i < 20; i++)
    {
        write_cycle(master, cycle++);
        nap(0.090);
    }
    g_ops->step(c);
    o = reading(c);
    CHECK(o != NULL, "online after a 2 s stall");
    if (o)
    {
        cJSON *diag = cJSON_GetObjectItem(o, "diag");
        cJSON *pk = diag ? cJSON_GetObjectItem(diag, "packets") : NULL;

        CHECK(pk && cJSON_GetObjectItem(pk, "inverter")->valuedouble == 20 &&
              cJSON_GetObjectItem(pk, "remote")->valuedouble == 20,
              "every cycle decoded after the stall");
        CHECK(diag && cJSON_GetObjectItem(diag, "bad_packets")->valuedouble == 0 &&
              cJSON_GetObjectItem(diag, "unknown_bytes")->valuedouble == 0,
              "nothing skipped after the stall");
        cJSON_Delete(o);
    }

    /* Silence: offline after the stale time, saying why. */
    run(c, master, 0.9, 0, &cycle);
    CHECK(g_ops->get_reading(c, err, sizeof(err)) == MF_ERR_OFFLINE, "offline when silent");
    CHECK(g_ops->last_error(c) && strstr(g_ops->last_error(c), "no data on the bus"),
          "says the bus is silent");

    /* Back online as soon as packets return. */
    run(c, master, 0.5, 1, &cycle);
    o = reading(c);
    CHECK(o != NULL, "online again");
    cJSON_Delete(o);

    /* The adapter goes away. */
    close(master);
    run(c, master, 0.3, 0, &cycle);
    CHECK(g_ops->get_reading(c, err, sizeof(err)) == MF_ERR_OFFLINE, "offline without adapter");
    CHECK(g_ops->last_error(c) && (strstr(g_ops->last_error(c), "adapter") != NULL),
          "says the adapter is gone");
    g_ops->close(c);
}

/* The adapter is unplugged and plugged back in: its by-id link goes away
 * and comes back pointing at a new node, and the same module reopens it. */
static void test_replug(void)
{
    char dir[] = "/tmp/mf-magnum-XXXXXX", link[256], name[128], err[256];
    int master, cycle = 0;
    void *c;
    cJSON *o;

    if (!mkdtemp(dir))
    {
        fprintf(stderr, "FAIL: mkdtemp: %s\n", strerror(errno));
        g_fail++;
        return;
    }
    snprintf(link, sizeof(link), "%s/usb-FTDI_FT232R_USB_UART_TEST0001-if00-port0",
             dir);
    if (make_pty(&master, name, sizeof(name)) != 0 || symlink(name, link) != 0)
    {
        fprintf(stderr, "FAIL: replug setup: %s\n", strerror(errno));
        g_fail++;
        rmdir(dir);
        return;
    }
    c = open_ctx(link, NULL, err, sizeof(err));
    CHECK(c != NULL, "open on a by-id style link");
    if (c)
    {
        run(c, master, 0.6, 1, &cycle);
        o = reading(c);
        CHECK(o != NULL, "online through the link");
        cJSON_Delete(o);

        /* Unplugged: the node and its link disappear. */
        close(master);
        master = -1;
        unlink(link);
        run(c, master, 0.3, 0, &cycle);
        CHECK(g_ops->get_reading(c, err, sizeof(err)) == MF_ERR_OFFLINE,
              "offline when unplugged");
        CHECK(g_ops->last_error(c) && strstr(g_ops->last_error(c), "adapter"),
              "says the adapter is gone");

        /* Plugged back in; the module retries every 2 s. */
        if (make_pty(&master, name, sizeof(name)) == 0 && symlink(name, link) == 0)
        {
            run(c, master, 2.8, 1, &cycle);
            o = reading(c);
            CHECK(o != NULL, "online again after replug");
            if (o)
            {
                cJSON *diag = cJSON_GetObjectItem(o, "diag");
                cJSON *port = diag ? cJSON_GetObjectItem(diag, "port") : NULL;

                CHECK(cJSON_IsString(port) && strcmp(port->valuestring, name) == 0,
                      "reopened the new node");
                cJSON_Delete(o);
            }
        }
        else
            CHECK(0, "replug: new pty");
        g_ops->close(c);
    }
    if (master >= 0)
        close(master);
    unlink(link);
    rmdir(dir);
}

/* Another process holds the port, as mf_magnum_dump does: the module
 * says so and does not read it. */
static void test_other_reader(void)
{
    char name[128], err[256];
    int master, ready[2], quit[2];
    pid_t pid;
    void *c;
    char b;

    if (make_pty(&master, name, sizeof(name)) != 0 || pipe(ready) != 0)
    {
        fprintf(stderr, "FAIL: other reader setup: %s\n", strerror(errno));
        g_fail++;
        return;
    }
    if (pipe(quit) != 0 || (pid = fork()) < 0)
    {
        fprintf(stderr, "FAIL: other reader setup: %s\n", strerror(errno));
        g_fail++;
        close(master);
        return;
    }
    if (pid == 0)
    {
        int fd = open(name, O_RDONLY | O_NOCTTY);

        if (fd >= 0)
        {
            (void)ioctl(fd, TIOCEXCL);
            (void)flock(fd, LOCK_EX);
        }
        if (write(ready[1], "x", 1) != 1 || read(quit[0], &b, 1) < 0)
            _exit(1);
        _exit(0);
    }
    if (read(ready[0], &b, 1) != 1)
        fprintf(stderr, "warn: no ready byte from the child\n");
    c = open_ctx(name, NULL, err, sizeof(err));
    CHECK(c != NULL, "open while another process holds the port");
    if (c)
    {
        CHECK(g_ops->step(c) == MF_STEP_ERROR && g_ops->last_error(c) &&
              strstr(g_ops->last_error(c), "in use by another reader"),
              "says another reader holds the port");
        CHECK(g_ops->fd(c) < 0, "does not read a held port");
        g_ops->close(c);
    }
    if (write(quit[1], "x", 1) != 1)
        fprintf(stderr, "warn: could not stop the child\n");
    (void)waitpid(pid, NULL, 0);
    close(ready[0]);
    close(ready[1]);
    close(quit[0]);
    close(quit[1]);
    close(master);
}

/* A removed module leaves the port as it found it: not exclusive, so the
 * next reader (mf_magnum_dump, a re-added module) can open it.  A pty keeps
 * its tty while the master is open, as TIOCEXCL would outlive a close. */
static void test_release(void)
{
    char name[128], err[256];
    int master, cycle = 0, fd;
    void *c;
    cJSON *o;

    if (make_pty(&master, name, sizeof(name)) != 0)
    {
        fprintf(stderr, "FAIL: openpty: %s\n", strerror(errno));
        g_fail++;
        return;
    }
    c = open_ctx(name, NULL, err, sizeof(err));
    CHECK(c != NULL, "open for the release test");
    if (c)
    {
        run(c, master, 0.5, 1, &cycle);
        o = reading(c);
        CHECK(o != NULL, "online before removal");
        cJSON_Delete(o);
        g_ops->close(c);
    }
    fd = open(name, O_RDONLY | O_NOCTTY | O_NONBLOCK);
    CHECK(fd >= 0, "port opens again after the module is removed");
    if (fd >= 0)
        close(fd);
    close(master);
}

/* Add and remove the module repeatedly (valgrind: nothing leaks). */
static void test_add_remove(int rounds)
{
    int i, cycle = 0;

    for (i = 0; i < rounds; i++)
    {
        char name[128], err[256];
        int master;
        void *c;

        if (make_pty(&master, name, sizeof(name)) != 0)
            break;
        c = open_ctx(name, NULL, err, sizeof(err));
        if (c)
        {
            g_ops->step(c);
            run(c, master, 0.25, 1, &cycle);
            g_ops->close(c);
        }
        close(master);
    }
}

int main(int argc, char **argv)
{
    void *dl;
    size_t (*entries)(const mf_plugin_ops_t **);
    int quick = argc > 2 && strcmp(argv[2], "quick") == 0;

    if (argc < 2)
    {
        fprintf(stderr, "usage: %s libmf_inverter_magnum.so [quick]\n", argv[0]);
        return 2;
    }
    setenv("MF_MAGNUM_STALE_S", "0.5", 1);
    dl = dlopen(argv[1], RTLD_NOW);
    if (!dl)
    {
        fprintf(stderr, "FAIL: dlopen %s: %s\n", argv[1], dlerror());
        return 1;
    }
    *(void **)&entries = dlsym(dl, "mf_plugin_entries");
    if (!entries || entries(&g_ops) != 1)
    {
        fprintf(stderr, "FAIL: mf_plugin_entries\n");
        return 1;
    }
    CHECK(strcmp(g_ops->kind, "inverter") == 0 && strcmp(g_ops->driver, "magnum") == 0,
          "inverter/magnum");
    CHECK(g_ops->caps(NULL) & MF_CAP_ACTION_REFRESH, "caps(NULL)");
    test_config_rules();
    test_bus();
    test_replug();
    test_other_reader();
    test_release();
    test_add_remove(quick ? 3 : 10);
    dlclose(dl);
    if (g_fail)
    {
        fprintf(stderr, "%d failure(s)\n", g_fail);
        return 1;
    }
    printf("test_magnum_pty: ok\n");
    return 0;
}
