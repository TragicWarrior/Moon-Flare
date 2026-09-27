#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "mf_fd.h"

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Beyond this the close() loop would be slow; a daemon never gets near it. */
#define FD_LIMIT_CAP 65536

int mf_fd_limit(void)
{
    struct rlimit rl;

    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY &&
        rl.rlim_cur < FD_LIMIT_CAP)
        return (int)rl.rlim_cur;
    return FD_LIMIT_CAP;
}

void mf_close_from(int lo, int limit)
{
    int fd;

#ifdef SYS_close_range
    /* Linux 5.9+.  An older kernel says ENOSYS: fall through. */
    if (syscall(SYS_close_range, (unsigned int)lo, ~0U, 0U) == 0)
        return;
#endif
    for (fd = lo; fd < limit; fd++)
        (void)close(fd);
}
