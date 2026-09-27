/*
 * Descriptors kept out of helpers (src/util/mf_fd.c), and the XD port
 * opened close-on-exec (src/xd/bms_proto.c): mf_gatt, started by the JK
 * plugin, held the XD's /dev/ttyUSB0 on batteryman.
 */
#define _GNU_SOURCE

#include "bms_proto.h"
#include "mf_fd.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_fail;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); g_fail++; } \
} while (0)

static int is_open(int fd)
{
    return fcntl(fd, F_GETFD) != -1 || errno != EBADF;
}

/* A child that closes from 3 up keeps 0-2 and none of the rest, with or
 * without close-on-exec. */
static void test_close_from(void)
{
    int p[2], a, b, limit, status = -1;
    pid_t pid;

    a = open("/dev/null", O_RDONLY);                 /* no close-on-exec */
    b = open("/dev/null", O_RDONLY | O_CLOEXEC);
    CHECK(a >= 3 && b >= 3 && pipe(p) == 0, "set up descriptors");
    limit = mf_fd_limit();
    CHECK(limit > p[1], "the limit covers them");
    pid = fork();
    if (pid == 0)
    {
        mf_close_from(3, limit);
        _exit(is_open(a) || is_open(b) || is_open(p[0]) || is_open(p[1]) ? 1 :
              !is_open(0) || !is_open(1) || !is_open(2) ? 2 : 0);
    }
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid, "child ran");
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "the child has only stdin, stdout and stderr");
    CHECK(is_open(a) && is_open(p[0]), "the parent keeps its own");
    close(a);
    close(b);
    close(p[0]);
    close(p[1]);
}

/* The XD opens its port close-on-exec. */
static void test_xd_cloexec(void)
{
    int m = posix_openpt(O_RDWR | O_NOCTTY), fd;
    const char *slave;

    CHECK(m >= 0 && grantpt(m) == 0 && unlockpt(m) == 0, "a pty");
    slave = m >= 0 ? ptsname(m) : NULL;
    CHECK(slave != NULL, "its slave");
    if (!slave)
        return;
    fd = bms_serial_open(slave, 9600);
    CHECK(fd >= 0, "bms_serial_open");
    CHECK(fd >= 0 && (fcntl(fd, F_GETFD) & FD_CLOEXEC), "blocking open is close-on-exec");
    if (fd >= 0)
        close(fd);
    fd = bms_serial_open_nonblock(slave, 9600);
    CHECK(fd >= 0 && (fcntl(fd, F_GETFD) & FD_CLOEXEC), "non-blocking open is close-on-exec");
    if (fd >= 0)
        close(fd);
    close(m);
}

int main(void)
{
    test_close_from();
    test_xd_cloexec();
    if (g_fail)
    {
        fprintf(stderr, "%d failure(s)\n", g_fail);
        return 1;
    }
    printf("test_fd: ok\n");
    return 0;
}
