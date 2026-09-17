/*
 * glibc execinfo backtrace on fatal signals.
 * Default on. Later: cmake -DMF_BACKTRACE=0
 */
#ifndef MF_BACKTRACE
#define MF_BACKTRACE 1
#endif

#include "debug/mf_backtrace.h"

#if MF_BACKTRACE

#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#define BT_DEPTH 64
#define BT_PATH  "/tmp/moonflare.backtrace"

static void crash_handler(int sig)
{
    void *frames[BT_DEPTH];
    int n, fd;
    char hdr[80];
    int hlen;

    hlen = snprintf(hdr, sizeof(hdr), "moonflare signal %d\n", sig);
    if (hlen < 0)
        hlen = 0;
    if (hlen > 0) {
        ssize_t wr = write(STDERR_FILENO, hdr, (size_t)hlen);
        (void)wr;
    }

    n = backtrace(frames, BT_DEPTH);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);

    fd = open(BT_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        if (hlen > 0) {
            ssize_t wr = write(fd, hdr, (size_t)hlen);
            (void)wr;
        }
        backtrace_symbols_fd(frames, n, fd);
        (void)close(fd);
    }

    signal(sig, SIG_DFL);
    raise(sig);
}

void mf_backtrace_install(void)
{
    struct sigaction sa;
    int sigs[] = { SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE };
    size_t i;

    sa.sa_handler = crash_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = (int)SA_RESETHAND;
    for (i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
        (void)sigaction(sigs[i], &sa, NULL);
}

#else

void mf_backtrace_install(void)
{
}

#endif
